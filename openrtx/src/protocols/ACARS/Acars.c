/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <string.h>
#include "interfaces/delays.h"
#include "protocols/ACARS/Acars.h"

#define SPB 20           /* Samples per bit: 48000 / 2400 */
#ifndef TIMING_GAIN
#define TIMING_GAIN 1.0f /* Samples per unit early-late difference */
#endif
#ifndef DELAY
#define DELAY 7       /* Discriminator delay: 1200 Hz +, 2400 Hz - */
#endif
#define FRAME_MAX 240 /* Bytes from the mode character to ETX/ETB */
#define SYNC 0x1616u  /* SYN SYN, as the last 16 bits received */
#define SOH 0x01
#define ETX 0x83      /* With its parity bit */
#define ETB 0x97
#define STX 0x02

/* Large and rarely touched: in PSRAM on the C62 (not zeroed) */
#ifdef PLATFORM_C62
#define ACARS_BSS __attribute__((section(".psram_section")))
#else
#define ACARS_BSS
#endif

enum state { SEARCH = 0, WAIT_SOH, TEXT, CRC1, CRC2 };

/* Demodulator and decoder: one thread only */
static struct {
    float bp[2][2]; /* Band-pass biquads' state */
    float delay[DELAY + 1];
    unsigned dpos;
    float box[SPB];
    float boxSum;
    unsigned bpos;
    unsigned phase;  /* Sample within the bit, 0..SPB-1 */
    float countdown; /* Samples to the next decision, in a frame */
    float hist[3];   /* |integrator| at the last 3 samples */
    float early;     /* |integrator| 2 samples before the decision */
    int lateIn;      /* Samples to the late reading, 0 none */
    bool prevSign;   /* Integrator sign at the previous sample */
    bool prevTone;   /* Last tone decided, within a frame */
    /* Sync search at every sampling phase: the last tone and 16 bits of
     * each phase, for each tone-to-bit mapping */
    bool searchTone[SPB];
    uint16_t sr[SPB][4];
    int hyp; /* Mapping locked on */
    enum state state;
    uint8_t byte;
    unsigned nbits;
    uint8_t frame[FRAME_MAX];
    unsigned len;
    unsigned badParity; /* Characters failing parity in the frame */
    uint8_t crc[2];
} rx;

static pthread_mutex_t acarsMutex = PTHREAD_MUTEX_INITIALIZER;
static ACARS_BSS struct acarsMsg msgs[ACARS_MAX_MSGS]; /* Under acarsMutex */
static int msgHead, msgCount;
static uint32_t msgSeq;
static struct acarsStats stats;

static const struct {
    const char *label;
    const char *name;
} labels[] = {
    { "Q0", "Link test" },
    { "_d", "General response" },
    { "SA", "Media advisory" },
    { "H1", "Terminal message" },
    { "5Z", "Airline" },
    { "QA", "Out" },
    { "QB", "Off" },
    { "QC", "On" },
    { "QD", "In" },
    { "QE", "Out, fuel" },
    { "QF", "Off, destination" },
    { "QG", "Out, return" },
    { "B9", "ATIS request" },
    { "BA", "ATC, FANS" },
    { "B6", "ATC, FANS" },
    { "AA", "ATC, FANS" },
    { "A6", "ATC, FANS" },
    { "80", "Position" },
    { "10", "Out" },
    { "11", "Off" },
    { "12", "On" },
    { "13", "In" },
    { "15", "Position" },
    { "16", "Position" },
    { "20", "Delay" },
    { "21", "Delay" },
    { "C1", "Uplink, cockpit" },
    { "RA", "Uplink, printer" },
};

const char *acars_labelName(const char *label)
{
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (strcmp(labels[i].label, label) == 0)
            return labels[i].name;
    }
    return NULL;
}

/* CRC-16 CCITT, reflected (0x8408), initial value 0 */
static uint16_t crcAdd(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    return crc;
}

static bool frameCrcOk(const uint8_t *f, unsigned len, const uint8_t *crc)
{
    uint16_t c = 0;
    for (unsigned i = 0; i < len; i++)
        c = crcAdd(c, f[i]);
    c = crcAdd(c, crc[0]);
    c = crcAdd(c, crc[1]);
    return c == 0;
}

static bool oddParity(uint8_t b)
{
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return (b & 1) != 0;
}

/**
 * Repair one or two characters that failed parity: flip a bit in each,
 * keep the result only if the CRC is then good.
 */
static bool repair(uint8_t *f, unsigned len, const uint8_t *crc)
{
    int bad[2], nbad = 0;
    for (unsigned i = 0; i < len; i++) {
        if (!oddParity(f[i])) {
            if (nbad == 2)
                return false;
            bad[nbad++] = (int)i;
        }
    }
    if (nbad == 0)
        return false;

    for (int a = 0; a < 8; a++) {
        f[bad[0]] ^= (uint8_t)(1u << a);
        if (nbad == 1) {
            if (frameCrcOk(f, len, crc))
                return true;
        } else {
            for (int b = 0; b < 8; b++) {
                f[bad[1]] ^= (uint8_t)(1u << b);
                if (frameCrcOk(f, len, crc))
                    return true;
                f[bad[1]] ^= (uint8_t)(1u << b);
            }
        }
        f[bad[0]] ^= (uint8_t)(1u << a);
    }
    return false;
}

