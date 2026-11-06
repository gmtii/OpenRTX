/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_lrrp.h"

#include <string.h>

/*
 * MBXML primitives (Motorola binary XML):
 *   uintvar   7 bits per byte, MSB first, bit 7 set on every byte but the last
 *   sintvar   as uintvar, but bit 6 of the first byte is the sign
 *   ufloatvar uintvar integer part, then uintvar fraction / 128^(its bytes)
 *   sfloatvar sintvar integer part, then uintvar fraction
 *   lat       32 bits: bit 31 = south, bits 30..0 = |lat| * 2^31 / 90
 *   long      32-bit two's complement, * 360 / 2^32 degrees
 *   info-time 40 bits: year(14) month(4) day(5) hour(5) min(6) sec(6), UTC
 */

typedef struct {
    const uint8_t *p;
    int pos, end;
    bool err;
} rd_t;

static uint32_t rd_uintvar(rd_t *r, int *nbytes)
{
    uint32_t v = 0;
    int n = 0;
    for (;;) {
        if (r->pos >= r->end || n >= 5) {
            r->err = true;
            return 0;
        }
        uint8_t b = r->p[r->pos++];
        v = (v << 7) | (b & 0x7Fu);
        n++;
        if (!(b & 0x80u)) {
            break;
        }
    }
    if (nbytes) {
        *nbytes = n;
    }
    return v;
}

static int32_t rd_sintvar(rd_t *r, bool *neg)
{
    uint32_t v;
    uint8_t b;
    *neg = false;
    if (r->pos >= r->end) {
        r->err = true;
        return 0;
    }
    b = r->p[r->pos++];
    *neg = (b & 0x40u) != 0;
    v = b & 0x3Fu;
    while (b & 0x80u) {
        if (r->pos >= r->end || v > 0xFFFFFFu) {
            r->err = true;
            return 0;
        }
        b = r->p[r->pos++];
        v = (v << 7) | (b & 0x7Fu);
    }
    return *neg ? -(int32_t)v : (int32_t)v;
}

static float rd_ufloatvar(rd_t *r)
{
    int n = 0;
    float i = (float)rd_uintvar(r, NULL);
    float f = (float)rd_uintvar(r, &n);
    float div = 1.0f;
    for (int k = 0; k < n; k++) {
        div *= 128.0f;
    }
    return i + f / div;
}

static float rd_sfloatvar(rd_t *r)
{
    bool neg = false;
    int n = 0;
    int32_t i = rd_sintvar(r, &neg);
    float f = (float)rd_uintvar(r, &n);
    float div = 1.0f, mag;
    for (int k = 0; k < n; k++) {
        div *= 128.0f;
    }
    mag = (float)(i < 0 ? -i : i) + f / div;
    return neg ? -mag : mag; /* "-0.x" keeps its sign */
}

static uint32_t rd_u32(rd_t *r)
{
    uint32_t v;
    if (r->pos + 4 > r->end) {
        r->err = true;
        return 0;
    }
    v = ((uint32_t)r->p[r->pos] << 24) | ((uint32_t)r->p[r->pos + 1] << 16)
      | ((uint32_t)r->p[r->pos + 2] << 8) | r->p[r->pos + 3];
    r->pos += 4;
    return v;
}

static uint8_t rd_u8(rd_t *r)
{
    if (r->pos >= r->end) {
        r->err = true;
        return 0;
    }
    return r->p[r->pos++];
}

static void rd_skip(rd_t *r, uint32_t n)
{
    if (n > (uint32_t)(r->end - r->pos)) {
        r->err = true;
        return;
    }
    r->pos += (int)n;
}

static void rd_latlon(rd_t *r, dmr_lrrp_t *o)
{
    uint32_t la = rd_u32(r);
    int32_t lo = (int32_t)rd_u32(r);
    double lat = (double)(la & 0x7FFFFFFFu) * (90.0 / 2147483648.0);
    if (la & 0x80000000u) {
        lat = -lat;
    }
    o->lat = lat;
    o->lon = (double)lo * (360.0 / 4294967296.0);
    o->has_pos = !r->err && lat >= -90.0 && lat <= 90.0;
}

static void rd_opaque_id(rd_t *r, dmr_lrrp_t *o, uint32_t n)
{
    uint32_t v = 0;
    if (n > (uint32_t)(r->end - r->pos)) {
        r->err = true;
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        v = (v << 8) | r->p[r->pos + (int)k];
    }
    r->pos += (int)n;
    o->has_request_id = true;
    o->request_id = v;
}

