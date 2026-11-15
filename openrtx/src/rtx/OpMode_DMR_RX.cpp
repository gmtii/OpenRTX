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
    dmrTxEnc_stop();

    uint32_t bursts, maxUs, dropped;
    dmrTxEnc_stats(&bursts, &maxUs, &dropped);
    printk("[DMR TX] end: %lu ms, %lu voice bursts (%lu without audio in "
           "time), encoder %lu bursts, max %lu us, dropped %lu\n",
           (unsigned long)((uint32_t)getTick() - txStart),
           (unsigned long)txVoice, (unsigned long)txSilent,
           (unsigned long)bursts, (unsigned long)maxUs,
           (unsigned long)dropped);

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
        if (!dmrTxEnc_start()) {
            printk("[DMR TX] the microphone could not be opened\n");
            rtx->opStatus = OFF;
            startRx = true;
            return;
        }
        txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
        if (txPath >= 0)
            txStream = audioStream_start(txPath, baseband, 2 * HALF_SAMPLES,
                                         DMR_IN_RATE,
                                         STREAM_OUTPUT | BUF_CIRC_DOUBLE);
        if (txStream < 0) {
            printk("[DMR TX] the output to the transmitter could not be "
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
        printk("[DMR TX] start: ID %lu -> %s %lu, CC %u, %lu Hz, %lu mW, "
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
        printk("[DMR TX] safety stop: the transmission did not end\n");
        txEnd(rtx);
        return;
    }

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
        const uint8_t *d = NULL;
        if ((txBurstSym >= 0) && (sym >= txBurstSym)
            && (sym < txBurstSym + DMR_TX_BURST_DIBITS))
            d = &txBurst[sym - txBurstSym];
        dmr_mod_dibits(&txMod, d, 1, txIdle + s * DMR_TX_SPS);
    }
    outputStream_sync(txStream, true);
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
    switch (rtx->opStatus) {
        case OFF:
            radio_disableRtx();
            if (txWanted(rtx)) {
                rtx->opStatus = TX;
                startTx = true;
                return;
            }
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

    /* PTT: stop receiving, OFF starts the transmission */
    if (txWanted(rtx)) {
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
    if (newCfg)
        dmr_demod_reset();

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
    dmr_demod_feed_disc(samples, (int)n);

    dmr_demod_status_t ds;
    dmr_demod_get_status(&ds);
    if (ds.locked)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