/**
 * Copy n characters of the frame from an offset, printable ASCII only.
 */
static void field(char *dst, size_t size, const uint8_t *f, unsigned len,
                  unsigned from, unsigned n)
{
    size_t o = 0;
    for (unsigned i = from; i < from + n && i < len && o + 1 < size; i++) {
        char c = (char)(f[i] & 0x7f);
        dst[o++] = (c >= 0x20 && c < 0x7f) ? c : ' ';
    }
    dst[o] = '\0';
}

/**
 * A valid frame: parse it into the history.
 */
static void accept(const uint8_t *f, unsigned len, bool repaired)
{
    struct acarsMsg m;
    memset(&m, 0, sizeof(m));

    /* Mode, address (7), acknowledge, label (2), block id, then STX */
    m.mode = (char)(f[0] & 0x7f);
    char addr[8];
    field(addr, sizeof(addr), f, len, 1, 7);
    const char *a = addr;
    while (*a == '.' || *a == ' ')
        a++;
    strncpy(m.address, a, sizeof(m.address) - 1);

    m.label[0] = (char)(f[9] & 0x7f);
    m.label[1] = (char)(f[10] & 0x7f);
    if (m.label[1] == 0x7f)
        m.label[1] = 'd'; /* "_DEL", the general response */
    m.blockId = (char)(f[11] & 0x7f);
    m.downlink = (m.blockId >= '0' && m.blockId <= '9');
    m.repaired = repaired;

    /* Text up to ETX/ETB, after a sequence number and the flight number in
     * downlinks */
    unsigned end = len - 1;
    if (len > 12 && (f[12] & 0x7f) == STX) {
        unsigned from = 13;
        if (m.downlink && end >= 23) {
            field(m.flight, sizeof(m.flight), f, end, 17, 6);
            from = 23;
        }
        field(m.text, sizeof(m.text), f, end, from, end - from);
    }

    pthread_mutex_lock(&acarsMutex);
    m.seq = ++msgSeq;
    m.tick = getTick();
    msgs[msgHead] = m;
    msgHead = (msgHead + 1) % ACARS_MAX_MSGS;
    if (msgCount < ACARS_MAX_MSGS)
        msgCount++;
    stats.frames++;
    if (repaired)
        stats.repaired++;
    pthread_mutex_unlock(&acarsMutex);
}

static void finishFrame(void)
{
    bool ok = frameCrcOk(rx.frame, rx.len, rx.crc);
    bool repaired = false;
    if (!ok) {
        repaired = repair(rx.frame, rx.len, rx.crc);
        ok = repaired;
    }
    if (ok && rx.len >= 12) {
        accept(rx.frame, rx.len, repaired);
    } else {
        pthread_mutex_lock(&acarsMutex);
        stats.crcErrors++;
        pthread_mutex_unlock(&acarsMutex);
    }
    rx.state = SEARCH;
}

/**
 * One received byte, after the sync word.
 */
static void byteIn(uint8_t b)
{
    switch (rx.state) {
        case WAIT_SOH:
            if (b == SOH) {
                rx.state = TEXT;
                rx.len = 0;
                rx.badParity = 0;
            } else if (b != 0x16) {
                rx.state = SEARCH; /* Not a frame */
            }
            break;

        case TEXT:
            rx.frame[rx.len++] = b;
            /* More bad characters than a repair can fix: the signal is gone
             * or the clock slipped; look for the next frame at once */
            if (!oddParity(b) && ++rx.badParity > 2) {
                rx.state = SEARCH;
                pthread_mutex_lock(&acarsMutex);
                stats.crcErrors++;
                pthread_mutex_unlock(&acarsMutex);
            } else if (b == ETX || b == ETB) {
                rx.state = CRC1;
            } else if (rx.len >= FRAME_MAX) {
                rx.state = SEARCH;
            }
            break;

        case CRC1:
            rx.crc[0] = b;
            rx.state = CRC2;
            break;

        case CRC2:
            rx.crc[1] = b;
            finishFrame();
            break;

        default:
            break;
    }
}

/**
 * One tone decision: true for 1200 Hz.
 */
/**
 * Sync search: every sample is a tone decision for its sampling phase. The
 * phase and the mapping that show SYN SYN set the bit clock and the
 * decoding of the frame, so the search does not wait for the clock to
 * settle on the few transitions before the sync word.
 */
static void searchIn(bool low)
{
    unsigned p = rx.phase;
    bool same = (low == rx.searchTone[p]);
    rx.searchTone[p] = low;
    const bool bits[4] = { low, !low, same, !same };

    for (int h = 0; h < 4; h++) {
        uint16_t *sr = &rx.sr[p][h];
        *sr = (uint16_t)((*sr >> 1) | (bits[h] ? 0x8000u : 0));
        if (*sr == SYNC) {
            rx.hyp = h;
            rx.countdown = SPB; /* This sample was a decision */
            rx.prevTone = low;
            rx.state = WAIT_SOH;
            rx.byte = 0;
            rx.nbits = 0;
            pthread_mutex_lock(&acarsMutex);
            stats.syncs++;
            pthread_mutex_unlock(&acarsMutex);
            return;
        }
    }
}

