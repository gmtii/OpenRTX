/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Short data decoding, ported from the ESP32-P4 SDR (main/tetra/tetra_l3.c)
 * onto libtetra's SDS events: libtetra parses D-SDS-DATA and D-STATUS, this
 * decodes their payload.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <tetra/tetra.h>
#include "interfaces/delays.h"
#include "protocols/TETRA/TetraSds.h"

/* Concatenated messages being reassembled (29.5.10 UDH, 29.5.14 SDS-TL) */
#define CONCAT_SLOTS 4
#define CONCAT_PARTS 8        /* 5 to 10 recommended (29.5.14.6) */
#define CONCAT_PART_BYTES 256 /* User defined data 4 is at most 2047 bits */
#define RAW_BYTES (CONCAT_PARTS * CONCAT_PART_BYTES + 1)

/* Large and not needed at boot: in PSRAM on the C62 (not zeroed) */
#ifdef PLATFORM_C62
#define SDS_BSS __attribute__((section(".psram_section")))
#else
#define SDS_BSS
#endif

struct concat {
    bool used;
    bool udh; /* UDH text (PID 0x8a): parts hold text only */
    uint32_t from, to;
    uint16_t ref;
    uint8_t nparts;
    uint16_t have; /* Bit i: part i + 1 received */
    uint8_t ppid;  /* Payload PID, from part 1 */
    uint8_t coding;
    long long t0;
    uint16_t bits[CONCAT_PARTS];
    uint8_t data[CONCAT_PARTS][CONCAT_PART_BYTES];
};

/* Bit reader over packed data, MSB first */
struct rd {
    const uint8_t *b;
    int n;
    int p;
    bool err;
};

static pthread_mutex_t sdsMutex = PTHREAD_MUTEX_INITIALIZER;
static SDS_BSS struct tetraSds store[TETRA_SDS_MAX]; /* Under sdsMutex */
static int storeHead;
static int storeCount;
static uint32_t storeSeq;

/* Decoding state, RTX thread only */
static SDS_BSS struct concat concats[CONCAT_SLOTS];
static SDS_BSS uint8_t raw[RAW_BYTES]; /* Reassembled payload */
static struct tetraSds cur;            /* Message being decoded */
static bool stored;                    /* cur was stored */

static uint32_t get(struct rd *r, int n)
{
    if (n < 0 || r->p + n > r->n) {
        r->err = true;
        r->p = r->n;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < n; i++, r->p++)
        v = (v << 1) | ((r->b[r->p >> 3] >> (7 - (r->p & 7))) & 1);
    return v;
}

/* Skip bits, any number */
static void skip(struct rd *r, int n)
{
    if (n < 0 || r->p + n > r->n) {
        r->err = true;
        r->p = r->n;
        return;
    }
    r->p += n;
}

static int left(const struct rd *r)
{
    return r->n - r->p;
}

const char *tetraSds_pidName(uint8_t pid)
{
    switch (pid & 0x7f) {
        case 0x01:
            return "OTAK";
        case 0x02:
            return pid & 0x80 ? "text" : "simple text";
        case 0x03:
            return pid & 0x80 ? "location system" : "simple location";
        case 0x04:
            return "WAP";
        case 0x05:
            return "WCMP";
        case 0x06:
            return "M-DMO";
        case 0x07:
            return "PIN authentication";
        case 0x08:
            return "end-to-end encrypted";
        case 0x09:
            return pid & 0x80 ? "immediate text" : "simple immediate text";
        case 0x0a:
            return pid & 0x80 ? "message with UDH" : "LIP";
        case 0x0b:
            return "NAP2";
        case 0x0c:
            return "concatenated SDS";
        case 0x0d:
            return pid & 0x80 ? "AGNSS" : "DOTAM";
        case 0x0e:
            return "simple AGNSS";
        default:
            break;
    }
    if ((pid & 0x7f) >= 0x40 && (pid & 0x7f) < 0x7f)
        return "user application";
    return "reserved";
}

