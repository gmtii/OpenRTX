/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * NXDN48 receiver.
 *
 * The demodulator is that of a System Fusion receiver (after OE3ANC
 * oe3anc-ht): RRC beta 0.2 matched filter at 5 samples per symbol (here
 * 12 kHz for 2400 baud), sync fitted by least squares for level, offset and
 * polarity, Gardner timing, and a flywheel from one good frame to the next.
 *
 * The frame layout, scrambler, LICH, SACCH and FACCH1 coding, Layer 3
 * fields and the voice channel bit positions follow MMDVMHost (NXDNControl,
 * NXDNLICH, NXDNSACCH, NXDNFACCH1, NXDNConvolution, NXDNCRC, NXDNLayer3,
 * NXDNAudio), Copyright (C) 2015-2025 Jonathan Naylor G4KLX, GPL-2.0 or
 * later. The LICH parity and the VCALL cipher field follow dsd-neo
 * (arancormonk, GPL-3.0 or later).
 */

#include "protocols/NXDN/nxdn_rx.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define NXDN_PRINT printk
#else
#define NXDN_PRINT printf
#endif
#include "dmr_port.h"
#include "dmr_voice.h"

/* ---- Demodulator: 12 kHz, 5 samples per symbol -------------------------- */

static const float rxTaps[41] = {
    0.0273581115f,  0.0070635427f,  -0.0231679822f, -0.0498223859f,
    -0.0577750618f, -0.0377312525f, 0.0076172472f,  0.0620248049f,
    0.1004128792f,  0.0989075145f,  0.0465864194f,  -0.0460535952f,
    -0.1462389372f, -0.2073460175f, -0.1845738160f, -0.0525461921f,
    0.1818755267f,  0.4770672269f,  0.7663595022f,  0.9773862095f,
    1.0546479089f,  0.9773862095f,  0.7663595022f,  0.4770672269f,
    0.1818755267f,  -0.0525461921f, -0.1845738160f, -0.2073460175f,
    -0.1462389372f, -0.0460535952f, 0.0465864194f,  0.0989075145f,
    0.1004128792f,  0.0620248049f,  0.0076172472f,  -0.0377312525f,
    -0.0577750618f, -0.0498223859f, -0.0231679822f, 0.0070635427f,
    0.0273581115f
};

/* NXDN mapping: dibit 00 = +1, 01 = +3, 10 = -1, 11 = -3 */
static const int levels[4] = { 1, 3, -1, -3 };
/* Frame sync word: 20 bits, ten symbols */
static const uint8_t fsw[3] = { 0xcd, 0xf5, 0x90 };

#define FRAME_BYTES 48
#define FRAME_SYMBOLS 192
#define FSW_SYMBOLS 10
#define HISTORY 100
#define DECIMATION 4
/* Smallest symbol step (level of +-1) taken as a signal */
#define MIN_STEP 30.0f
/* Largest sync fit residual taken as a sync: tighter than Fusion's 0.4,
 * the NXDN sync is half as long */
#define MAX_SYNC_ERROR 0.3f

/* Filter and history written twice, at i and i + length: contiguous
 * windows, no modulo in the per sample loops */
struct demod {
    float filter[2 * 41];
    unsigned filterPos;
    float history[2 * HISTORY];
    unsigned historyPos;
    uint32_t samples;
    bool receiving;
    unsigned pending;
    float bestError;
    uint32_t bestTime;
    float centre;
    float step;
    float invStep;
    float nextSymbol;
    float previousSymbol;
    unsigned symbols;
    uint8_t frame[FRAME_BYTES];
    int32_t decimSum;
    unsigned decimCount;
    bool flywheel;
    /* The frame was received by flywheel: its sync word was demodulated,
     * not found, and fswErrors counts its wrong bits. The LICH has a
     * single parity bit: this check keeps the flywheel from running on
     * noise after a transmission */
    bool fromFlywheel;
    unsigned fswErrors;
};

static struct demod dm;

static struct {
    int64_t sum;
    int64_t sumSq;
    uint32_t n;
    float bestError;
    float bestStep;
    uint32_t syncs;
    int rssiMax;
    uint32_t blocks;
    uint32_t feedUs;
    uint32_t feedMaxUs;
} diag;