static bool is_request_doc(uint8_t id)
{
    return id == 0x04 || id == 0x05 || id == 0x08 || id == 0x09 || id == 0x0E
        || id == 0x0F || id == 0x14;
}

static bool is_report_doc(uint8_t id)
{
    return id == 0x06 || id == 0x07 || id == 0x0C || id == 0x0D || id == 0x12
        || id == 0x13;
}

const char *dmr_lrrp_doc_name(uint8_t id)
{
    switch (id) {
        case 0x04:
        case 0x05:
            return "Immediate-Location-Request";
        case 0x06:
        case 0x07:
            return "Immediate-Location-Report";
        case 0x08:
        case 0x09:
            return "Triggered-Location-Request";
        case 0x0A:
        case 0x0B:
            return "Triggered-Location-Answer";
        case 0x0C:
        case 0x0D:
            return "Triggered-Location-Report";
        case 0x0E:
        case 0x0F:
            return "Triggered-Location-Stop-Request";
        case 0x10:
        case 0x11:
            return "Triggered-Location-Stop-Answer";
        case 0x12:
        case 0x13:
            return "Unsolicited-Location-Report";
        case 0x14:
            return "Location-Protocol-Request";
        case 0x15:
            return "Location-Protocol-Report";
        default:
            return "LRRP";
    }
}

/* One token of a request document. Returns false on an unknown token. */
static bool request_token(rd_t *r, uint32_t tok, dmr_lrrp_t *o)
{
    switch (tok) {
        case 0x22:
            rd_opaque_id(r, o, rd_uintvar(r, NULL));
            return true;
        case 0x23:
            rd_opaque_id(r, o, 1);
            return true;
        case 0x24:
            (void)rd_uintvar(r, NULL);
            return true;
        case 0x31:
            o->interval_s = (int)rd_uintvar(r, NULL);
            return true;
        case 0x33:
        case 0x34: /* oneshot / periodic trigger */
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53: /* ret-info */
        case 0x54:
        case 0x57:
        case 0x62:
        case 0x64: /* request-... flags */
        case 0x66:
        case 0x69:
        case 0x74:
        case 0x76: /* require-... flags */
            return true;
        case 0x3F:
        case 0x42:
        case 0x4A:
        case 0x55:
        case 0x5F:
        case 0x67:
        case 0x71:
            (void)rd_uintvar(r, NULL);
            return true;
        case 0x56:
        case 0x60:
        case 0x68:
        case 0x72:
            (void)rd_ufloatvar(r);
            return true;
        case 0x61:
        case 0x73:
            (void)rd_u8(r);
            return true;
        default:
            return false;
    }
}

/* One token of a report / answer document. *uncertain is set after tokens
 * whose layout is not confirmed by a test vector (see dmr_lrrp_parse).
 * Returns false on an unknown token. */
