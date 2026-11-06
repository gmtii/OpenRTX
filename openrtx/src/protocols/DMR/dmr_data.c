/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_data.h"
#include "dmr_fec.h"
#include "dmr_trellis.h"

#include <string.h>

#define DMR_CRC_MASK_DATA_HEADER 0xCCCCu
#define CRC9_MASK_RATE_12 0x0F0u
#define CRC9_MASK_RATE_34 0x1FFu
#define CRC9_MASK_RATE_1 0x10Fu

/* A confirmed packet waits this long for a selective retry. */
#define CONFIRMED_HOLD_MS 15000u
/* Identical packets (same source, same CRC-32) within this window are
 * reported once: confirmed retries and repeated unconfirmed sends. */
#define DUP_WINDOW_MS 10000u

typedef struct {
    bool collecting; /* blocks of this packet are expected now */
    bool pending;    /* buffer holds a (partial) packet */
    dmr_data_header_t hdr;
    int total;       /* blocks in the whole packet */
    int expect;      /* blocks still to come in this attempt */
    int blen;        /* user octets per block (0 = not yet known) */
    int rate_dt;
    int count;       /* unconfirmed: blocks stored so far */
    uint8_t have[(DMR_DATA_MAX_BLOCKS + 7) / 8];
    uint8_t buf[DMR_DATA_MAX_BLOCKS * 24];
    uint32_t t_ms;
    /* duplicate suppression */
    bool last_valid;
    uint32_t last_src, last_crc, last_ms;
} asm_t;

static asm_t s_asm[2];
static dmr_data_stats_t s_stats;

/* ---- CRCs ------------------------------------------------------------------ */

/* CRC-9 (x^9+x^6+x^4+x^3+1) of a confirmed block: its user octets (for the
 * last block including the CRC-32) followed by the 7-bit DBSN, inverted and
 * masked per data rate (ETSI TS 102 361-1 B.3.10). */
uint16_t dmr_crc9(const uint8_t *bytes, int nbytes, int dbsn, uint16_t mask)
{
    uint16_t crc = 0;
    const int n = nbytes * 8 + 7;
    for (int i = 0; i < n; i++) {
        int bit = (i < nbytes * 8) ? (bytes[i >> 3] >> (7 - (i & 7))) & 1 :
                                     (dbsn >> (6 - (i - nbytes * 8))) & 1;
        if (((crc >> 8) & 1u) ^ (unsigned)bit) {
            crc = (uint16_t)(((crc << 1) ^ 0x059u) & 0x1FFu);
        } else {
            crc = (uint16_t)((crc << 1) & 0x1FFu);
        }
    }
    return (uint16_t)((crc ^ 0x1FFu) ^ mask);
}

/* CRC-32 of a packet (x^32+x^26+...+1, 0x04C11DB7, init 0, no final XOR),
 * over the octets in pairs swapped (2nd, 1st, 4th, 3rd...); it is sent
 * least significant octet first (B.3.9). Checked against ok-dmrlib's
 * test_crc32.py vectors. */
