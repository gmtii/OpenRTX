/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces/delays.h"
#include "protocols/AIS/Ais.h"

#define SPB 5.0f        /* Samples per bit: 48000 / 9600 */
#define LANES 3         /* Slicers, see ais_reset() */
#define FRAME_BYTES 128 /* Up to five slots of data */
#define MIN_BYTES 14    /* 96-bit message 27 and the CRC */
/* The last 12 training bits and the start flag, as the last 20 bits */
#define TRAIN_FLAG_A 0xAAA7Eu
#define TRAIN_FLAG_B 0x5557Eu
#define CRC_GOOD 0xF0B8u /* CRC-16/X.25 residue */
/* A bad frame counts as an error only if no other lane decodes the burst
 * within this many samples */
#define BAD_WAIT 400u

/* Large and rarely touched: in PSRAM on the C62 (not zeroed) */
#ifdef PLATFORM_C62
#define AIS_BSS __attribute__((section(".psram_section")))
#else
#define AIS_BSS
#endif

/* One slicer: hysteresis threshold, bit clock and HDLC receiver */
struct lane {
    float threshold;  /* Fraction of the peak to flip the level */
    bool level;       /* Slicer output */
    bool sampled;     /* Level at the previous bit */
    float phase;      /* Bit clock, 0..1, a bit decided on wrapping */
    uint32_t history; /* Last bits, newest in bit 0 */
    bool collecting;  /* Between the start flag and the end flag */
    unsigned ones;    /* Consecutive ones, for de-stuffing */
    unsigned nbits;   /* Bits stored in frame */
    uint8_t frame[FRAME_BYTES];
};

/* Level window: 64 samples, 12.8 bits. Data has a transition at least
 * every 7 bits (bit stuffing and flags), so the window always holds both
 * levels during a burst */
#define WINDOW 64

/* Demodulator: one thread only */
static struct {
    float prev, prev2;
    float window[WINDOW];
    unsigned wpos;
    uint32_t samples;     /* Counter, for BAD_WAIT */
    uint32_t badDeadline; /* A bad frame waits until then, 0 none */
    struct lane lane[LANES];
} rx;

static pthread_mutex_t aisMutex = PTHREAD_MUTEX_INITIALIZER;
static AIS_BSS struct aisShip ships[AIS_MAX_SHIPS]; /* Under aisMutex */
static int shipCount;
static uint32_t shipSeq;
static struct aisStats stats;

static uint16_t crcAdd(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    return crc;
}

/**
 * Unsigned field of a frame, bits numbered from the most significant bit
 * of the first byte.
 */
static uint32_t bitsU(const uint8_t *f, unsigned from, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = from; i < from + n; i++)
        v = (v << 1) | ((f[i >> 3] >> (7 - (i & 7))) & 1);
    return v;
}

static int32_t bitsS(const uint8_t *f, unsigned from, unsigned n)
{
    uint32_t v = bitsU(f, from, n);
    if (v & (1u << (n - 1)))
        v |= ~0u << n;
    return (int32_t)v;
}

/**
 * Text field in 6-bit ASCII, trailing '@' and spaces removed. Leaves dst
 * as it was if the field is empty.
 */
static void text6(char *dst, size_t size, const uint8_t *f, unsigned from,
                  unsigned chars)
{
    char buf[24];
    unsigned n = 0;
    for (unsigned i = 0; i < chars && n + 1 < sizeof(buf); i++) {
        uint32_t v = bitsU(f, from + 6 * i, 6);
        buf[n++] = (char)(v < 32 ? v + 64 : v);
    }
    while (n > 0 && (buf[n - 1] == '@' || buf[n - 1] == ' '))
        n--;
    /* '@' inside the text is padding too */
    for (unsigned i = 0; i < n; i++)
        if (buf[i] == '@')
            buf[i] = ' ';
    if (n == 0)
        return;
    if (n >= size)
        n = size - 1;
    memcpy(dst, buf, n);
    dst[n] = '\0';
}

/**
 * The table entry of a station, a new one (the oldest replaced when full).
 */
static struct aisShip *shipFor(uint32_t mmsi)
{
    for (int i = 0; i < shipCount; i++)
        if (ships[i].mmsi == mmsi)
            return &ships[i];