static void diagReset(void)
{
    memset(&diag, 0, sizeof(diag));
    diag.bestError = 99.0f;
    diag.rssiMax = -200;
}

static int syncLevel(unsigned i)
{
    return levels[(fsw[i / 4] >> (6 - 2 * (i % 4))) & 3];
}

static float syncX[FSW_SYMBOLS];
static float syncSumX, syncDenominator;

static void syncInit(void)
{
    float sumXX = 0.0f;
    syncSumX = 0.0f;
    for (unsigned i = 0; i < FSW_SYMBOLS; i++) {
        syncX[i] = (float)syncLevel(i);
        syncSumX += syncX[i];
        sumXX += syncX[i] * syncX[i];
    }
    syncDenominator = sumXX - syncSumX * syncSumX / (float)FSW_SYMBOLS;
}

static float sampleAt(unsigned current, float time)
{
    const unsigned whole = (unsigned)time;
    const float fraction = time - (float)whole;
    const unsigned back = dm.samples - whole;
    const float before = dm.history[current + HISTORY - back];
    const float after = dm.history[current + HISTORY + 1 - back];
    return ((before + (after - before) * fraction) - dm.centre) * dm.invStep;
}

/**
 * One 12 kHz sample.
 *
 * @return true when dm.frame holds a complete frame.
 */
static bool demodSample(float input)
{
    dm.filterPos = dm.filterPos ? dm.filterPos - 1 : 40;
    dm.filter[dm.filterPos] = input;
    dm.filter[dm.filterPos + 41] = input;
    const float *window = &dm.filter[dm.filterPos];
    float value = 0.0f;
    for (unsigned i = 0; i < 41; i++)
        value += window[i] * rxTaps[i];
    value *= 0.2f;

    dm.history[dm.historyPos] = value;
    dm.history[dm.historyPos + HISTORY] = value;
    const unsigned current = dm.historyPos;
    dm.historyPos = dm.historyPos + 1 == HISTORY ? 0 : dm.historyPos + 1;
    dm.samples++;
    if (dm.samples > 10000 && !dm.receiving && !dm.pending && !dm.flywheel)
        dm.samples = 140;
    else if (dm.samples > 100000 && !dm.pending) {
        dm.samples -= 90000;
        dm.nextSymbol -= 90000.0f;
    }

    if (dm.receiving) {
        if ((float)dm.samples < dm.nextSymbol + 1.0f)
            return false;
        const float normalized = sampleAt(current, dm.nextSymbol);
        const float middle = sampleAt(current, dm.nextSymbol - 2.5f);
        const unsigned dibit = normalized >= 0.0f
                                 ? (normalized > 2.0f ? 1u : 0u)
                                 : (normalized < -2.0f ? 3u : 2u);
        dm.frame[dm.symbols / 4] |=
            (uint8_t)(dibit << (6 - 2 * (dm.symbols % 4)));
        dm.symbols++;
        float timing = middle * (dm.previousSymbol - normalized) / 9.0f;
        if (timing < -1.0f)
            timing = -1.0f;
        else if (timing > 1.0f)
            timing = 1.0f;
        dm.nextSymbol += 5.0f + 0.15f * timing;
        dm.previousSymbol = normalized;
        if (dm.symbols == FRAME_SYMBOLS) {
            dm.receiving = false;
            if (dm.fromFlywheel) {
                dm.fswErrors = 0;
                for (unsigned i = 0; i < 3; i++) {
                    uint8_t x = (uint8_t)(dm.frame[i] ^ fsw[i]);
                    if (i == 2)
                        x &= 0xf0;
                    while (x) {
                        dm.fswErrors += x & 1u;
                        x >>= 1;
                    }
                }
                memcpy(dm.frame, fsw, 2);
                dm.frame[2] = (uint8_t)((dm.frame[2] & 0x0f) | fsw[2]);
            }
            return true;
        }
        return false;
    }
    if (dm.flywheel) {
        dm.flywheel = false;
        dm.fromFlywheel = true;
        dm.receiving = true;
        dm.symbols = 0;
        memset(dm.frame, 0, sizeof(dm.frame));
        return false;
    }
    if (dm.samples < 140)
        return false;

    /* Least squares fit y = centre + step * sync, either polarity, over
     * the last ten symbols, one every 5 samples */
    const float *y = &dm.history[current + HISTORY - (FSW_SYMBOLS - 1) * 5];
    const float n = (float)FSW_SYMBOLS;
    float sumY = 0, sumXY = 0;
    for (unsigned i = 0; i < FSW_SYMBOLS; i++) {
        sumY += y[i * 5];
        sumXY += syncX[i] * y[i * 5];
    }
    const float step = (sumXY - syncSumX * sumY / n) / syncDenominator;
    const float centre = (sumY - step * syncSumX) / n;
    float error = 1.0f;
    if (fabsf(step) >= MIN_STEP) {
        float residual = 0.0f;
        for (unsigned i = 0; i < FSW_SYMBOLS; i++) {
            const float delta = y[i * 5] - centre - step * syncX[i];
            residual += delta * delta;
        }
        error = residual / (n * step * step);
        if (error < diag.bestError) {
            diag.bestError = error;
            diag.bestStep = step;
        }
    }
    if (!dm.pending && error < MAX_SYNC_ERROR) {
        diag.syncs++;
        dm.pending = 1;
        dm.bestError = error;
        dm.bestTime = dm.samples;
        dm.centre = centre;
        dm.step = step;
    } else if (dm.pending) {
        dm.pending++;
        if (error < dm.bestError) {
            dm.bestError = error;
            dm.bestTime = dm.samples;
            dm.centre = centre;
            dm.step = step;
        }
        if (dm.pending >= 5) {
            dm.pending = 0;
            dm.receiving = true;
            dm.fromFlywheel = false;
            dm.invStep = 1.0f / dm.step;
            dm.nextSymbol = (float)dm.bestTime + 5.0f;
            dm.previousSymbol = (float)syncLevel(FSW_SYMBOLS - 1);
            dm.symbols = FSW_SYMBOLS;
            memset(dm.frame, 0, sizeof(dm.frame));
            memcpy(dm.frame, fsw, sizeof(fsw));
        }
    }
    return false;
}

