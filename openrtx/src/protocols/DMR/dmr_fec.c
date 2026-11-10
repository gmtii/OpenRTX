/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_fec.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Generic systematic cyclic-code parity                                      */
/* ------------------------------------------------------------------------- */

/* Remainder of data(x) * x^deg modulo gen(x), data given MSB-first as a k-bit
 * number. A shortened code is the full code with leading zero data bits, which
 * never change the remainder - so shortening needs no special handling. */
static uint32_t cyc_parity(uint32_t data, int k, uint32_t gen, int deg)
{
    uint32_t reg = data << deg;
    for (int i = k + deg - 1; i >= deg; i--) {
        if (reg & (1u << i)) {
            reg ^= gen << (i - deg);
        }
    }
    return reg & ((1u << deg) - 1u);
}

static uint32_t parity_bit(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

static int popcount32(uint32_t v)
{
    int c = 0;
    while (v) {
        v &= v - 1u;
        c++;
    }
    return c;
}

#define G_HAM3 0xBu    /* x^3+x+1 */
#define G_HAM4 0x13u   /* x^4+x+1 */
#define G_GOLAY 0xC75u /* x^11+x^10+x^6+x^5+x^4+x^2+1 */
#define G_QR 0x139u    /* x^8+x^5+x^4+x^3+1 */

uint32_t dmr_hamming_7_4_encode(uint32_t d)
{
    return (d << 3) | cyc_parity(d, 4, G_HAM3, 3);
}
uint32_t dmr_hamming_15_11_encode(uint32_t d)
{
    return (d << 4) | cyc_parity(d, 11, G_HAM4, 4);
}
uint32_t dmr_hamming_13_9_encode(uint32_t d)
{
    return (d << 4) | cyc_parity(d, 9, G_HAM4, 4);
}

uint32_t dmr_hamming_16_11_encode(uint32_t d)
{
    uint32_t cw15 = dmr_hamming_15_11_encode(d);
    return (cw15 << 1) | parity_bit(cw15);
}

uint32_t dmr_golay_20_8_encode(uint32_t d)
{
    uint32_t cw19 = (d << 11) | cyc_parity(d, 8, G_GOLAY, 11);
    return (cw19 << 1) | parity_bit(cw19);
}

uint32_t dmr_qr_16_7_encode(uint32_t d)
{
    uint32_t cw15 = (d << 8) | cyc_parity(d, 7, G_QR, 8);
    return (cw15 << 1) | parity_bit(cw15);
}

/* Single-error correction for the cyclic Hamming codes: the syndrome of a
 * single error at position p equals the parity of the unit vector there. */
static bool hamming_cyclic_decode(uint32_t *cw, int n, int k, uint32_t gen,
                                  int deg)
{
    uint32_t data = *cw >> deg;
    uint32_t syn = (*cw & ((1u << deg) - 1u)) ^ cyc_parity(data, k, gen, deg);

    if (syn == 0) {
        return true;
    }
    for (int p = 0; p < n; p++) {
        uint32_t e = 1u << p;
        uint32_t esyn = (e & ((1u << deg) - 1u))
                      ^ cyc_parity(e >> deg, k, gen, deg);
        if (esyn == syn) {
            *cw ^= e;
            return true;
        }
    }
    return false;
}

bool dmr_hamming_7_4_decode(uint32_t *cw)
{
    return hamming_cyclic_decode(cw, 7, 4, G_HAM3, 3);
}
bool dmr_hamming_15_11_decode(uint32_t *cw)
{
    return hamming_cyclic_decode(cw, 15, 11, G_HAM4, 4);
}
bool dmr_hamming_13_9_decode(uint32_t *cw)
{
    return hamming_cyclic_decode(cw, 13, 9, G_HAM4, 4);
}

/* (16,11,4): correct 1 error, detect 2 (overall parity tells them apart). */
bool dmr_hamming_16_11_decode(uint32_t *cw)
{
    uint32_t cw15 = *cw >> 1;
    bool overall_ok = parity_bit(*cw) == 0u;
    uint32_t syn = (cw15 & 0xFu) ^ cyc_parity(cw15 >> 4, 11, G_HAM4, 4);

    if (syn == 0) {
        if (!overall_ok) {
            *cw ^= 1u; /* error in the overall parity bit itself */
        }
        return true;
    }
    if (overall_ok) {
        return false; /* even number of errors: detected, not correctable */
    }
    if (!hamming_cyclic_decode(&cw15, 15, 11, G_HAM4, 4)) {
        return false;
    }
    *cw = (cw15 << 1) | parity_bit(cw15);
    return true;
}

/* Golay(20,8) and QR(16,7) have only 256 / 128 codewords: a brute-force
 * nearest-codeword search is simple, obviously correct and cheap at DMR's
 * ~33 bursts/s. Correction limits follow the minimum distances measured in
 * the host test: d=8 -> 3 errors for Golay(20,8), d=6 -> 2 for QR(16,7). */
static bool nearest_decode(uint32_t *cw, int k, uint32_t (*enc)(uint32_t),
                           int t, int *errors)
{
    int best = 99;
    uint32_t best_cw = 0;
    for (uint32_t d = 0; d < (1u << k); d++) {
        uint32_t c = enc(d);
        int dist = popcount32(c ^ *cw);
        if (dist < best) {
            best = dist;
            best_cw = c;
            if (dist == 0) {
                break;
            }
        }
    }
    if (errors != NULL) {
        *errors = best;
    }
    if (best > t) {
        return false;
    }
    *cw = best_cw;
    return true;
}

bool dmr_golay_20_8_decode(uint32_t *cw)
{
    return nearest_decode(cw, 8, dmr_golay_20_8_encode, 3, NULL);
}
bool dmr_qr_16_7_decode(uint32_t *cw)
{
    return nearest_decode(cw, 7, dmr_qr_16_7_encode, 2, NULL);
}
bool dmr_golay_20_8_decode_errors(uint32_t *cw, int *errors)
{
    return nearest_decode(cw, 8, dmr_golay_20_8_encode, 3, errors);
}
bool dmr_qr_16_7_decode_errors(uint32_t *cw, int *errors)
{
    return nearest_decode(cw, 7, dmr_qr_16_7_encode, 2, errors);
}

/* ------------------------------------------------------------------------- */
/* Bit helpers                                                                */
/* ------------------------------------------------------------------------- */

uint32_t dmr_bits_to_u32(const uint8_t *bits, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 1) | (bits[i] & 1u);
    }
    return v;
}