    struct aisShip *s;
    if (shipCount < AIS_MAX_SHIPS) {
        s = &ships[shipCount++];
    } else {
        s = &ships[0];
        for (int i = 1; i < shipCount; i++)
            if (ships[i].tick < s->tick)
                s = &ships[i];
    }
    memset(s, 0, sizeof(*s));
    s->mmsi = mmsi;
    s->cls = AIS_OTHER;
    s->sog = 1023;
    s->cog = 3600;
    s->heading = 511;
    s->navStatus = 15;
    return s;
}

/**
 * Position in 1/10000 minute, if available.
 */
static void position(struct aisShip *s, int32_t lon, int32_t lat)
{
    if (abs(lon) <= 180 * 600000 && abs(lat) <= 90 * 600000) {
        s->lon = lon;
        s->lat = lat;
        s->posValid = true;
    }
}

static void dimensions(struct aisShip *s, const uint8_t *f, unsigned from)
{
    unsigned length = bitsU(f, from, 9) + bitsU(f, from + 9, 9);
    unsigned beam = bitsU(f, from + 18, 6) + bitsU(f, from + 24, 6);
    if (length != 0)
        s->length = (uint16_t)length;
    if (beam != 0)
        s->beam = (uint8_t)(beam > 255 ? 255 : beam);
}

/**
 * A valid frame: update the station it comes from.
 *
 * @return false if the message is not one we read.
 */
static bool accept(const uint8_t *f, unsigned bytes)
{
    const unsigned bits = 8 * bytes;
    const unsigned type = bitsU(f, 0, 6);
    if (bits < 38)
        return false;
    const uint32_t mmsi = bitsU(f, 8, 30);

    pthread_mutex_lock(&aisMutex);
    struct aisShip *s = shipFor(mmsi);

    switch (type) {
        case 1:
        case 2:
        case 3:
            if (bits < 137)
                break;
            s->cls = AIS_CLASS_A;
            s->navStatus = (uint8_t)bitsU(f, 38, 4);
            s->sog = (uint16_t)bitsU(f, 50, 10);
            position(s, bitsS(f, 61, 28), bitsS(f, 89, 27));
            s->cog = (uint16_t)bitsU(f, 116, 12);
            s->heading = (uint16_t)bitsU(f, 128, 9);
            break;

        case 4:
            if (bits < 134)
                break;
            s->cls = AIS_BASE;
            position(s, bitsS(f, 79, 28), bitsS(f, 107, 27));
            break;

        case 5:
            if (bits < 422)
                break;
            s->cls = AIS_CLASS_A;
            text6(s->callsign, sizeof(s->callsign), f, 70, 7);
            text6(s->name, sizeof(s->name), f, 112, 20);
            s->shipType = (uint8_t)bitsU(f, 232, 8);
            dimensions(s, f, 240);
            s->draught = (uint8_t)bitsU(f, 294, 8);
            text6(s->destination, sizeof(s->destination), f, 302, 20);
            break;

        case 9:
            if (bits < 128)
                break;
            s->cls = AIS_SAR;
            s->sog = (uint16_t)(bitsU(f, 50, 10) * 10);
            position(s, bitsS(f, 61, 28), bitsS(f, 89, 27));
            s->cog = (uint16_t)bitsU(f, 116, 12);
            break;

        case 18:
        case 19:
            if (bits < 133)
                break;
            s->cls = AIS_CLASS_B;
            s->sog = (uint16_t)bitsU(f, 46, 10);
            position(s, bitsS(f, 57, 28), bitsS(f, 85, 27));
            s->cog = (uint16_t)bitsU(f, 112, 12);
            s->heading = (uint16_t)bitsU(f, 124, 9);
            if (type == 19 && bits >= 301) {
                text6(s->name, sizeof(s->name), f, 143, 20);
                s->shipType = (uint8_t)bitsU(f, 263, 8);
                dimensions(s, f, 271);
            }
            break;

        case 21:
            if (bits < 249)
                break;
            s->cls = AIS_ATON;
            text6(s->name, sizeof(s->name), f, 43, 20);
            position(s, bitsS(f, 164, 28), bitsS(f, 192, 27));
            break;

        case 24:
            if (bits < 160)
                break;
            if (s->cls == AIS_OTHER)
                s->cls = AIS_CLASS_B;
            if (bitsU(f, 38, 2) == 0) {
                text6(s->name, sizeof(s->name), f, 40, 20);
            } else if (bits >= 162) {
                s->shipType = (uint8_t)bitsU(f, 40, 8);
                text6(s->callsign, sizeof(s->callsign), f, 90, 7);
                dimensions(s, f, 132);
            }
            break;

        case 27:
            if (bits < 94)
                break;
            s->cls = AIS_CLASS_A;
            s->navStatus = (uint8_t)bitsU(f, 40, 4);
            /* 1/10 minute */
            if (bitsS(f, 44, 18) != 181 * 600 && bitsS(f, 62, 17) != 91 * 600)
                position(s, bitsS(f, 44, 18) * 1000, bitsS(f, 62, 17) * 1000);
            s->sog = (uint16_t)(bitsU(f, 79, 6) * 10);
            s->cog = (uint16_t)(bitsU(f, 85, 9) * 10);
            break;

        default:
            break;
    }

    s->msgType = (uint8_t)type;
    if (s->reports < 0xffff)
        s->reports++;
    s->tick = getTick();
    s->seq = ++shipSeq;
    stats.frames++;
    stats.ships = (uint32_t)shipCount;
    pthread_mutex_unlock(&aisMutex);
    return true;
}

