/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <hwconfig.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#endif
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "rtx/OpMode_DMR_RX.hpp"
#ifdef CONFIG_DMR_TX
#include <stdarg.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "core/state.h"
#include "protocols/DMR/dmr_tx.h"
#include "opendmr/dmr_tx_enc.h"
#endif

#define DMR_IN_RATE 48000
/* 30 ms per half buffer: one burst period */
#define HALF_SAMPLES 1440

/* Received samples, or in transmission the baseband sent */
static int16_t baseband[2 * HALF_SAMPLES];
static float samples[HALF_SAMPLES];

#ifdef CONFIG_DMR_TX
/*
 * Transmission as a mobile station in direct mode (simplex, to another
 * radio or a hotspot): a burst every 60 ms (132 symbols, 27.5 ms), the
 * carrier on throughout (the PA power is a filtered PWM voltage, too slow
 * to ramp in the 1.25 ms guard of DMR). Two voice LC headers, the voice
 * superframes while the PTT is held, a terminator.
 *
 * Level: M17 gives +-2.4 kHz for symbols +-3 on the C62 with its gain of
 * 23000 on a filter of DC gain 0.3165 per symbol; DMR wants +-1.944 kHz.
 * The C62 transmitter inverts, as for M17: Settings > DMR > TX Polarity
 * flips that default.
 */
#define DMR_TX_GAIN (23000.0f * 0.3165f * 1944.0f / 2400.0f)
#define DMR_TX_INVERT true
#define DMR_TX_HEADERS 2
#define HALF_SYMS (HALF_SAMPLES / DMR_TX_SPS) /* 144: 30 ms */
#define FRAME_SYMS (2 * HALF_SYMS)            /* 288: 60 ms */
/* After the terminator, carrier on while the DSP's queue (some 180 ms of
 * audio) still plays */
#define TAIL_SYMS (450 * 48 / 10) /* 450 ms */
/* Never on air longer than this after the PTT is released */
#define TX_END_MAX_MS 1000

/* AMBE+2 silence, 72 bits in DMR order (MMDVMHost): when the encoder has no
 * frames ready */
static const uint8_t ambeSilence[DMR_TX_AMBE_BYTES] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

static dmr_tx_t txCall;
static dmr_mod_t txMod;
static pathId txPath = -1;
static streamId txStream = -1;
static stream_sample_t *txIdle;
static int64_t txHalf;       /* Stream half being written, from 1 */
static bool txEnding;        /* PTT released: the terminator is next */
static long long txEndingAt;
static bool txTermSent;
static int64_t txTermEnd;    /* Symbol where the tail ends */
static unsigned txHeaders;
static uint32_t txVoice, txSilent, txStart;
static int txPrevPrio;
static bool txNoIdWarned;
static uint8_t txBurst[DMR_TX_BURST_DIBITS];
static int64_t txBurstSym = -1; /* Stream symbol of txBurst's first */
static int64_t txNextSym = -1;  /* Of the next burst, -1 none */

/* C62 audio driver (csk6_codec.h) */
extern "C" {
bool csk6codec_rtxInputBoundary(int half, uint64_t *index);
bool csk6codec_captureTime(uint64_t index, int64_t *us);
uint64_t csk6codec_captureCount(void);
uint32_t csk6codec_captureStarts(void);
bool csk6codec_rtxOutputTime(uint32_t sample, int64_t *us);
uint32_t csk6codec_playLatencyMs(void);
bool csk6codec_echoArm(uint32_t samples);
bool csk6codec_echoResult(uint64_t *index, int32_t *peak);
}

/*
 * Where the transmission leaves the DAC, measured on every transmission
 * for the repeater mode, which needs it within a millisecond or two:
 *  - the estimate (csk6codec_rtxOutputTime(): the DSP's queue and the
 *    latency it states): how long after a half is written it leaves,
 *    taken once the DSP settled;
 *  - with C62_DAC_ECHO, the truth: a single +3 symbol written before the
 *    first burst and found in the DSP's echo of the DAC, against the
 *    estimate for the same symbol.
 */