/* ---- Frame decoding ------------------------------------------------------ */

static const uint8_t scrambler[FRAME_BYTES] = {
    0x00, 0x00, 0x00, 0x82, 0xA0, 0x88, 0x8A, 0x00, 0xA2, 0xA8, 0x82, 0x8A,
    0x82, 0x02, 0x20, 0x08, 0x8A, 0x20, 0xAA, 0xA2, 0x82, 0x08, 0x22, 0x8A,
    0xAA, 0x08, 0x28, 0x88, 0x28, 0x28, 0x00, 0x0A, 0x02, 0x82, 0x20, 0x28,
    0x82, 0x2A, 0xAA, 0x20, 0x22, 0x80, 0xA8, 0x8A, 0x08, 0xA0, 0xAA, 0x02
};

#define FSW_BITS 20
#define LICH_BITS 16
#define SACCH_BITS 60
#define FACCH1_BITS 144
#define VOICE_OFFSET 96 /* bits: sync, LICH and SACCH come first */

static bool getBit(const uint8_t *p, unsigned n)
{
    return (p[n / 8] >> (7 - n % 8)) & 1u;
}

static void setBit(uint8_t *p, unsigned n, bool value)
{
    const uint8_t mask = (uint8_t)(0x80u >> (n % 8));
    p[n / 8] = (uint8_t)(value ? (p[n / 8] | mask) : (p[n / 8] & ~mask));
}

/* Rate 1/2, K = 5 Viterbi decoder with erasures (MMDVMHost
 * CNXDNConvolution): inputs 0 or 2 for a bit, 1 for a punctured one */
static const uint8_t branch1[8] = { 0, 0, 0, 0, 2, 2, 2, 2 };
static const uint8_t branch2[8] = { 0, 2, 2, 0, 0, 2, 2, 0 };

static struct {
    uint16_t metrics[2][16];
    uint16_t decisions[110];
    unsigned old;
    unsigned count;
} vit;