static bool report_token(rd_t *r, uint32_t tok, dmr_lrrp_t *o, bool *uncertain)
{
    switch (tok) {
        case 0x22:
            rd_opaque_id(r, o, rd_uintvar(r, NULL));
            return true;
        case 0x23:
            rd_opaque_id(r, o, 1);
            return true;
        case 0x24:
            (void)rd_uintvar(r, NULL);
            return true;

        case 0x51: /* circle-2d: lat, long, radius */
            rd_latlon(r, o);
            o->radius_m = rd_ufloatvar(r);
            o->has_radius = !r->err;
            return true;
        case 0x54:
        case 0x55: /* circle-3d: lat, long, radius, altitude, altitude-acc */
            rd_latlon(r, o);
            o->radius_m = rd_ufloatvar(r);
            o->has_radius = !r->err;
            o->alt_m = rd_sfloatvar(r);
            o->has_alt = !r->err;
            (void)rd_ufloatvar(r);
            *uncertain = true;
            return true;
        case 0x66: /* point-2d */
            rd_latlon(r, o);
            return true;
        case 0x69: /* point-3d: lat, long, altitude */
            rd_latlon(r, o);
            o->alt_m = rd_sfloatvar(r);
            o->has_alt = !r->err;
            return true;
        case 0x6A: /* point-3d with altitude accuracy */
            rd_latlon(r, o);
            o->alt_m = rd_sfloatvar(r);
            o->has_alt = !r->err;
            (void)rd_ufloatvar(r);
            *uncertain = true;
            return true;

        case 0x34:
        case 0x35: /* info-time */
            if (r->pos + 5 > r->end) {
                r->err = true;
                return true;
            }
            {
                uint64_t v = 0;
                for (int k = 0; k < 5; k++) {
                    v = (v << 8) | r->p[r->pos + k];
                }
                r->pos += 5;
                o->year = (uint16_t)(v >> 26);
                o->month = (uint8_t)((v >> 22) & 0xFu);
                o->day = (uint8_t)((v >> 17) & 0x1Fu);
                o->hour = (uint8_t)((v >> 12) & 0x1Fu);
                o->min = (uint8_t)((v >> 6) & 0x3Fu);
                o->sec = (uint8_t)(v & 0x3Fu);
                o->has_time = o->month >= 1 && o->month <= 12 && o->day >= 1
                           && o->hour < 24 && o->min < 60 && o->sec < 61;
            }
            return true;

        case 0x56: /* direction-hor, 2 degree units */
            o->dir_deg = rd_u8(r) * 2;
            o->has_dir = !r->err && o->dir_deg < 360;
            return true;
        case 0x6C: /* speed-hor, m/s */
            o->speed_mps = rd_ufloatvar(r);
            o->has_speed = !r->err;
            return true;
        case 0x70: /* speed-vrt, m/s */
            o->vspeed_mps = rd_sfloatvar(r);
            o->has_vspeed = !r->err;
            return true;
        case 0x65:
        case 0x6B: /* lev-conf, unknown uint8 */
            (void)rd_u8(r);
            return true;
        case 0x36: /* protocol-version */
            (void)rd_uintvar(r, NULL);
            return true;
        case 0x37: /* result, explicit result-code */
            o->result = (int)rd_uintvar(r, NULL);
            *uncertain = true;
            return true;
        case 0x38: /* result, result-code 0 */
            o->result = 0;
            return true;
        case 0x39: /* operation-error: result-code, opaque */
            o->result = (int)rd_uintvar(r, NULL);
            rd_skip(r, rd_uintvar(r, NULL));
            return true;
        default:
            return false;
    }
}

bool dmr_lrrp_parse(const uint8_t *p, int len, dmr_lrrp_t *out)
{
    rd_t r = { p, 0, len, false };
    uint32_t doc_id, doc_len;
    bool request, uncertain = false;
    dmr_lrrp_t snap;

    memset(out, 0, sizeof(*out));
    out->result = -1;
    out->interval_s = -1;

    doc_id = rd_uintvar(&r, NULL);
    if (r.err || doc_id < 0x04 || doc_id > 0x15) {
        return false;
    }
    doc_len = rd_uintvar(&r, NULL);
    if (r.err || doc_len > (uint32_t)(len - r.pos)) {
        return false;
    }
    r.end = r.pos + (int)doc_len;
    out->doc_id = (uint8_t)doc_id;
    request = is_request_doc(out->doc_id);
    out->kind = request                    ? DMR_LRRP_REQUEST :
                is_report_doc(out->doc_id) ? DMR_LRRP_REPORT :
                                             DMR_LRRP_ANSWER;

    if (!(doc_id & 1u) && doc_id != 0x14) {
        /* document with a constant data table: skip it (1 = "same as the
         * previous document", nothing to skip) */
        uint32_t cdt = rd_uintvar(&r, NULL);
        if (cdt > 1) {
            rd_skip(&r, cdt);
        }
    }

    snap = *out;
    while (!r.err && r.pos < r.end) {
        uint32_t tok;
        bool known;
        if (!uncertain) {
            snap = *out; /* everything up to here rests on confirmed layouts */
        }
        tok = rd_uintvar(&r, NULL);
        known = !r.err
             && (request ? request_token(&r, tok, out) :
                           report_token(&r, tok, out, &uncertain));
        if (!known || r.err) {
            r.err = true;
            break;
        }
    }
    if (r.err && uncertain) {
        /* a token with an unconfirmed layout was followed by a parse failure:
         * keep only what was read before it (the position itself is read by
         * the confirmed part of that token, and survives when it was first) */
        bool pos = out->has_pos;
        double lat = out->lat, lon = out->lon;
        *out = snap;
        if (pos && !out->has_pos) {
            out->has_pos = true;
            out->lat = lat;
            out->lon = lon;
        }
    }
    return true;
}