static const char *deliveryStatus(uint8_t ds)
{
    switch (ds) {
        case 0x00:
            return "received";
        case 0x01:
            return "receipt acknowledged";
        case 0x02:
            return "consumed";
        case 0x03:
            return "consumed acknowledged";
        case 0x04:
            return "forwarded";
        case 0x05:
            return "sent to group";
        case 0x06:
            return "part received";
        case 0x20:
            return "congestion, stored";
        case 0x21:
            return "stored by SwMI";
        case 0x22:
            return "not reachable, stored";
        case 0x4a:
            return "delivery failed";
        case 0x4b:
            return "not registered";
        case 0x4c:
            return "queue full";
        case 0x50:
            return "protocol not supported";
        case 0x51:
            return "coding not supported";
        case 0x60:
            return "memory full";
        case 0x61:
            return "memory available";
        default:
            break;
    }
    if (ds < 0x20)
        return "success";
    if (ds < 0x40)
        return "temporary error";
    return ds < 0x60 ? "transfer failed" : "flow control";
}

static void begin(uint32_t from, uint32_t to)
{
    memset(&cur, 0, sizeof(cur));
    cur.from = from;
    cur.to = to;
}

static void commit(void)
{
    if (cur.kind == TETRA_SDS_KIND_E2EE)
        snprintf(cur.text, sizeof(cur.text), "end-to-end encrypted");

    pthread_mutex_lock(&sdsMutex);
    cur.tick = getTick();
    cur.seq = ++storeSeq;
    store[storeHead] = cur;
    storeHead = (storeHead + 1) % TETRA_SDS_MAX;
    if (storeCount < TETRA_SDS_MAX)
        storeCount++;
    pthread_mutex_unlock(&sdsMutex);
    stored = true;
}

/* ---- Text ---- */

/* Latin-1 / 7-bit GSM text to plain ASCII for the UI fonts */
static char asciiOf(uint8_t c)
{
    static const char hi[] =
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (c >= 0x20 && c < 0x7f)
        return (char)c;
    if (c >= 0xc0)
        return hi[c - 0xc0];
    return ' ';
}

static void textDecode(struct rd *r, uint8_t coding, int bits, char *out,
                       size_t len)
{
    size_t o = 0;
    if (coding == 0) {
        /* GSM 7-bit default alphabet, packed */
        uint8_t oct[TETRA_SDS_TEXT * 7 / 8 + 1];
        int no = 0;
        while (bits >= 8 && no < (int)sizeof(oct)) {
            oct[no++] = (uint8_t)get(r, 8);
            bits -= 8;
        }
        int nsep = no * 8 / 7;
        for (int i = 0; i < nsep && o + 1 < len; i++) {
            int bit = i * 7, byte = bit / 8, sh = bit % 8;
            uint16_t w = oct[byte];
            if (byte + 1 < no)
                w |= (uint16_t)(oct[byte + 1] << 8);
            uint8_t s = (uint8_t)((w >> sh) & 0x7f);
            out[o++] = s == 0    ? '@' :
                       s == 2    ? '$' :
                       s == 0x11 ? '_' :
                                   asciiOf(s);
        }
    } else {
        /* 8-bit alphabets (ISO 8859-x); UCS-2 shows its low bytes */
        const bool ucs2 = (coding == 0x1a);
        const int w = ucs2 ? 16 : 8;
        while (bits >= w && o + 1 < len) {
            uint8_t c = (uint8_t)get(r, w);
            bits -= w;
            if (c != 0)
                out[o++] = asciiOf(c);
        }
    }
    out[o] = '\0';
}

/* ---- LIP (ETSI TS 100 392-18-1) ---- */

/* Horizontal velocity, 7 bits: km/h, -1 unknown */
static int lipSpeed(uint32_t v)
{
    if (v == 127)
        return -1;
    if (v <= 28)
        return (int)v;
    /* 16 * 1.038^(v - 13), without powf */
    float s = 16.0f;
    for (uint32_t i = 13; i < v; i++)
        s *= 1.038f;
    return (int)(s + 0.5f);
}

/* Degrees with 5 decimals, without printf floats */
static int printDeg(char *out, size_t len, float deg)
{
    long v = (long)(deg * 100000.0f + (deg < 0 ? -0.5f : 0.5f));
    const char *sign = v < 0 ? "-" : "";
    if (v < 0)
        v = -v;
    return snprintf(out, len, "%s%ld.%05ld", sign, v / 100000, v % 100000);
}