static void viterbiDecode(uint8_t s0, uint8_t s1)
{
    const uint16_t *oldM = vit.metrics[vit.old];
    uint16_t *newM = vit.metrics[vit.old ^ 1u];
    uint16_t d = 0;
    for (unsigned i = 0; i < 8; i++) {
        const unsigned j = i * 2;
        const int e0 = (int)branch1[i] - (int)s0;
        const int e1 = (int)branch2[i] - (int)s1;
        const uint16_t metric = (uint16_t)((e0 < 0 ? -e0 : e0)
                                           + (e1 < 0 ? -e1 : e1));
        uint16_t m0 = (uint16_t)(oldM[i] + metric);
        uint16_t m1 = (uint16_t)(oldM[i + 8] + (4 - metric));
        const unsigned d0 = m0 >= m1;
        newM[j] = d0 ? m1 : m0;
        m0 = (uint16_t)(oldM[i] + (4 - metric));
        m1 = (uint16_t)(oldM[i + 8] + metric);
        const unsigned d1 = m0 >= m1;
        newM[j + 1] = d1 ? m1 : m0;
        d |= (uint16_t)((d1 << (j + 1)) | (d0 << j));
    }
    if (vit.count < sizeof(vit.decisions) / sizeof(vit.decisions[0]))
        vit.decisions[vit.count++] = d;
    vit.old ^= 1u;
}

static void viterbiChainback(uint8_t *out, unsigned nBits)
{
    uint32_t state = 0;
    unsigned dp = vit.count;
    while (nBits-- > 0 && dp > 0) {
        --dp;
        const unsigned i = state >> 4;
        const unsigned bit = (vit.decisions[dp] >> i) & 1u;
        state = (bit << 7) | (state >> 1);
        setBit(out, nBits, bit != 0);
    }
}

/*
 * Deinterleave (column-wise matrix of 'columns' columns), depuncture
 * (an erasure every 'period' coded bits from 'first') and decode a block
 * of 'bits' channel bits at bit 'offset' of the frame.
 */
static void channelDecode(const uint8_t *frame, unsigned offset, unsigned bits,
                          unsigned columns, unsigned first, unsigned period,
                          unsigned steps, uint8_t *out, unsigned outBits)
{
    uint8_t coded[210];
    unsigned n = 0, next = first;
    const unsigned rows = bits / columns;
    for (unsigned i = 0; i < bits; i++) {
        if (n == next) {
            coded[n++] = 1;
            next += period;
        }
        const unsigned src = (i % columns) * rows + i / columns;
        coded[n++] = getBit(frame, offset + src) ? 2 : 0;
    }
    while (n < steps * 2)
        coded[n++] = 0;
    memset(&vit, 0, sizeof(vit));
    for (unsigned i = 0; i < steps; i++)
        viterbiDecode(coded[2 * i], coded[2 * i + 1]);
    viterbiChainback(out, outBits);
}

static uint8_t crc6(const uint8_t *in, unsigned length)
{
    uint8_t crc = 0x3f;
    for (unsigned i = 0; i < length; i++) {
        const bool bit1 = getBit(in, i);
        const bool bit2 = (crc & 0x20) != 0;
        crc = (uint8_t)(crc << 1);
        if (bit1 ^ bit2)
            crc ^= 0x27;
    }
    return crc & 0x3f;
}

static uint16_t crc12(const uint8_t *in, unsigned length)
{
    uint16_t crc = 0x0fff;
    for (unsigned i = 0; i < length; i++) {
        const bool bit1 = getBit(in, i);
        const bool bit2 = (crc & 0x0800) != 0;
        crc = (uint16_t)(crc << 1);
        if (bit1 ^ bit2)
            crc ^= 0x080f;
    }
    return crc & 0x0fff;
}

static unsigned readBits(const uint8_t *in, unsigned offset, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++)
        v = (v << 1) | getBit(in, offset + i);
    return v;
}

/* SACCH: 26 bits (structure, RAN, 18 bits of a Layer 3 message), CRC6 */
static bool sacchDecode(const uint8_t *frame, uint8_t out[5])
{
    memset(out, 0, 5);
    /* 12 columns of 5: bit i at (i % 12) * 5 + i / 12 */
    channelDecode(frame, FSW_BITS + LICH_BITS, SACCH_BITS, 12, 5, 6, 40,
                  out, 36);
    return crc6(out, 26) == readBits(out, 26, 6);
}

/* FACCH1: 80 bits of a Layer 3 message, CRC12 */
static bool facch1Decode(const uint8_t *frame, unsigned offset,
                         uint8_t out[12])
{
    memset(out, 0, 12);
    /* 16 columns of 9 */
    channelDecode(frame, offset, FACCH1_BITS, 16, 1, 4, 100, out, 96);
    return crc12(out, 80) == readBits(out, 80, 12);
}