#define RRC_DELAY ((DMR_TX_RRC_TAPS - 1) / 2) /* Symbol to its pulse peak */
#define PROBE_SYM (HALF_SYMS + HALF_SYMS / 2)  /* In the first half written */
#define ECHO_WINDOW 28800                      /* 600 ms to find it */
#define ECHO_MIN 1000                          /* Weakest echo accepted */
#define SETTLE_HALVES 27                       /* 0.8 s: the DSP settled */
static bool txEchoArmed, txEchoDone;
static uint64_t txEchoIdx;
static int32_t txEchoPeak;
/*
 * The offset between the transmitter's stream and the capture: the capture
 * sample at which stream sample s leaves the DSP, minus s, by the estimate.
 * Both count the codec's clock, so it should not move within a
 * transmission; it is what places a burst in a repeater's slot, measured in
 * capture samples. Taken on every half once the DSP settled: the first,
 * lowest and highest. Also how long the first half then written took to
 * leave (the DSP's queue, which changes with each start).
 */
static bool txOffOk;
static int64_t txOffFirst, txOffMin, txOffMax;
static uint32_t txOffHalves;
static int64_t txLatUs;
/* How they went over the transmission: every PROFILE_EVERY halves, the
 * offset from the first and how long the half then written took to leave
 * (a straight rise is a rate, a rise that levels off the time base
 * settling) */
#define PROFILE_EVERY 8
#define PROFILE_POINTS 24
static int16_t txProfOff[PROFILE_POINTS];
static uint8_t txProfLatMs[PROFILE_POINTS];
static unsigned txProfN;

/* The estimate's capture sample for an uptime, from the capture now */
static bool capSampleAt(int64_t us, int64_t *sample)
{
    const uint64_t cnow = csk6codec_captureCount();
    int64_t capUs;
    if (!csk6codec_captureTime(cnow, &capUs))
        return false;
    *sample = (int64_t)cnow + (us - capUs) * 48 / 1000;
    return true;
}
/* The microphone opened before reception closed (see update()), and the
 * capture's starts when the PTT was taken: the capture must not stop and
 * restart around a transmission, its sample count would skip the gap */
static bool txMicEarly;
static uint32_t txCapStarts;

static uint64_t txEchoArmIdx; /* Capture sample when the search began */
/* The capture's pace during the transmission: its count and the time when
 * the transmission started */
static uint64_t txCapStart;
static int64_t txCapStartUs;