static void lipPosition(int32_t latRaw, int32_t lonRaw, int speed, int dir)
{
    const float lat = (float)latRaw * (180.0f / 16777216.0f);
    const float lon = (float)lonRaw * (360.0f / 33554432.0f);
    int o = printDeg(cur.text, sizeof(cur.text), lat);
    cur.text[o++] = ' ';
    o += printDeg(cur.text + o, sizeof(cur.text) - o, lon);
    if (speed >= 0 && dir >= 0)
        snprintf(cur.text + o, sizeof(cur.text) - o, " %d km/h %d deg", speed,
                 dir);
    else if (speed >= 0)
        snprintf(cur.text + o, sizeof(cur.text) - o, " %d km/h", speed);
}

static void rxLip(struct rd *r, int len)
{
    const int end = r->p + len;
    const uint32_t type = get(r, 2);

    if (type == 0) {
        /* Short location report (6.2.1) */
        skip(r, 2); /* Time elapsed */
        const int32_t lon = (int32_t)(get(r, 25) << 7) >> 7;
        const int32_t lat = (int32_t)(get(r, 24) << 8) >> 8;
        skip(r, 3);                                    /* Position error */
        const int speed = lipSpeed(get(r, 7));
        const int dir = (int)(get(r, 4) * 225u / 10u); /* 22.5 degree steps */
        if (!r->err && r->p <= end) {
            lipPosition(lat, lon, speed, speed > 0 ? dir : -1);
            return;
        }
    } else if (type == 1 && get(r, 4) == 3) {
        /* Long location report (6.3.x): time, location shape, velocity */
        switch (get(r, 2)) {
            case 1:
                skip(r, 2); /* Time elapsed */
                break;
            case 2:
                skip(r, 22); /* Time of position */
                break;
            case 3:
                r->err = true;
                break;
            default:
                break;
        }
        /* Bits after longitude and latitude, for each shape with them */
        static const int8_t shapeRest[11] = { -1, 0,  6,  22, 12, 18,
                                              34, 21, 37, 49, 3 };
        const uint32_t shape = get(r, 4);
        if (!r->err && shape >= 1 && shape <= 10) {
            const int32_t lon = (int32_t)(get(r, 25) << 7) >> 7;
            const int32_t lat = (int32_t)(get(r, 24) << 8) >> 8;
            int speed = -1, dir = -1;
            skip(r, shapeRest[shape]);
            switch (get(r, 3)) { /* Velocity type */
                case 1:
                    speed = lipSpeed(get(r, 7));
                    break;
                case 2:
                    speed = lipSpeed(get(r, 7));
                    skip(r, 3);
                    break;
                case 5:
                    speed = lipSpeed(get(r, 7));
                    dir = (int)(get(r, 8) * 360u / 256u);
                    break;
                default:
                    break; /* Not decoded */
            }
            if (r->err || r->p > end)
                speed = dir = -1; /* The position came first: keep it */
            lipPosition(lat, lon, speed, dir);
            return;
        }
    }
    snprintf(cur.text, sizeof(cur.text), "(%s)",
             type == 1 ? "LIP request or other" : "no position");
}

/* ---- User data 4 ---- */

/* A user payload, after its PID and SDS-TL header, into cur */
static void payload(struct rd *r, uint8_t pid, int len)
{
    cur.pid = pid;
    switch (pid) {
        case 0x02: /* Simple text */
        case 0x09: /* Simple immediate text */
        case 0x82: /* Text (SDS-TL) */
        case 0x89: /* Immediate text (SDS-TL) */
        {
            const bool ts = get(r, 1) != 0 && (pid & 0x80);
            const uint8_t coding = (uint8_t)get(r, 7);
            len -= 8;
            if (ts) {
                skip(r, 24); /* Timestamp */
                len -= 24;
            }
            cur.kind = TETRA_SDS_KIND_TEXT;
            textDecode(r, coding, len, cur.text, sizeof(cur.text));
            break;
        }
        case 0x0a: /* Location information protocol */
            cur.kind = TETRA_SDS_KIND_LIP;
            rxLip(r, len);
            break;
        case 0x08: /* End-to-end encrypted: left alone */
        case 0x88:
            cur.kind = TETRA_SDS_KIND_E2EE;
            break;
        default:
            cur.kind = TETRA_SDS_KIND_OTHER;
            snprintf(cur.text, sizeof(cur.text), "PID 0x%02x %s, %d bits", pid,
                     tetraSds_pidName(pid), len > 0 ? len : 0);
            break;
    }
}