/* Voice channel: positions of the bits of a, b and c in 72 (NXDNAudio) */
static unsigned posA(unsigned i)
{
    return i < 18 ? 4 * i : 4 * (i - 18) + 1;
}

static unsigned posB(unsigned i)
{
    return i < 12 ? 25 + 4 * i : 2 + 4 * (i - 12);
}

static unsigned posC(unsigned i)
{
    return i < 7 ? 46 + 4 * i : 3 + 4 * (i - 7);
}

static void voiceExtract(const uint8_t *frame, unsigned offset,
                         uint8_t abc[9])
{
    memset(abc, 0, 9);
    unsigned n = 0;
    for (unsigned i = 0; i < 24; i++, n++)
        setBit(abc, n, getBit(frame, offset + posA(i)));
    for (unsigned i = 0; i < 23; i++, n++)
        setBit(abc, n, getBit(frame, offset + posB(i)));
    for (unsigned i = 0; i < 25; i++, n++)
        setBit(abc, n, getBit(frame, offset + posC(i)));
}

/* ---- Receiver state ------------------------------------------------------ */

#define MSG_VCALL 0x01
#define MSG_TX_REL 0x08

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct nxdnStatus st;
static uint32_t lastFrameMs;
static uint32_t lastVoiceMs;
static uint32_t nextStatsMs;
static uint32_t voiceFrames;
static uint32_t sacchGood, facchGood;
/* SACCH superframe: four 18-bit parts of a Layer 3 message */
static uint8_t superMsg[9];
static uint8_t superMask;
/* The transmission was released: nothing follows, no flywheel */
static bool txReleased;

static void callEnd(const char *why)
{
    if (!st.call)
        return;
    st.call = false;
    st.serial++;
    dmr_voice_ambe49_flush();
    NXDN_PRINT("NXDN fin (%s) %u>%s%u\n", why, (unsigned)st.src,
               st.group ? "TG " : "", (unsigned)st.dst);
}

/*
 * A Layer 3 message (FACCH1 or a complete SACCH superframe); lock held.
 * A superframe is four SACCHs with a 6-bit CRC each: on a noisy signal a
 * wrong one gets through now and then, so its message counts only when
 * the next superframe repeats it. FACCH1 has a 12-bit CRC: taken at once.
 */
static uint8_t lastWeak[8];
static bool lastWeakValid;

static void layer3(const uint8_t *m, bool strong)
{
    if (!strong) {
        const bool repeated = lastWeakValid && memcmp(lastWeak, m, 8) == 0;
        memcpy(lastWeak, m, 8);
        lastWeakValid = true;
        if (!repeated)
            return;
    }
    const unsigned type = m[0] & 0x3fu;
    if (type == MSG_VCALL) {
        const uint16_t src = (uint16_t)((m[3] << 8) | m[4]);
        const uint16_t dst = (uint16_t)((m[5] << 8) | m[6]);
        const bool group = (m[2] & 0x80u) == 0;
        const uint8_t cipher = m[7] >> 6;
        const bool changed = !st.idsValid || st.src != src || st.dst != dst
                          || st.group != group || st.cipher != cipher;
        st.src = src;
        st.dst = dst;
        st.group = group;
        st.cipher = cipher;
        st.keyId = m[7] & 0x3fu;
        st.encrypted = cipher != 0;
        st.idsValid = true;
        if (!st.call || changed) {
            st.serial++;
            NXDN_PRINT("NXDN llamada %u>%s%u RAN %d%s\n", (unsigned)src,
                       group ? "TG " : "", (unsigned)dst,
                       st.ran == 0xff ? -1 : (int)st.ran,
                       cipher ? " CIFRADA" : "");
            if (cipher)
                NXDN_PRINT("NXDN cifrado tipo %u clave %u: sin audio\n",
                           (unsigned)cipher, (unsigned)st.keyId);
        }
        st.call = true;
    } else if (type == MSG_TX_REL) {
        txReleased = true;
        callEnd("liberada");
    }
}