void dmr_u32_to_bits(uint32_t v, int n, uint8_t *bits)
{
    for (int i = 0; i < n; i++) {
        bits[i] = (uint8_t)((v >> (n - 1 - i)) & 1u);
    }
}

/* ------------------------------------------------------------------------- */
/* BPTC(196,96)                                                               */
/* ------------------------------------------------------------------------- */
/*
 * ETSI TS 102 361-1 B.1.1: a 13 x 15 matrix preceded by one reserved bit
 * R(3). Rows 0..8 carry 11 data bits + Hamming(15,11) parity (row 0 starts
 * with 3 reserved bits R(2..0), so it holds only 8 data bits); rows 9..12 are
 * Hamming(13,9) column parity. Matrix bit a is TRANSMITTED at position
 * (a * 181) mod 196 (equivalently, received bit i lands at matrix position
 * (i * 13) mod 196, 13 being the inverse of 181 mod 196). Getting this
 * direction backwards is self-consistent - our own encoder and decoder still
 * agree - so it was only caught by feeding our encoding to an independent
 * decoder (dsd-fme); see test/host_dmr/README.
 */
int dmr_bptc196_decode(const uint8_t in[196], uint8_t out[96])
{
    uint8_t lin[196];
    uint8_t m[13][15];
    int bad = 0;

    for (int a = 0; a < 196; a++) {
        lin[a] = in[(a * 181) % 196] & 1u;
    }
    for (int r = 0; r < 13; r++) {
        for (int c = 0; c < 15; c++) {
            m[r][c] = lin[1 + r * 15 + c];
        }
    }

    /* rows, columns, rows again (a column fix can make a failed row decodable) */
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < 9; r++) {
            uint32_t cw = dmr_bits_to_u32(m[r], 15);
            if (dmr_hamming_15_11_decode(&cw)) {
                dmr_u32_to_bits(cw, 15, m[r]);
            }
        }
        for (int c = 0; c < 15; c++) {
            uint8_t col[13];
            uint32_t cw;
            for (int r = 0; r < 13; r++) {
                col[r] = m[r][c];
            }
            cw = dmr_bits_to_u32(col, 13);
            if (dmr_hamming_13_9_decode(&cw)) {
                dmr_u32_to_bits(cw, 13, col);
                for (int r = 0; r < 13; r++) {
                    m[r][c] = col[r];
                }
            }
        }
    }

    /* final verdict */
    for (int r = 0; r < 9; r++) {
        uint32_t cw = dmr_bits_to_u32(m[r], 15);
        uint32_t orig = cw;
        if (!dmr_hamming_15_11_decode(&cw) || cw != orig) {
            bad++;
        }
    }
    for (int c = 0; c < 15; c++) {
        uint8_t col[13];
        uint32_t cw, orig;
        for (int r = 0; r < 13; r++) {
            col[r] = m[r][c];
        }
        cw = orig = dmr_bits_to_u32(col, 13);
        if (!dmr_hamming_13_9_decode(&cw) || cw != orig) {
            bad++;
        }
    }

    {
        int k = 0;
        for (int c = 3; c < 11; c++) {
            out[k++] = m[0][c];
        }
        for (int r = 1; r < 9; r++) {
            for (int c = 0; c < 11; c++) {
                out[k++] = m[r][c];
            }
        }
    }
    return bad;
}