static void laneSearch(struct lane *l)
{
    l->collecting = false;
    l->history = 0;
}

static void laneStart(struct lane *l)
{
    l->collecting = true;
    l->ones = 0;
    l->nbits = 0;
    memset(l->frame, 0, sizeof(l->frame));
}

/**
 * The end flag of a frame: check its length and CRC.
 */
static void laneFinish(struct lane *l)
{
    unsigned bytes = l->nbits / 8;
    if ((l->nbits % 8) != 0 || bytes < MIN_BYTES)
        return; /* Noise after a false start, or between two flags */

    /* Bytes were assembled least significant bit first, as sent: the CRC
     * runs on them so, the fields are read from their most significant bit
     * (ITU-R M.1371, as AIS-catcher and gpsd decode real ships) */
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < bytes; i++)
        crc = crcAdd(crc, l->frame[i]);
    if (crc != CRC_GOOD) {
        if (rx.badDeadline == 0)
            rx.badDeadline = rx.samples + BAD_WAIT;
        return;
    }

    accept(l->frame, bytes - 2);

    /* The other lanes heard the same burst */
    rx.badDeadline = 0;
    for (int i = 0; i < LANES; i++)
        laneSearch(&rx.lane[i]);
}

/**
 * One NRZI-decoded bit of a lane.
 */
static void laneBit(struct lane *l, bool bit)
{
    l->history = (l->history << 1) | bit;

    if (!l->collecting) {
        uint32_t h = l->history & 0xFFFFFu;
        if (h == TRAIN_FLAG_A || h == TRAIN_FLAG_B) {
            /* Once per burst, the first lane to find it */
            bool first = true;
            for (int i = 0; i < LANES; i++)
                first &= !rx.lane[i].collecting;
            if (first) {
                pthread_mutex_lock(&aisMutex);
                stats.syncs++;
                pthread_mutex_unlock(&aisMutex);
            }
            laneStart(l);
        }
        return;
    }

    if (l->ones == 5) {
        if (!bit) { /* Stuffed zero */
            l->ones = 0;
            return;
        }
        l->ones = 6;
        return;
    }
    if (l->ones == 6) {
        if (bit) { /* Seven ones: abort */
            laneSearch(l);
            return;
        }
        /* Flag: the zero and five ones before it were stored */
        l->nbits = (l->nbits >= 6) ? l->nbits - 6 : 0;
        laneFinish(l);
        /* Not a valid frame (a valid one stops every lane): the flag may
         * open the next one */
        if (l->collecting)
            laneStart(l);
        return;
    }

    if (l->nbits >= 8 * FRAME_BYTES) {
        laneSearch(l);
        return;
    }
    if (bit)
        l->frame[l->nbits >> 3] |= (uint8_t)(1u << (l->nbits & 7));
    l->nbits++;
    l->ones = bit ? l->ones + 1 : 0;
}