static void handleFrame(uint8_t *frame, uint32_t now)
{
    /* A frame by flywheel whose sync word did not come: the transmission
     * has ended (or faded); search for the next sync instead */
    if (dm.fromFlywheel && dm.fswErrors > 3) {
        dm.flywheel = false;
        return;
    }
    for (unsigned i = 0; i < FRAME_BYTES; i++)
        frame[i] ^= scrambler[i];

    /* LICH: one bit per symbol pair, the first bit of each dibit */
    unsigned lich = 0;
    for (unsigned i = 0; i < 8; i++)
        lich = (lich << 1) | getBit(frame, FSW_BITS + 2 * i);
    unsigned parity = ((lich >> 7) + (lich >> 6) + (lich >> 5) + (lich >> 4))
                    & 1u;
    const unsigned l7 = lich >> 1;
    if (l7 == 0x08 || l7 == 0x4a || l7 == 0x48 || l7 == 0x46)
        parity = ((lich >> 7) + (lich >> 6) + (lich >> 5) + (lich >> 4)
                  + (lich >> 3) + (lich >> 2) + (lich >> 1)) & 1u;
    const bool good = parity == (lich & 1u);
    dm.flywheel = good;
    if (!good) {
        pthread_mutex_lock(&lock);
        st.lichBad++;
        pthread_mutex_unlock(&lock);
        return;
    }
    const unsigned rfct = (lich >> 6) & 3u;
    const unsigned usc = (lich >> 4) & 3u;
    const unsigned option = (lich >> 2) & 3u;

    /* Decode outside the lock */
    uint8_t sacch[5], facch[2][12];
    bool sacchOk = false, facchOk[2] = { false, false };
    uint8_t voice[4][9];
    unsigned nVoice = 0;
    bool traffic = rfct != NXDN_RCCH && usc != 1; /* not control, not UDCH */

    if (traffic) {
        sacchOk = sacchDecode(frame, sacch);
        /* Option: 3 four voice frames, 1 FACCH1 then voice, 2 voice then
         * FACCH1, 0 two FACCH1 */
        const bool firstFacch = option == 0 || option == 1;
        const bool secondFacch = option == 0 || option == 2;
        if (firstFacch)
            facchOk[0] = facch1Decode(frame, VOICE_OFFSET, facch[0]);
        else
            for (unsigned i = 0; i < 2; i++)
                voiceExtract(frame, VOICE_OFFSET + 72 * i, voice[nVoice++]);
        if (secondFacch)
            facchOk[1] = facch1Decode(frame, VOICE_OFFSET + FACCH1_BITS,
                                      facch[1]);
        else
            for (unsigned i = 2; i < 4; i++)
                voiceExtract(frame, VOICE_OFFSET + 72 * i, voice[nVoice++]);
    }

    pthread_mutex_lock(&lock);
    lastFrameMs = now;
    st.frames++;
    if (!st.sync || st.channel != rfct)
        st.serial++;
    st.sync = true;
    st.channel = (uint8_t)rfct;
    st.level = (int16_t)fabsf(3.0f * dm.step);
    if (sacchOk) {
        sacchGood++;
        const uint8_t ran = sacch[0] & 0x3fu;
        if (st.ran != ran) {
            st.ran = ran;
            st.serial++;
        }
        /* Superframe parts: structure 3 = 1/4, 2 = 2/4, 1 = 3/4, 0 = 4/4
         * (0 is also a single SACCH, in non-superframe frames) */
        if (usc == 2) {
            const unsigned structure = sacch[0] >> 6;
            const unsigned part = 3u - structure;
            if (part == 0) {
                memset(superMsg, 0, sizeof(superMsg));
                superMask = 0;
            }
            for (unsigned i = 0; i < 18; i++)
                setBit(superMsg, part * 18 + i, getBit(sacch, 8 + i));
            superMask |= (uint8_t)(1u << part);
            if (superMask == 0x0f) {
                layer3(superMsg, false);
                superMask = 0;
            }
        }
    }
    for (unsigned i = 0; i < 2; i++)
        if (facchOk[i]) {
            facchGood++;
            layer3(facch[i], true);
        }
    const bool play = nVoice > 0 && !st.encrypted;
    if (nVoice > 0) {
        /* Voice frames mean a call, even before its header is decoded */
        if (!st.call) {
            st.call = true;
            st.serial++;
        }
        lastVoiceMs = now;
        if (!st.voice && play) {
            st.voice = true;
            st.serial++;
        }
    }
    const bool released = !st.call;
    if (txReleased) {
        txReleased = false;
        dm.flywheel = false;
    }
    pthread_mutex_unlock(&lock);

    if (play && !released) {
        for (unsigned i = 0; i < nVoice; i++)
            dmr_voice_ambe72(voice[i], now);
        voiceFrames += nVoice;
    }
}