void dmr_bptc196_encode(const uint8_t in[96], uint8_t out[196])
{
    uint8_t m[13][15];
    uint8_t lin[196];
    int k = 0;

    memset(m, 0, sizeof(m));
    for (int c = 3; c < 11; c++) {
        m[0][c] = in[k++];
    }
    for (int r = 1; r < 9; r++) {
        for (int c = 0; c < 11; c++) {
            m[r][c] = in[k++];
        }
    }
    for (int r = 0; r < 9; r++) {
        uint32_t cw = dmr_hamming_15_11_encode(dmr_bits_to_u32(m[r], 11));
        dmr_u32_to_bits(cw, 15, m[r]);
    }
    for (int c = 0; c < 15; c++) {
        uint8_t col[13];
        uint32_t cw;
        for (int r = 0; r < 9; r++) {
            col[r] = m[r][c];
        }
        cw = dmr_hamming_13_9_encode(dmr_bits_to_u32(col, 9));
        dmr_u32_to_bits(cw, 13, col);
        for (int r = 9; r < 13; r++) {
            m[r][c] = col[r];
        }
    }
    lin[0] = 0; /* R(3) */
    for (int r = 0; r < 13; r++) {
        for (int c = 0; c < 15; c++) {
            lin[1 + r * 15 + c] = m[r][c];
        }
    }
    for (int a = 0; a < 196; a++) {
        out[(a * 181) % 196] = lin[a];
    }
}

/* ------------------------------------------------------------------------- */
/* Embedded LC (BPTC 8 x 16)                                                  */
/* ------------------------------------------------------------------------- */
/*
 * ETSI TS 102 361-1 B.2.1: 8 rows x 16 columns, filled column by column in
 * transmitted order (bit t -> row t % 8, column t / 8). Rows 0..6: 11 bits +
 * Hamming(16,11,4); row 7: even parity of each column. The 72 LC bits are
 * rows 0-1 columns 0-10 and rows 2-6 columns 0-9; the 5-bit checksum sits in
 * column 10 of rows 2-6, MSB first.
 */
bool dmr_emb_lc_decode(const uint8_t in[128], uint8_t lc[72])
{
    uint8_t m[8][16];
    bool ok = true;
    int k = 0;
    uint8_t crc = 0;

    for (int t = 0; t < 128; t++) {
        m[t % 8][t / 8] = in[t] & 1u;
    }
    for (int r = 0; r < 7; r++) {
        uint32_t cw = dmr_bits_to_u32(m[r], 16);
        if (dmr_hamming_16_11_decode(&cw)) {
            dmr_u32_to_bits(cw, 16, m[r]);
        } else {
            ok = false;
        }
    }
    /* Row 7 (column parity) is deliberately not a reject condition: a bit
     * error in that row touches no data, and anything it could catch is
     * already caught by the row Hamming codes plus the 5-bit checksum.
     * Rejecting on it threw away ~1 in 8 perfectly good single-error LCs. */
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 11; c++) {
            lc[k++] = m[r][c];
        }
    }
    for (int r = 2; r < 7; r++) {
        for (int c = 0; c < 10; c++) {
            lc[k++] = m[r][c];
        }
        crc = (uint8_t)((crc << 1) | m[r][10]);
    }
    return ok && crc == dmr_emb_crc5(lc);
}

void dmr_emb_lc_encode(const uint8_t lc[72], uint8_t out[128])
{
    uint8_t m[8][16];
    uint8_t crc = dmr_emb_crc5(lc);
    int k = 0;

    memset(m, 0, sizeof(m));
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 11; c++) {
            m[r][c] = lc[k++];
        }
    }
    for (int r = 2; r < 7; r++) {
        for (int c = 0; c < 10; c++) {
            m[r][c] = lc[k++];
        }
        m[r][10] = (uint8_t)((crc >> (6 - r)) & 1u);
    }
    for (int r = 0; r < 7; r++) {
        dmr_u32_to_bits(dmr_hamming_16_11_encode(dmr_bits_to_u32(m[r], 11)), 16,
                        m[r]);
    }
    for (int c = 0; c < 16; c++) {
        uint8_t p = 0;
        for (int r = 0; r < 7; r++) {
            p ^= m[r][c];
        }
        m[7][c] = p;
    }
    for (int t = 0; t < 128; t++) {
        out[t] = m[t % 8][t / 8];
    }
}

uint8_t dmr_emb_crc5(const uint8_t lc[72])
{
    uint32_t sum = 0;
    for (int i = 0; i < 9; i++) {
        sum += dmr_bits_to_u32(&lc[i * 8], 8);
    }
    return (uint8_t)(sum % 31u);
}