void ais_process(const int16_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        /* Low-pass: average over 3/5 of a bit */
        float x = (float)samples[i];
        float y = (x + rx.prev + rx.prev2) * (1.0f / 3.0f);
        rx.prev2 = rx.prev;
        rx.prev = x;

        /* The two levels: highest and lowest over the window. Their middle
         * is the centre (the carrier offset), half their distance the
         * amplitude: both follow a new burst within a few bits */
        rx.window[rx.wpos] = y;
        rx.wpos = (rx.wpos + 1) % WINDOW;
        float hi = rx.window[0], lo = rx.window[0];
        for (int k = 1; k < WINDOW; k++) {
            hi = fmaxf(hi, rx.window[k]);
            lo = fminf(lo, rx.window[k]);
        }
        float v = y - 0.5f * (hi + lo);
        float amp = 0.5f * (hi - lo);

        for (int k = 0; k < LANES; k++) {
            struct lane *l = &rx.lane[k];
            float th = amp * l->threshold;
            bool level = (v > th) ? true : (v < -th) ? false : l->level;

            /* Bit clock: a transition belongs half way between two
             * decisions */
            l->phase += 1.0f / SPB;
            if (level != l->level)
                l->phase += 0.25f * (0.5f - l->phase);
            l->level = level;
            if (l->phase >= 1.0f) {
                l->phase -= 1.0f;
                laneBit(l, level == l->sampled); /* NRZI: 1 no change */
                l->sampled = level;
            }
        }

        rx.samples++;
        if (rx.badDeadline != 0 && (int32_t)(rx.samples - rx.badDeadline) > 0) {
            rx.badDeadline = 0;
            pthread_mutex_lock(&aisMutex);
            stats.crcErrors++;
            pthread_mutex_unlock(&aisMutex);
        }
    }

    bool inFrame = false;
    for (int k = 0; k < LANES; k++)
        inFrame |= rx.lane[k].collecting && rx.lane[k].nbits > 16;
    pthread_mutex_lock(&aisMutex);
    stats.inFrame = inFrame;
    pthread_mutex_unlock(&aisMutex);
}

void ais_reset(void)
{
    memset(&rx, 0, sizeof(rx));
    /* Hysteresis, as a fraction of the amplitude: none (best with noise),
     * a little, and much (where the receive filters flatten single bits
     * and long runs droop) */
    rx.lane[0].threshold = 0.0f;
    rx.lane[1].threshold = 1.0f / 8.0f;
    rx.lane[2].threshold = 3.0f / 8.0f;
    pthread_mutex_lock(&aisMutex);
    memset(ships, 0, sizeof(ships));
    shipCount = 0;
    shipSeq = 0;
    memset(&stats, 0, sizeof(stats));
    pthread_mutex_unlock(&aisMutex);
}

int ais_get(struct aisShip *out, int max)
{
    pthread_mutex_lock(&aisMutex);
    int n = 0;
    bool taken[AIS_MAX_SHIPS] = { false };
    while (n < max && n < shipCount) {
        int best = -1;
        for (int i = 0; i < shipCount; i++)
            if (!taken[i] && (best < 0 || ships[i].seq > ships[best].seq))
                best = i;
        taken[best] = true;
        out[n++] = ships[best];
    }
    pthread_mutex_unlock(&aisMutex);
    return n;
}

uint32_t ais_lastSeq(void)
{
    pthread_mutex_lock(&aisMutex);
    uint32_t s = shipSeq;
    pthread_mutex_unlock(&aisMutex);
    return s;
}

void ais_getStats(struct aisStats *out)
{
    pthread_mutex_lock(&aisMutex);
    *out = stats;
    pthread_mutex_unlock(&aisMutex);
}

const char *ais_navStatusName(uint8_t status)
{
    static const char *const names[] = {
        "Underway", "Anchored",      "Not under cmd", "Restricted", "Draught",
        "Moored",   "Aground",       "Fishing",       "Sailing",    NULL,
        NULL,       "Towing astern", "Pushing",       NULL,         "AIS-SART",
    };
    if (status >= sizeof(names) / sizeof(names[0]))
        return NULL;
    return names[status];
}

const char *ais_shipTypeName(uint8_t type)
{
    switch (type) {
        case 30:
            return "Fishing";
        case 31:
        case 32:
            return "Towing";
        case 33:
            return "Dredging";
        case 34:
            return "Diving";
        case 35:
            return "Military";
        case 36:
            return "Sailing";
        case 37:
            return "Pleasure";
        case 50:
            return "Pilot";
        case 51:
            return "SAR";
        case 52:
            return "Tug";
        case 53:
            return "Port tender";
        case 55:
            return "Law enforcement";
        case 58:
            return "Medical";
        default:
            break;
    }
    switch (type / 10) {
        case 2:
            return "WIG";
        case 4:
            return "High speed";
        case 6:
            return "Passenger";
        case 7:
            return "Cargo";
        case 8:
            return "Tanker";
        case 9:
            return "Other";
        default:
            return NULL;
    }
}