uint32_t dmr_crc32(const uint8_t *bytes, int nbytes)
{
    uint32_t crc = 0;
    for (int i = 0; i < nbytes; i++) {
        int j = i ^ 1;
        uint8_t b = (j < nbytes) ?
                        bytes[j] :
                        0; /* odd length never occurs: blocks are even */
        for (int k = 7; k >= 0; k--) {
            if (((crc >> 31) & 1u) ^ ((b >> k) & 1u)) {
                crc = (crc << 1) ^ 0x04C11DB7u;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ---- Helpers ------------------------------------------------------------- */

static void bits_to_bytes(const uint8_t *bits, int nbytes, uint8_t *out)
{
    for (int i = 0; i < nbytes; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) {
            v = (uint8_t)((v << 1) | (bits[i * 8 + k] & 1u));
        }
        out[i] = v;
    }
}

static uint32_t be24(const uint8_t *b)
{
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}

const char *dmr_sap_name(uint8_t sap)
{
    switch (sap) {
        case DMR_SAP_UDT:
            return "UDT";
        case DMR_SAP_TCPIP_COMP:
            return "TCP/IP comp";
        case DMR_SAP_UDPIP_COMP:
            return "UDP/IP comp";
        case DMR_SAP_IP:
            return "IP";
        case DMR_SAP_ARP:
            return "ARP";
        case DMR_SAP_PROPRIETARY:
            return "proprietary";
        case DMR_SAP_SHORT_DATA:
            return "short data";
        default:
            return "reserved";
    }
}

void dmr_data_reset(void)
{
    memset(s_asm, 0, sizeof(s_asm));
    memset(&s_stats, 0, sizeof(s_stats));
}

void dmr_data_get_stats(dmr_data_stats_t *out)
{
    *out = s_stats;
}

void dmr_data_interrupt(int slot)
{
    asm_t *a = &s_asm[slot & 1];
    a->collecting = false;
    if (a->pending && a->hdr.dpf != DMR_DPF_CONFIRMED) {
        a->pending = false; /* unconfirmed blocks are never repeated */
    }
}

/* ---- Data header ------------------------------------------------------------ */

static void parse_header(const uint8_t b[12], dmr_data_header_t *h)
{
    memset(h, 0, sizeof(*h));
    h->dpf = b[0] & 0x0Fu;
    h->group = (b[0] & 0x80u) != 0;
    h->resp_req = (b[0] & 0x40u) != 0;
    h->sap = (uint8_t)(b[1] >> 4);
    h->dst = be24(&b[2]);
    h->src = be24(&b[5]);
    switch (h->dpf) {
        case DMR_DPF_UNCONFIRMED:
        case DMR_DPF_CONFIRMED:
            h->poc = ((b[0] >> 4) & 1) << 4 | (b[1] & 0x0F);
            h->full = (b[8] & 0x80u) != 0;
            h->btf = b[8] & 0x7F;
            h->fsn = b[9] & 0x0F;
            if (h->dpf == DMR_DPF_CONFIRMED) {
                h->ns = (b[9] >> 4) & 7;
            }
            break;
        case DMR_DPF_RESPONSE:
            h->full = (b[8] & 0x80u) != 0;
            h->btf = b[8] & 0x7F;
            break;
        case DMR_DPF_SHORT_DEFINED:
        case DMR_DPF_SHORT_RAW:
            h->btf = ((b[0] >> 4) & 3) << 4
                   | (b[1] & 0x0F); /* appended blocks */
            h->full = true;
            break;
        default:
            break;
    }
}

bool dmr_data_header(int slot, const uint8_t data[96], uint32_t t_ms,
                     dmr_data_header_t *out)
{
    asm_t *a = &s_asm[slot & 1];
    uint8_t b[12];
    dmr_data_header_t h;
    uint16_t crc = (uint16_t)dmr_bits_to_u32(&data[80], 16);

    if ((uint16_t)(dmr_crc_ccitt(data, 80) ^ DMR_CRC_MASK_DATA_HEADER) != crc) {
        s_stats.headers_bad++;
        return false;
    }
    s_stats.headers++;
    bits_to_bytes(data, 12, b);
    parse_header(b, &h);
    *out = h;

    a->collecting = false;
    if (h.dpf != DMR_DPF_UNCONFIRMED && h.dpf != DMR_DPF_CONFIRMED
        && h.dpf != DMR_DPF_SHORT_DEFINED && h.dpf != DMR_DPF_SHORT_RAW) {
        /* responses (ACK / NACK), UDT, proprietary headers: nothing to
         * assemble here. A confirmed packet stays pending for its retry. */
        if (a->pending && a->hdr.dpf != DMR_DPF_CONFIRMED) {
            a->pending = false;
        }
        return true;
    }
    if (h.btf < 1) {
        return true;
    }

    if (h.dpf == DMR_DPF_CONFIRMED && !h.full && a->pending
        && a->hdr.dpf == DMR_DPF_CONFIRMED && a->hdr.src == h.src
        && a->hdr.dst == h.dst && a->hdr.ns == h.ns
        && (t_ms - a->t_ms) < CONFIRMED_HOLD_MS) {
        /* selective retry of the pending packet: keep the blocks we have */
        a->collecting = true;
        a->expect = h.btf;
        a->t_ms = t_ms;
        return true;
    }

    if (h.btf > DMR_DATA_MAX_BLOCKS) {
        s_stats.too_long++;
        a->pending = false;
        return true;
    }
    a->hdr = h;
    a->collecting = true;
    a->pending = true;
    a->total = h.btf;
    a->expect = h.btf;
    a->blen = 0;
    a->rate_dt = 0;
    a->count = 0;
    a->t_ms = t_ms;
    memset(a->have, 0, sizeof(a->have));
    return true;
}

/* ---- IP / UDP --------------------------------------------------------------- */

static void decode_udp_payload(dmr_packet_t *pk, const uint8_t *p, int n)
{
    pk->udp_len = n;
    if (pk->src_port == 4001 || pk->dst_port == 4001) {
        pk->lrrp_valid = dmr_lrrp_parse(p, n, &pk->lrrp);
        return;
    }
    if (pk->src_port == 4005 || pk->dst_port == 4005 || pk->src_port == 4007
        || pk->dst_port == 4007) {
        return; /* ARS registration / TMS text message */
    }
    /* Port not sent as a number (compressed header port IDs) or unusual:
     * accept LRRP only when the document length accounts for every octet. */
    if (n >= 2 && n < 128 && p[1] < 0x80 && p[1] + 2 == n
        && dmr_lrrp_parse(p, n, &pk->lrrp)) {
        pk->lrrp_valid = true;
    }
}

static void decode_ip(dmr_packet_t *pk, const uint8_t *p, int n)
{
    if (pk->hdr.sap == DMR_SAP_IP) {
        int ihl, tot;
        if (n < 28 || (p[0] >> 4) != 4) {
            return;
        }
        ihl = (p[0] & 0x0F) * 4;
        tot = (p[2] << 8) | p[3];
        if (ihl < 20 || tot > n || tot < ihl + 8 || p[9] != 17 /* UDP */) {
            return;
        }
        pk->ip_src = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16)
                   | ((uint32_t)p[14] << 8) | p[15];
        pk->ip_dst = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16)
                   | ((uint32_t)p[18] << 8) | p[19];
        p += ihl;
        n = tot - ihl;
        {
            int ulen = (p[4] << 8) | p[5];
            pk->udp = true;
            pk->src_port = (uint16_t)((p[0] << 8) | p[1]);
            pk->dst_port = (uint16_t)((p[2] << 8) | p[3]);
            if (ulen < 8 || ulen > n) {
                ulen = n;
            }
            decode_udp_payload(pk, p + 8, ulen - 8);
        }
    } else if (pk->hdr.sap == DMR_SAP_UDPIP_COMP) {
        /* ETSI TS 102 361-3 7.2.4: IPv4 identification (16), source and
         * destination address IDs (4 + 4), then source and destination port
         * IDs (7 bits each, after a 1-bit opcode); port ID 0 = the port
         * number follows as a 16-bit extension, source first. */
        int off = 5;
        if (n < 5) {
            return;
        }
        pk->compressed = true;
        pk->udp = true;
        pk->src_port_id = p[3] & 0x7F;
        pk->dst_port_id = p[4] & 0x7F;
        if (pk->src_port_id == 0) {
            if (n < off + 2) {
                return;
            }
            pk->src_port = (uint16_t)((p[off] << 8) | p[off + 1]);
            off += 2;
        }
        if (pk->dst_port_id == 0) {
            if (n < off + 2) {
                return;
            }
            pk->dst_port = (uint16_t)((p[off] << 8) | p[off + 1]);
            off += 2;
        }
        decode_udp_payload(pk, p + off, n - off);
    }
}

/* ---- Data blocks ------------------------------------------------------------ */

int dmr_data_block(int slot, int dt, const uint8_t info[196], uint32_t t_ms,
                   dmr_packet_t *out)
{
    asm_t *a = &s_asm[slot & 1];
    uint8_t raw[24];
    int rawlen, blen, idx;
    bool confirmed;
    uint16_t mask;

    if (!a->collecting) {
        return DMR_DATA_IGNORED;
    }
    s_stats.blocks++;
    confirmed = (a->hdr.dpf == DMR_DPF_CONFIRMED);

    switch (dt) {
        case DMR_DATA_DT_RATE_12: {
            uint8_t bits[96];
            (void)dmr_bptc196_decode(
                info, bits); /* leftovers are caught by the CRCs */
            bits_to_bytes(bits, 12, raw);
            rawlen = 12;
            mask = CRC9_MASK_RATE_12;
            break;
        }
        case DMR_DATA_DT_RATE_34:
            (void)dmr_trellis34_decode(info, raw);
            rawlen = 18;
            mask = CRC9_MASK_RATE_34;
            break;
        case DMR_DATA_DT_RATE_1:
            /* uncoded: 96 bits, 4 reserved bits, 96 bits */
            bits_to_bytes(info, 12, raw);
            bits_to_bytes(&info[100], 12, &raw[12]);
            rawlen = 24;
            mask = CRC9_MASK_RATE_1;
            break;
        default:
            return DMR_DATA_IGNORED;
    }

    blen = confirmed ? rawlen - 2 : rawlen;
    if (a->blen == 0) {
        a->blen = blen;
        a->rate_dt = dt;
    } else if (a->rate_dt != dt) {
        a->collecting = false; /* the rate cannot change within a packet */
        a->pending = false;
        return DMR_DATA_CRC_BAD;
    }

    if (confirmed) {
        int dbsn = raw[0] >> 1;
        uint16_t crc9 = (uint16_t)(((raw[0] & 1u) << 8) | raw[1]);
        if (dmr_crc9(&raw[2], blen, dbsn, mask) != crc9 || dbsn >= a->total) {
            s_stats.crc9_bad++;
            idx = -1; /* a retry may bring it again */
        } else {
            idx = dbsn;
        }
        if (idx >= 0) {
            memcpy(&a->buf[idx * blen], &raw[2], (size_t)blen);
            a->have[idx >> 3] |= (uint8_t)(1u << (idx & 7));
        }
    } else {
        idx = a->count++;
        if (idx < a->total) {
            memcpy(&a->buf[idx * blen], raw, (size_t)blen);
        }
    }

    if (--a->expect <= 0) {
        a->collecting = false;
    }

    /* complete? */
    if (confirmed) {
        for (int i = 0; i < a->total; i++) {
            if (!(a->have[i >> 3] & (1u << (i & 7)))) {
                /* a gap: more blocks of this attempt to come, or (attempt
                 * over) the packet stays pending for a selective retry */
                return DMR_DATA_PENDING;
            }
        }
    } else if (a->count < a->total) {
        return DMR_DATA_PENDING;
    }

    /* all blocks here: check the packet CRC-32 */
    a->collecting = false;
    a->pending = false;
    {
        const int nbytes = a->total * a->blen;
        const uint8_t *c = &a->buf[nbytes - 4];
        uint32_t rx = (uint32_t)c[0] | ((uint32_t)c[1] << 8)
                    | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
        uint32_t calc = dmr_crc32(a->buf, nbytes - 4);
        int ulen = nbytes - 4 - a->hdr.poc;

        if (calc != rx || ulen < 0) {
            s_stats.crc32_bad++;
            return DMR_DATA_CRC_BAD;
        }
        if (a->last_valid && a->last_src == a->hdr.src && a->last_crc == rx
            && (t_ms - a->last_ms) < DUP_WINDOW_MS) {
            s_stats.dup++;
            a->last_ms = t_ms;
            return DMR_DATA_DUP;
        }
        a->last_valid = true;
        a->last_src = a->hdr.src;
        a->last_crc = rx;
        a->last_ms = t_ms;
        s_stats.packets++;

        memset(out, 0, sizeof(*out));
        out->hdr = a->hdr;
        out->len = ulen;
        out->crc32 = rx;
        decode_ip(out, a->buf, ulen);
        if (out->lrrp_valid) {
            s_stats.lrrp++;
        }
    }
    return DMR_DATA_PACKET;
}