/* ------------------------------------------------------------------------- */
/* RS(12,9) over GF(2^8), primitive polynomial 0x11D                          */
/* ------------------------------------------------------------------------- */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static bool gf_ready;

static void gf_init(void)
{
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) {
            x ^= 0x11Du;
        }
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
    gf_ready = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    return gf_exp[gf_log[a] + gf_log[b]];
}

/* Syndromes = the codeword polynomial (byte 0 = highest degree) evaluated at
 * alpha^1..alpha^3; all zero for a valid codeword. */
bool dmr_rs_12_9_check(const uint8_t bytes[12], uint32_t mask)
{
    uint8_t cw[12];

    if (!gf_ready) {
        gf_init();
    }
    memcpy(cw, bytes, 12);
    cw[9] ^= (uint8_t)(mask >> 16);
    cw[10] ^= (uint8_t)(mask >> 8);
    cw[11] ^= (uint8_t)mask;

    for (int j = 1; j <= 3; j++) {
        uint8_t s = 0;
        for (int i = 0; i < 12; i++) {
            s = (uint8_t)(cw[i] ^ gf_mul(gf_exp[j], s));
        }
        if (s != 0) {
            return false;
        }
    }
    return true;
}

static uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (a == 0) {
        return 0;
    }
    return gf_exp[(gf_log[a] + 255 - gf_log[b]) % 255];
}

/* Single-byte correction: with one error of value e at byte i (polynomial
 * degree 11 - i, locator X = a^(11-i)) the syndromes are S_j = e * X^j, so
 * X = S2/S1, and S3 = S2*X must hold. Anything else is left uncorrected. */
bool dmr_rs_12_9_decode(uint8_t bytes[12], uint32_t mask)
{
    uint8_t cw[12], s[4];

    if (!gf_ready) {
        gf_init();
    }
    memcpy(cw, bytes, 12);
    cw[9] ^= (uint8_t)(mask >> 16);
    cw[10] ^= (uint8_t)(mask >> 8);
    cw[11] ^= (uint8_t)mask;
    for (int j = 1; j <= 3; j++) {
        uint8_t v = 0;
        for (int i = 0; i < 12; i++) {
            v = (uint8_t)(cw[i] ^ gf_mul(gf_exp[j], v));
        }
        s[j] = v;
    }
    if (s[1] == 0 && s[2] == 0 && s[3] == 0) {
        return true;
    }
    if (s[1] == 0 || s[2] == 0) {
        return false;
    }
    {
        uint8_t x = gf_div(s[2], s[1]);
        int deg = gf_log[x];
        int i = 11 - deg;
        if (gf_mul(s[2], x) != s[3] || i < 0 || i > 11) {
            return false;
        }
        bytes[i] ^= gf_div(s[1], x); /* e = S1 / X */
    }
    return dmr_rs_12_9_check(bytes, mask);
}

/* Systematic encoding with g(x) = (x-a)(x-a^2)(x-a^3). */
void dmr_rs_12_9_parity(const uint8_t data[9], uint32_t mask, uint8_t parity[3])
{
    uint8_t g[4]; /* g(x) = x^3 + g[1] x^2 + g[2] x + g[3] */
    uint8_t rem[3] = { 0, 0, 0 };

    if (!gf_ready) {
        gf_init();
    }
    {
        uint8_t a1 = gf_exp[1], a2 = gf_exp[2], a3 = gf_exp[3];
        g[0] = 1;
        g[1] = (uint8_t)(a1 ^ a2 ^ a3);
        g[2] = (uint8_t)(gf_mul(a1, a2) ^ gf_mul(a1, a3) ^ gf_mul(a2, a3));
        g[3] = gf_mul(gf_mul(a1, a2), a3);
    }
    for (int i = 0; i < 9; i++) {
        uint8_t fb = (uint8_t)(data[i] ^ rem[0]);
        rem[0] = (uint8_t)(rem[1] ^ gf_mul(fb, g[1]));
        rem[1] = (uint8_t)(rem[2] ^ gf_mul(fb, g[2]));
        rem[2] = gf_mul(fb, g[3]);
    }
    parity[0] = (uint8_t)(rem[0] ^ (mask >> 16));
    parity[1] = (uint8_t)(rem[1] ^ (mask >> 8));
    parity[2] = (uint8_t)(rem[2] ^ mask);
}

/* ------------------------------------------------------------------------- */
/* CRC-CCITT (CSBK)                                                           */
/* ------------------------------------------------------------------------- */

uint16_t dmr_crc_ccitt(const uint8_t *bits, int nbits)
{
    uint16_t crc = 0;
    for (int i = 0; i < nbits; i++) {
        if (((crc >> 15) & 1u) ^ (bits[i] & 1u)) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return (uint16_t)(crc ^ 0xFFFFu);
}