/**
 * One tone decision within a frame: true for 1200 Hz.
 */
static void toneIn(bool low)
{
    bool same = (low == rx.prevTone);
    rx.prevTone = low;
    const bool bits[4] = { low, !low, same, !same };

    /* Characters, least significant bit first */
    if (bits[rx.hyp])
        rx.byte |= (uint8_t)(1u << rx.nbits);
    if (++rx.nbits == 8) {
        uint8_t b = rx.byte;
        rx.byte = 0;
        rx.nbits = 0;
        byteIn(b);
        if (rx.state == SEARCH)
            memset(rx.sr, 0, sizeof(rx.sr));
    }
}

/*
 * Band-pass 900-2700 Hz around the tones: a 2nd order high-pass at 900 Hz
 * and a 2nd order low-pass at 2700 Hz (RBJ biquads, Q 0.707, at 48 kHz),
 * direct form II transposed.
 */
static const float bpCoef[2][5] = {
    /* b0, b1, b2, a1, a2 */
    { 0.92007f, -1.84013f, 0.92007f, -1.83373f, 0.84653f }, /* HP 900 */
    { 0.02483f, 0.04966f, 0.02483f, -1.50744f, 0.60676f },  /* LP 2700 */
};

static float bandPass(float x)
{
    for (int k = 0; k < 2; k++) {
        const float *c = bpCoef[k];
        float y = c[0] * x + rx.bp[k][0];
        rx.bp[k][0] = c[1] * x - c[3] * y + rx.bp[k][1];
        rx.bp[k][1] = c[2] * x - c[4] * y;
        x = y;
    }
    return x;
}

void acars_process(const int16_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float y = bandPass((float)samples[i]);

        /* Delay-line discriminator, integrated over one bit */
        float d = y * rx.delay[(rx.dpos + 1) % (DELAY + 1)];
        rx.delay[rx.dpos] = y;
        rx.dpos = (rx.dpos + 1) % (DELAY + 1);
        rx.boxSum += d - rx.box[rx.bpos];
        rx.box[rx.bpos] = d;
        rx.bpos = (rx.bpos + 1) % SPB;

        bool sign = rx.boxSum > 0;
        if (rx.state == SEARCH) {
            searchIn(sign);
            rx.prevSign = sign;
            if (++rx.phase == SPB)
                rx.phase = 0;
            continue;
        }

        /*
         * Bit clock within a frame: a decision every bit, counted down so
         * that no bit is ever skipped or doubled. Early-late gate: the
         * integrator peaks at the right decision point, so its magnitude
         * 2 samples later against 2 samples earlier tells which way to
         * move the next decisions; equal on a steady tone.
         */
        float mag = fabsf(rx.boxSum);
        if (rx.lateIn > 0 && --rx.lateIn == 0) {
            float sum = mag + rx.early;
            if (sum > 0)
                rx.countdown += TIMING_GAIN * (mag - rx.early) / sum;
        }
        rx.prevSign = sign;

        rx.countdown -= 1.0f;
        if (rx.countdown <= 0) {
            rx.countdown += SPB;
            toneIn(sign);
            rx.early = rx.hist[1]; /* 2 samples ago */
            rx.lateIn = 2;
        }
        rx.hist[2] = rx.hist[1];
        rx.hist[1] = rx.hist[0];
        rx.hist[0] = mag;

        if (++rx.phase == SPB)
            rx.phase = 0;
    }

    pthread_mutex_lock(&acarsMutex);
    stats.inFrame = (rx.state != SEARCH);
    pthread_mutex_unlock(&acarsMutex);
}

void acars_reset(void)
{
    memset(&rx, 0, sizeof(rx));
    pthread_mutex_lock(&acarsMutex);
    memset(msgs, 0, sizeof(msgs));
    msgHead = msgCount = 0;
    memset(&stats, 0, sizeof(stats));
    pthread_mutex_unlock(&acarsMutex);
}

int acars_get(struct acarsMsg *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&acarsMutex);
    for (int i = 0; i < msgCount && n < max; i++)
        out[n++] = msgs[(msgHead - 1 - i + ACARS_MAX_MSGS) % ACARS_MAX_MSGS];
    pthread_mutex_unlock(&acarsMutex);
    return n;
}

uint32_t acars_lastSeq(void)
{
    pthread_mutex_lock(&acarsMutex);
    uint32_t s = msgCount ? msgSeq : 0;
    pthread_mutex_unlock(&acarsMutex);
    return s;
}

void acars_getStats(struct acarsStats *out)
{
    pthread_mutex_lock(&acarsMutex);
    *out = stats;
    pthread_mutex_unlock(&acarsMutex);
}