/* Append n packed bits to raw at bit position *at */
static void rawPut(int *at, const uint8_t *p, int n)
{
    for (int i = 0; i < n && *at < RAW_BYTES * 8; i++, (*at)++) {
        uint8_t bit = (p[i >> 3] >> (7 - (i & 7))) & 1;
        if (bit)
            raw[*at >> 3] |= (uint8_t)(0x80u >> (*at & 7));
    }
}

/* A reassembled (or timed out) concatenated message, stored */
static void concatFlush(struct concat *c)
{
    int got = 0;
    for (int i = 0; i < CONCAT_PARTS; i++)
        got += (c->have >> i) & 1;

    begin(c->from, c->to);
    cur.parts = c->nparts;
    cur.partsGot = (uint8_t)got;
    const bool part1 = (c->have & 1) != 0;

    memset(raw, 0, sizeof(raw));
    int bits = 0;
    if (!c->udh && part1)
        rawPut(&bits, &c->ppid, 8);
    for (int i = 0; i < CONCAT_PARTS && i < c->nparts; i++) {
        if (c->have & (1u << i))
            rawPut(&bits, c->data[i], c->bits[i]);
    }
    c->used = false;

    struct rd r = { raw, bits, 0, false };
    if (c->udh) {
        cur.pid = 0x8a;
        cur.kind = TETRA_SDS_KIND_TEXT;
        textDecode(&r, c->coding, bits, cur.text, sizeof(cur.text));
    } else if (part1) {
        skip(&r, 8);
        payload(&r, c->ppid, bits - 8);
    } else {
        cur.pid = 0x8c;
        cur.kind = TETRA_SDS_KIND_OTHER;
        snprintf(cur.text, sizeof(cur.text), "first part missing");
    }

    if (got < c->nparts) {
        char body[TETRA_SDS_TEXT];
        memcpy(body, cur.text, sizeof(body));
        /* The prefix takes the room of the end of the text */
        snprintf(cur.text, sizeof(cur.text), "[%d/%u] %.*s", got, c->nparts,
                 TETRA_SDS_TEXT - 26, body);
    }
    commit();
}

static void concatExpire(void)
{
    const long long now = getTick();
    for (int i = 0; i < CONCAT_SLOTS; i++) {
        if (concats[i].used && now - concats[i].t0 > TETRA_SDS_CONCAT_MS)
            concatFlush(&concats[i]);
    }
}