static int64_t uptimeUs(void)
{
    return (int64_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

/*
 * The transmission's console lines, printed a while after it ends: the
 * DSP writes to the same serial line when its audio starts and stops, and
 * the two get mixed character by character.
 */
#define TX_LOG_DELAY_MS 800
static char txLog[1024];
static size_t txLogLen;
static long long txLogAt; /* 0: nothing to print yet */

static void txLogf(const char *fmt, ...)
{
    if (txLogLen >= sizeof(txLog) - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(txLog + txLogLen, sizeof(txLog) - txLogLen, fmt,
                            ap);
    va_end(ap);
    if (n > 0)
        txLogLen += (size_t)n;
    if (txLogLen > sizeof(txLog) - 1)
        txLogLen = sizeof(txLog) - 1;
}

/*
 * The lines of the last transmissions, kept and printed again: with the
 * programming cable plugged in the microphone does not work, so the tests
 * with speech are made without it and their lines read once it is back.
 * Every 30 s, one transmission a second, oldest first. In PSRAM, cleared
 * at enable().
 */
#define TX_HISTORY 8
#define TX_HISTORY_EVERY_MS 30000
static char txHistory[TX_HISTORY][sizeof(txLog)]
    __attribute__((section(".psram_section")));
static uint32_t txHistorySeq[TX_HISTORY];
static uint32_t txSeq;           /* Transmissions so far */
static int txHistoryNext = -1;   /* Being printed, -1 none */
static long long txHistoryAt;    /* Next one */

static void txHistoryClear(void)
{
    memset(txHistory, 0, sizeof(txHistory));
    memset(txHistorySeq, 0, sizeof(txHistorySeq));
    txSeq = 0;
    txHistoryNext = -1;
    txHistoryAt = getTick() + TX_HISTORY_EVERY_MS;
}

static void txHistoryPrint(void)
{
    const long long now = getTick();
    if ((txSeq == 0) || (now < txHistoryAt))
        return;
    if (txHistoryNext < 0) {
        printk("[DMR TX history] the last %lu transmissions:\n",
               (unsigned long)(txSeq < TX_HISTORY ? txSeq : TX_HISTORY));
        txHistoryNext = (txSeq <= TX_HISTORY) ? 0 : (int)(txSeq % TX_HISTORY);
        txHistoryAt = now + 1000;
        return;
    }
    const int i = txHistoryNext;
    printk("[DMR TX history] #%lu\n%s", (unsigned long)txHistorySeq[i],
           txHistory[i]);
    txHistoryNext = (i + 1) % TX_HISTORY;
    const int last = (int)((txSeq - 1) % TX_HISTORY);
    if (i == last) {
        txHistoryNext = -1;
        txHistoryAt = now + TX_HISTORY_EVERY_MS;
    } else {
        txHistoryAt = now + 1000;
    }
}

/* Print the lines of the last transmission, once their time came, and keep
 * them */
static void txLogFlush(void)
{
    if ((txLogAt != 0) && (getTick() >= txLogAt)) {
        printk("%s", txLog);
        const int i = (int)(txSeq % TX_HISTORY);
        memcpy(txHistory[i], txLog, sizeof(txLog));
        txHistorySeq[i] = ++txSeq;
        txLogLen = 0;
        txLog[0] = 0;
        txLogAt = 0;
        return;
    }
    if (txLogAt == 0)
        txHistoryPrint();
}

/*
 * Repeater timing, from reception, for transmitting in its timeslots: the
 * capture sample (csk6_codec's 48 kHz count) at the centre of symbol 0 of
 * its TS1 bursts; TS2 ones count 30 ms earlier. A burst more than 5 ms off
 * the reference is from the other slot (CACH misread) and dropped. The
 * reference is also kept as a time: the capture stops and restarts around
 * a transmission, and its count does not include the gap.
 */
#define FRAME_SAMPLES (2 * HALF_SAMPLES) /* 60 ms */
#define RPT_SLOT_TOL 240                 /* 5 ms */
#define RPT_STALE_MS 2000                /* Heard this recently */
#define RPT_LOG_MS 5000
static int64_t rxCapOffset; /* Capture sample minus demodulator sample */
static bool rxMapOk;
static uint32_t rptSeq[2];
static uint64_t rptTs1Cap;
static bool rptTs1Ok;
static int64_t rptTs1Us;
static long long rptTs1Ms; /* When the reference was last confirmed */
static uint32_t rptAccepted, rptRejected;
/* A run of TS1 bursts in step, for the repeater's period */
static uint64_t rptRunFirst, rptRunLast;
static long long rptRunLastMs;
static bool rptRun;
static long long rptNextLog;

static void rptPeriod(uint64_t cap)
{
    const long long now = getTick();
    if (!rptRun || (now - rptRunLastMs > RPT_STALE_MS) || (cap <= rptRunLast)) {
        rptRunFirst = rptRunLast = cap;
        rptRun = true;
    } else {
        rptRunLast = cap;
    }
    rptRunLastMs = now;
}

/* The repeater's 60 ms frame against 2880 capture samples, ppm */
static bool rptPpm(int32_t *ppm, uint32_t *frames)
{
    if (!rptRun)
        return false;
    const uint64_t span = rptRunLast - rptRunFirst;
    const uint32_t k = (uint32_t)((span + FRAME_SAMPLES / 2) / FRAME_SAMPLES);
    if (k < 50)
        return false;
    *ppm = (int32_t)(((int64_t)span - (int64_t)k * FRAME_SAMPLES) * 1000000
                     / ((int64_t)k * FRAME_SAMPLES));
    *frames = k;
    return true;
}

/* A new channel, or the demodulator reset: forget the repeater */
static void rptReset(void)
{
    rptSeq[0] = rptSeq[1] = 0;
    rptTs1Ok = false;
    rptRun = false;
    rptAccepted = rptRejected = 0;
}

/* The repeater bursts the demodulator found since the last block */
static void rptTiming(void)
{
    if (!rxMapOk)
        return;
    const long long now = getTick();
    for (int slot = 0; slot < 2; slot++) {
        uint32_t sample, seq;
        if (!dmr_demod_slot_start(slot, &sample, &seq) || (seq == rptSeq[slot]))
            continue;
        rptSeq[slot] = seq;
        const int64_t cap = (int64_t)sample + rxCapOffset;
        const uint64_t ts1 = (uint64_t)(cap - (slot ? FRAME_SAMPLES / 2 : 0));
        if (!rptTs1Ok || (now - rptTs1Ms > RPT_STALE_MS)) {
            rptTs1Ok = true; /* A new reference */
        } else {
            int64_t d = ((int64_t)ts1 - (int64_t)rptTs1Cap) % FRAME_SAMPLES;
            if (d > FRAME_SAMPLES / 2)
                d -= FRAME_SAMPLES;
            if (d < -FRAME_SAMPLES / 2)
                d += FRAME_SAMPLES;
            if ((d < -RPT_SLOT_TOL) || (d > RPT_SLOT_TOL)) {
                rptRejected++;
                continue;
            }
            if (slot == 0)
                rptPeriod(ts1);
        }
        rptTs1Cap = ts1;
        rptAccepted++;
        rptTs1Ms = now;
        csk6codec_captureTime(rptTs1Cap, &rptTs1Us);
    }

    if (now < rptNextLog)
        return;
    rptNextLog = now + RPT_LOG_MS;
    if ((rptAccepted == 0) && (rptRejected == 0))
        return;
    int32_t ppm = 0;
    uint32_t frames = 0;
    const bool havePpm = rptPpm(&ppm, &frames);
    const uint32_t us = (uint32_t)((rptTs1Cap % FRAME_SAMPLES) * 125u / 6u);
    printk("[DMR RPT] TS1 at %lu.%03lu ms of the 60 ms cycle (%lu bursts, "
           "%lu of the other slot dropped), period %+ld ppm (%lu frames), "
           "uptime %lld us\n", (unsigned long)(us / 1000),
           (unsigned long)(us % 1000), (unsigned long)rptAccepted,
           (unsigned long)rptRejected, (long)(havePpm ? ppm : 0),
           (unsigned long)frames, (long long)rptTs1Us);
    rptAccepted = rptRejected = 0;
}
#endif

/**
 * Call events (start, end, IDs, alias) on the console.
 */
static void printLine(const char *line)
{
#ifdef __ZEPHYR__
    printk("DMR %s\n", line); /* Unbuffered, safe from any thread */
#else
    printf("DMR %s\n", line);
#endif
}

OpMode_DMR_RX::OpMode_DMR_RX()
    : startRx(false), startTx(false), dataErrors(0), basebandPath(-1),
      basebandId(-1)
{
}

#ifdef CONFIG_DMR_TX
bool OpMode_DMR_RX::txWanted(const rtxStatus_t *rtx)
{
    if (!platform_getPttStatus() || rtx->txDisable)
        return false;
    if (state.dmr_settings.id == 0) {
        if (!txNoIdWarned)
            printk("[DMR TX] no DMR ID: set it in Settings > DMR\n");
        txNoIdWarned = true;
        return false;
    }
    txNoIdWarned = false;
    return true;
}

void OpMode_DMR_RX::txEnd(rtxStatus_t *rtx)
{
    if (txStream >= 0)
        audioStream_stop(txStream); /* Plays what is queued, then stops */
    if (txPath >= 0)
        audioPath_release(txPath);
    txStream = -1;
    txPath = -1;
    /* Reception back before the microphone closes: the capture goes on */
    startSampling();
    dmrTxEnc_stop();
    txMicEarly = false;

    uint32_t bursts, maxUs, dropped;
    dmrTxEnc_stats(&bursts, &maxUs, &dropped);
    txLogf("[DMR TX] end: %lu ms, %lu voice bursts (%lu without audio in "
           "time), encoder %lu bursts, max %lu us, dropped %lu\n",
           (unsigned long)((uint32_t)getTick() - txStart),
           (unsigned long)txVoice, (unsigned long)txSilent,
           (unsigned long)bursts, (unsigned long)maxUs,
           (unsigned long)dropped);

    /* The DAC timing of this transmission */
    if (txOffOk)
        txLogf("[DMR TX] DAC: stream to capture offset %lld samples "
               "(estimate), over %lu more halves from %+lld to %+lld; the "
               "first half then left %lld.%03lld ms after written (the DSP "
               "states %lu ms)\n", (long long)txOffFirst,
               (unsigned long)txOffHalves,
               (long long)(txOffMin - txOffFirst),
               (long long)(txOffMax - txOffFirst),
               (long long)(txLatUs / 1000), (long long)(txLatUs % 1000),
               (unsigned long)csk6codec_playLatencyMs());
    else
        txLogf("[DMR TX] DAC: no estimate (transmission too short)\n");
    if (txOffOk && (txProfN > 0)) {
        txLogf("[DMR TX] DAC profile, every %u halves (samples/ms):",
               PROFILE_EVERY);
        for (unsigned i = 0; i < txProfN; i++)
            txLogf(" %+d/%u", txProfOff[i], txProfLatMs[i]);
        txLogf("\n");
    }
    /* The probe in capture samples: where the echo saw it and where the
     * estimate puts it (its output time, as a capture sample now) */
    if (txEchoDone && (txEchoPeak >= ECHO_MIN)) {
        const int64_t probe = PROBE_SYM * DMR_TX_SPS + RRC_DELAY;
        const int64_t echoRel = (int64_t)(txEchoIdx - txEchoArmIdx);
        int64_t estUs, estIdx;
        if (csk6codec_rtxOutputTime((uint32_t)probe, &estUs)
            && capSampleAt(estUs, &estIdx)) {
            const int64_t d = estIdx - (int64_t)txEchoIdx;
            txLogf("[DMR TX] DAC echo: peak %ld, %lld samples after the "
                   "search began; offset by the echo %lld, by the estimate "
                   "%lld; estimate minus echo %lld samples (%lld us)\n",
                   (long)txEchoPeak, (long long)echoRel,
                   (long long)((int64_t)txEchoIdx - probe),
                   (long long)(estIdx - probe), (long long)d,
                   (long long)(d * 125 / 6));
        } else {
            txLogf("[DMR TX] DAC echo: peak %ld, %lld samples after the "
                   "search began; no time base for the estimate\n",
                   (long)txEchoPeak, (long long)echoRel);
        }
    } else if (txEchoArmed || txEchoDone) {
        txLogf("[DMR TX] DAC echo: not found (peak %ld)\n",
               (long)txEchoPeak);
    }
    /* Samples the capture counted against the time that passed: 48 kHz if
     * it kept up (the microphone and the radio input share it) */
    {
        const uint64_t c = csk6codec_captureCount() - txCapStart;
        const int64_t us = uptimeUs() - txCapStartUs;
        if (us > 0)
            txLogf("[DMR TX] capture: %llu samples in %lld ms, %lld "
                   "samples/s; restarted %lu times since the PTT\n",
                   (unsigned long long)c, (long long)(us / 1000),
                   (long long)(c * 1000000 / us),
                   (unsigned long)(csk6codec_captureStarts() - txCapStarts));
    }
    txLogAt = getTick() + TX_LOG_DELAY_MS;
    txEchoArmed = false;

    k_thread_priority_set(k_current_get(), txPrevPrio);
    rtx->opStatus = OFF;
    startRx = true;
}

/*
 * One half buffer (30 ms, 144 symbols) of transmission each call. Bursts
 * are placed by symbol, every 288 from the second half written.
 */
void OpMode_DMR_RX::txUpdate(rtxStatus_t *rtx)
{
    const dmrSettings_t *dmr = &state.dmr_settings;

    if (startTx) {
        startTx = false;
        txPrevPrio = k_thread_priority_get(k_current_get());
        dmr_tx_setup(&txCall, dmr->id, dmr->tg, !dmr->private_call, dmr->cc);
        dmr_mod_init(&txMod, DMR_TX_GAIN,
                     dmr->tx_invert ? !DMR_TX_INVERT : DMR_TX_INVERT);
        memset(baseband, 0, sizeof(baseband));
        if (!txMicEarly)
            txCapStarts = csk6codec_captureStarts();
        txMicEarly = false;
        if (!dmrTxEnc_start()) {
            txLogf("[DMR TX] the microphone could not be opened\n");
            txLogAt = getTick() + TX_LOG_DELAY_MS;
            rtx->opStatus = OFF;
            startRx = true;
            return;
        }
        /* The probe's echo: armed before the stream starts */
        txEchoArmIdx = csk6codec_captureCount();
        txCapStart = txEchoArmIdx;
        txCapStartUs = uptimeUs();
        txEchoArmed = csk6codec_echoArm(ECHO_WINDOW);
        txEchoDone = false;
        txEchoPeak = -1;
        txOffOk = false;
        txOffHalves = 0;
        txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
        if (txPath >= 0)
            txStream = audioStream_start(txPath, baseband, 2 * HALF_SAMPLES,
                                         DMR_IN_RATE,
                                         STREAM_OUTPUT | BUF_CIRC_DOUBLE);
        if (txStream < 0) {
            txLogf("[DMR TX] the output to the transmitter could not be "
                   "opened\n");
            txEnd(rtx);
            return;
        }
        txIdle = outputStream_getIdleBuffer(txStream);
        txHalf = 1; /* The stream plays its first half while we fill this */
        /* Above the encoder thread (13): the transmitter must be fed every
         * 30 ms, while a burst of three frames takes it up to 35 ms */
        k_thread_priority_set(k_current_get(), 12);
        radio_enableTx();
        txEnding = false;
        txTermSent = false;
        txHeaders = DMR_TX_HEADERS;
        txVoice = txSilent = 0;
        txBurstSym = -1;
        txNextSym = 2 * HALF_SYMS;
        txStart = (uint32_t)getTick();
        txLogf("[DMR TX] start: ID %lu -> %s %lu, CC %u, %lu Hz, %lu mW, "
               "polarity %s\n", (unsigned long)dmr->id,
               dmr->private_call ? "ID" : "TG", (unsigned long)dmr->tg,
               (unsigned)dmr->cc, (unsigned long)rtx->txFrequency,
               (unsigned long)rtx->txPower,
               dmr->tx_invert ? "inverted" : "normal");
    }

    if (!txEnding && (!platform_getPttStatus() || rtx->txDisable)) {
        txEnding = true;
        txEndingAt = getTick();
    }
    if (txEnding && (getTick() - txEndingAt > TX_END_MAX_MS)) {
        txLogf("[DMR TX] safety stop: the transmission did not end\n");
        txEnd(rtx);
        return;
    }

    if (txEchoArmed && !txEchoDone
        && csk6codec_echoResult(&txEchoIdx, &txEchoPeak))
        txEchoDone = true;

    /* This half, symbol by symbol */
    const int64_t h0 = txHalf * HALF_SYMS;
    for (int s = 0; s < HALF_SYMS; s++) {
        const int64_t sym = h0 + s;
        if ((txNextSym >= 0) && (sym >= txNextSym)) {
            if (txHeaders > 0) {
                dmr_tx_header(&txCall, txBurst);
                txHeaders--;
            } else if (txEnding) {
                dmr_tx_terminator(&txCall, txBurst);
                txTermSent = true;
                txTermEnd = sym + DMR_TX_BURST_DIBITS + TAIL_SYMS;
            } else {
                uint8_t ambe[3][DMR_TX_AMBE_BYTES];
                if (!dmrTxEnc_pop(ambe)) {
                    for (int k = 0; k < 3; k++)
                        memcpy(ambe[k], ambeSilence, DMR_TX_AMBE_BYTES);
                    txSilent++;
                }
                dmr_tx_voice(&txCall, ambe, txBurst);
                txVoice++;
            }
            txBurstSym = sym;
            txNextSym = txTermSent ? -1 : sym + FRAME_SYMS;
        }
        static const uint8_t probe = 1; /* Dibit 01: +3 */
        const uint8_t *d = NULL;
        if (txEchoArmed && (sym == PROBE_SYM))
            d = &probe;
        else if ((txBurstSym >= 0) && (sym >= txBurstSym)
                 && (sym < txBurstSym + DMR_TX_BURST_DIBITS))
            d = &txBurst[sym - txBurstSym];
        dmr_mod_dibits(&txMod, d, 1, txIdle + s * DMR_TX_SPS);
    }
    outputStream_sync(txStream, true);
    /* Once settled: the stream to capture offset of the half just written */
    if (txHalf >= SETTLE_HALVES) {
        const int64_t written = uptimeUs();
        const int64_t first = txHalf * HALF_SAMPLES;
        int64_t out, cap;
        if (csk6codec_rtxOutputTime((uint32_t)first, &out)
            && capSampleAt(out, &cap)) {
            const int64_t off = cap - first;
            if (!txOffOk) {
                txOffFirst = txOffMin = txOffMax = off;
                txLatUs = out - written;
                txOffOk = true;
                txProfN = 0;
            } else {
                if (off < txOffMin)
                    txOffMin = off;
                if (off > txOffMax)
                    txOffMax = off;
                txOffHalves++;
            }
            if (((txOffHalves % PROFILE_EVERY) == 0)
                && (txProfN < PROFILE_POINTS)) {
                int64_t d = off - txOffFirst;
                if (d > INT16_MAX)
                    d = INT16_MAX;
                if (d < INT16_MIN)
                    d = INT16_MIN;
                int64_t lat = (out - written) / 1000;
                if (lat < 0)
                    lat = 0;
                if (lat > 255)
                    lat = 255;
                txProfOff[txProfN] = (int16_t)d;
                txProfLatMs[txProfN] = (uint8_t)lat;
                txProfN++;
            }
        }
    }
    txIdle = outputStream_getIdleBuffer(txStream);
    txHalf++;

    /* The terminator and its tail are queued */
    if (txTermSent && (h0 >= txTermEnd))
        txEnd(rtx);
}
#else
bool OpMode_DMR_RX::txWanted(const rtxStatus_t *)
{
    return false;
}

void OpMode_DMR_RX::txUpdate(rtxStatus_t *rtx)
{
    rtx->opStatus = OFF;
    startRx = true;
}

void OpMode_DMR_RX::txEnd(rtxStatus_t *)
{
}
#endif

OpMode_DMR_RX::~OpMode_DMR_RX()
{
    disable();
}

void OpMode_DMR_RX::enable()
{
    dmr_demod_init();
#ifdef CONFIG_DMR_TX
    rptReset();
    txHistoryClear();
#endif
    dmr_proto_set_printer(printLine);
    dmrSpeech_start();
    startRx = true;
}

void OpMode_DMR_RX::disable()
{
#ifdef CONFIG_DMR_TX
    if ((txStream >= 0) || (txPath >= 0)) {
        rtxStatus_t dummy;
        txEnd(&dummy);
    }
#endif
    startRx = false;
    startTx = false;
    stopSampling();
    dmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dmr_demod_reset();
}

void OpMode_DMR_RX::startSampling()
{
    if (basebandId >= 0)
        return; /* Already, see txEnd() */
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   DMR_IN_RATE, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DMR_RX::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_DMR_RX::update(rtxStatus_t *const rtx, const bool newCfg)
{
#ifdef CONFIG_DMR_TX
    if (rtx->opStatus != TX)
        txLogFlush();
#endif
    switch (rtx->opStatus) {
        case OFF:
            radio_disableRtx();
            if (txWanted(rtx)) {
                rtx->opStatus = TX;
                startTx = true;
                return;
            }
#ifdef CONFIG_DMR_TX
            /* The PTT went before the transmission started */
            if (txMicEarly) {
                dmrTxEnc_stop();
                txMicEarly = false;
            }
#endif
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

        case RX:
            break;

        case TX:
            txUpdate(rtx);
            return;

        default:
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

    /* PTT: stop receiving, OFF starts the transmission. The microphone opens
     * first, so that the capture they share does not stop */
    if (txWanted(rtx)) {
#ifdef CONFIG_DMR_TX
        txCapStarts = csk6codec_captureStarts();
        txMicEarly = dmrTxEnc_start();
#endif
        stopSampling();
        startRx = true;
        rtx->opStatus = OFF;
        return;
    }

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    /* A new frequency is a new channel: start from scratch */
    if (newCfg) {
        dmr_demod_reset();
#ifdef CONFIG_DMR_TX
        rptReset();
#endif
    }

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples. A failed read means we were
     * late and samples were lost; the framer finds the bursts again by
     * itself. Do not lose more by sleeping, unless the stream keeps failing.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0)
            dmr_demod_note_overrun();
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    /* The demodulator takes any scale: levels come from the sync words */
    size_t n = block.len;
    if (n > HALF_SAMPLES)
        n = HALF_SAMPLES;
    for (size_t i = 0; i < n; i++)
        samples[i] = (float)block.data[i];
#ifdef CONFIG_DMR_TX
    /* This block ends at the driver's last boundary of its half */
    {
        uint64_t boundary;
        const int half = (block.data == baseband) ? 0 : 1;
        if (csk6codec_rtxInputBoundary(half, &boundary) && (boundary >= n)) {
            rxCapOffset = (int64_t)(boundary - n) - (int64_t)dmr_demod_fed();
            rxMapOk = true;
        }
    }
#endif
    dmr_demod_feed_disc(samples, (int)n);
#ifdef CONFIG_DMR_TX
    rptTiming();
#endif

    dmr_demod_status_t ds;
    dmr_demod_get_status(&ds);
    if (ds.locked)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