void nxdn_rx_reset(void)
{
    memset(&dm, 0, sizeof(dm));
    syncInit();
    diagReset();
    dmr_voice_reset();
    pthread_mutex_lock(&lock);
    const uint32_t serial = st.serial;
    memset(&st, 0, sizeof(st));
    st.ran = 0xff;
    st.serial = serial + 1u;
    lastFrameMs = lastVoiceMs = 0;
    voiceFrames = sacchGood = facchGood = 0;
    superMask = 0;
    lastWeakValid = false;
    txReleased = false;
    pthread_mutex_unlock(&lock);
}

void nxdn_rx_feed(const int16_t *samples, unsigned n, uint32_t now_ms)
{
    const uint32_t t0 = dmr_port_cycles();
    for (unsigned i = 0; i < n; i++) {
        diag.sum += samples[i];
        diag.sumSq += (int64_t)samples[i] * samples[i];
        diag.n++;
        dm.decimSum += samples[i];
        if (++dm.decimCount < DECIMATION)
            continue;
        const float x = (float)dm.decimSum / (float)DECIMATION;
        dm.decimSum = 0;
        dm.decimCount = 0;
        if (demodSample(x))
            handleFrame(dm.frame, now_ms);
    }
    const uint32_t us = dmr_port_cycles_to_us(dmr_port_cycles() - t0);
    diag.blocks++;
    diag.feedUs += us;
    if (us > diag.feedMaxUs)
        diag.feedMaxUs = us;
}

void nxdn_rx_noteRssi(int dbm)
{
    if (dbm > diag.rssiMax)
        diag.rssiMax = dbm;
}

void nxdn_rx_overrun(void)
{
    pthread_mutex_lock(&lock);
    st.overruns++;
    pthread_mutex_unlock(&lock);
}

void nxdn_rx_tick(uint32_t now_ms)
{
    pthread_mutex_lock(&lock);
    if (st.voice && (now_ms - lastVoiceMs) > 500u) {
        st.voice = false;
        st.serial++;
    }
    if (st.call && (now_ms - lastFrameMs) > 600u)
        callEnd("sin tramas");
    if (st.sync && (now_ms - lastFrameMs) > 1000u) {
        st.sync = false;
        st.serial++;
    }
    if ((int32_t)(now_ms - nextStatsMs) >= 0) {
        nextStatsMs = now_ms + 10000u;
        long mean = 0, rms = 0;
        if (diag.n) {
            const double m = (double)diag.sum / diag.n;
            const double var = (double)diag.sumSq / diag.n - m * m;
            mean = (long)m;
            rms = (long)sqrt(var > 0.0 ? var : 0.0);
        }
        NXDN_PRINT("NXDN estado: %s tramas %lu LICH mal %lu SACCH %lu "
                   "FACCH %lu voz %lu nivel %d perdidos %lu | RSSI max %d "
                   "dBm entrada media %ld rms %ld | syncs %lu mejor ajuste "
                   "%d.%02d (paso %d) | demod %lu/%lu us por bloque de "
                   "30 ms\n",
                   st.sync ? "ENGANCHADO" : "buscando",
                   (unsigned long)st.frames, (unsigned long)st.lichBad,
                   (unsigned long)sacchGood, (unsigned long)facchGood,
                   (unsigned long)voiceFrames, (int)st.level,
                   (unsigned long)st.overruns, diag.rssiMax, mean, rms,
                   (unsigned long)diag.syncs,
                   diag.bestError < 99.0f ? (int)diag.bestError : 99,
                   diag.bestError < 99.0f
                       ? (int)(diag.bestError * 100.0f) % 100 : 99,
                   (int)diag.bestStep,
                   (unsigned long)(diag.blocks ? diag.feedUs / diag.blocks
                                               : 0),
                   (unsigned long)diag.feedMaxUs);
        diagReset();
    }
    pthread_mutex_unlock(&lock);
}

void nxdn_rx_status(struct nxdnStatus *out)
{
    pthread_mutex_lock(&lock);
    *out = st;
    pthread_mutex_unlock(&lock);
}