/* One concatenation part (cur: its addresses); r at its data, len bits */
static void concatAdd(bool udh, uint16_t ref, uint8_t n, uint8_t seq,
                      uint8_t ppid, uint8_t coding, struct rd *r, int len)
{
    const uint32_t from = cur.from, to = cur.to;
    struct concat *c = NULL, *oldest = NULL;

    for (int i = 0; i < CONCAT_SLOTS && c == NULL; i++) {
        struct concat *e = &concats[i];
        if (e->used && e->udh == udh && e->from == from && e->to == to
            && e->ref == ref && e->nparts == n)
            c = e;
    }
    if (c == NULL) {
        for (int i = 0; i < CONCAT_SLOTS && c == NULL; i++) {
            if (!concats[i].used)
                c = &concats[i];
            else if (oldest == NULL || concats[i].t0 < oldest->t0)
                oldest = &concats[i];
        }
        if (c == NULL) {
            concatFlush(oldest); /* Overwrites cur: from and to were copied */
            c = oldest;
        }
        memset(c, 0, sizeof(*c) - sizeof(c->data));
        c->used = true;
        c->udh = udh;
        c->from = from;
        c->to = to;
        c->ref = ref;
        c->nparts = n;
        c->t0 = getTick();
    }
    if (seq > CONCAT_PARTS || (c->have & (1u << (seq - 1))))
        return; /* Too many parts to keep, or a repetition */

    if (len > CONCAT_PART_BYTES * 8)
        len = CONCAT_PART_BYTES * 8;
    if (len > left(r))
        len = left(r);
    uint8_t *d = c->data[seq - 1];
    memset(d, 0, CONCAT_PART_BYTES);
    for (int i = 0; i < len; i++) {
        if (get(r, 1))
            d[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }
    c->bits[seq - 1] = (uint16_t)len;
    c->have |= (uint16_t)(1u << (seq - 1));
    if (seq == 1)
        c->ppid = ppid;
    if (udh)
        c->coding = coding;
    if (n <= CONCAT_PARTS && c->have == (uint16_t)((1u << n) - 1))
        concatFlush(c);
}

/* Message with user data header (29.5.9): concatenated or plain text */
static void udh(struct rd *r, int len)
{
    const bool ts = get(r, 1) != 0;
    const uint8_t coding = (uint8_t)get(r, 7);
    len -= 8;
    if (ts) {
        skip(r, 24);
        len -= 24;
    }
    const int udhl = (int)get(r, 8) * 8;
    len -= 8;
    const int end = r->p + udhl;
    int ref = -1, n = 0, seq = 0;
    while (!r->err && r->p + 16 <= end) {
        const uint32_t id = get(r, 8), l = get(r, 8);
        if (id == 0 && l == 3) {
            ref = (int)get(r, 8);
            n = (int)get(r, 8);
            seq = (int)get(r, 8);
        } else if (id == 8 && l == 4) {
            ref = (int)get(r, 16);
            n = (int)get(r, 8);
            seq = (int)get(r, 8);
        } else {
            skip(r, (int)l * 8);
        }
    }
    if (r->err || end > r->n) {
        cur.kind = TETRA_SDS_KIND_OTHER;
        snprintf(cur.text, sizeof(cur.text), "message with UDH, bad header");
        commit();
        return;
    }
    r->p = end;
    len -= udhl;
    if (ref >= 0 && n > 0 && seq > 0 && seq <= n) {
        concatAdd(true, (uint16_t)ref, (uint8_t)n, (uint8_t)seq, 0x8a, coding,
                  r, len);
        return;
    }
    cur.kind = TETRA_SDS_KIND_TEXT;
    textDecode(r, coding, len, cur.text, sizeof(cur.text));
    commit();
}

/* User defined data 4 (29.4): PID, SDS-TL header, payload */
static void userData4(struct rd *r)
{
    int len = left(r);
    if (len < 8)
        return;
    const uint8_t pid = (uint8_t)get(r, 8);
    len -= 8;
    cur.pid = pid;

    if (pid == 0x08 || pid == 0x88) {
        payload(r, pid, len);
        commit();
        return;
    }
    if (pid & 0x80) {
        /* SDS-TL header */
        const uint8_t mt = (uint8_t)get(r, 4);
        len -= 4;
        if (mt == 1 || mt == 2) {
            /* SDS-REPORT / SDS-ACK */
            skip(r, 4); /* Ack required, storage / reserved */
            const uint8_t ds = (uint8_t)get(r, 8);
            const uint8_t ref = (uint8_t)get(r, 8);
            cur.kind = TETRA_SDS_KIND_REPORT;
            snprintf(cur.text, sizeof(cur.text), "%s ref %u: %s",
                     mt == 1 ? "Report" : "Ack", ref, deliveryStatus(ds));
            commit();
            return;
        }
        if (mt != 0) {
            cur.kind = TETRA_SDS_KIND_OTHER;
            snprintf(cur.text, sizeof(cur.text),
                     "PID 0x%02x %s, SDS-TL type %u", pid,
                     tetraSds_pidName(pid), mt);
            commit();
            return;
        }
        skip(r, 3); /* Delivery report request, service selection */
        const bool storage = get(r, 1) != 0;
        skip(r, 8); /* Message reference */
        len -= 12;
        if (storage) {
            const int p0 = r->p;
            skip(r, 5); /* Validity period */
            switch (get(r, 3)) {
                case 0:
                    skip(r, 8);
                    break;
                case 1:
                    skip(r, 24);
                    break;
                case 2:
                    skip(r, 48);
                    break;
                case 3: {
                    int digits = (int)get(r, 8);
                    skip(r, 4 * (digits + (digits & 1)));
                    break;
                }
                default:
                    break;
            }
            len -= r->p - p0;
        }
    }
    if (r->err || len < 0)
        return;

    if (pid == 0x0c || pid == 0x8c) {
        /* Concatenation control (table 29.60) */
        const uint32_t type = get(r, 3);
        const bool ext = get(r, 1) != 0;
        uint16_t ref = ext ? (uint16_t)(get(r, 8) << 4) : 0;
        ref |= (uint16_t)get(r, 4);
        const uint8_t n = (uint8_t)get(r, 8), seq = (uint8_t)get(r, 8);
        len -= ext ? 32 : 24;
        uint8_t ppid = 0;
        if (seq == 1) {
            ppid = (uint8_t)get(r, 8);
            len -= 8;
        }
        if (!r->err && type == 0 && n > 0 && seq > 0 && seq <= n && len >= 0) {
            concatAdd(false, ref, n, seq, ppid, 0, r, len);
            return;
        }
        cur.kind = TETRA_SDS_KIND_OTHER;
        snprintf(cur.text, sizeof(cur.text), "concatenated, bad control");
        commit();
        return;
    }
    if (pid == 0x8a) {
        udh(r, len);
        return;
    }
    payload(r, pid, len);
    commit();
}

/* ---- Interface ---- */

void tetraSds_reset(void)
{
    pthread_mutex_lock(&sdsMutex);
    memset(store, 0, sizeof(store));
    storeHead = storeCount = 0;
    memset(concats, 0, sizeof(concats));
    pthread_mutex_unlock(&sdsMutex);
}

bool tetraSds_receive(const struct tetra_sds *s, struct tetraSds *out)
{
    /* Concatenations are only touched here, in the RTX thread */
    concatExpire();
    stored = false; /* Expired concatenations are not this message */

    begin(s->src_ssi, s->dst_ssi);
    struct rd r = { s->data, s->nbits, 0, false };

    switch (s->type) {
        case TETRA_SDS_STATUS: {
            const uint16_t st = s->status;
            if ((st >> 10) == 0x1f) {
                /* SDS-SHORT REPORT (29.4.2.3) */
                static const char *const shortReport[4] = {
                    "not supported", "memory full", "received", "consumed"
                };
                cur.kind = TETRA_SDS_KIND_REPORT;
                snprintf(cur.text, sizeof(cur.text), "Short report ref %u: %s",
                         st & 0xff, shortReport[(st >> 8) & 3]);
            } else {
                cur.kind = TETRA_SDS_KIND_STATUS;
                snprintf(cur.text, sizeof(cur.text), "Status %u (0x%04x)", st,
                         st);
            }
            commit();
            break;
        }

        case TETRA_SDS_UDD1:
        case TETRA_SDS_UDD2:
        case TETRA_SDS_UDD3: {
            cur.kind = TETRA_SDS_KIND_SHORT;
            int o = snprintf(cur.text, sizeof(cur.text),
                             "Type %d:", (int)s->type + 1);
            for (int i = 0; i < (s->nbits + 7) / 8 && o + 4 < TETRA_SDS_TEXT;
                 i++)
                o += snprintf(cur.text + o, sizeof(cur.text) - o, " %02x",
                              s->data[i]);
            commit();
            break;
        }

        case TETRA_SDS_UDD4:
            userData4(&r);
            break;

        default:
            break;
    }

    if (stored && out != NULL)
        *out = cur;
    return stored;
}

int tetraSds_get(struct tetraSds *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&sdsMutex);
    for (int i = 0; i < storeCount && n < max; i++)
        out[n++] = store[(storeHead - 1 - i + TETRA_SDS_MAX) % TETRA_SDS_MAX];
    pthread_mutex_unlock(&sdsMutex);
    return n;
}
