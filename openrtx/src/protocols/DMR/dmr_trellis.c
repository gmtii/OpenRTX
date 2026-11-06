/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_trellis.h"

#include <string.h>

/* Dibit interleaver (ETSI TS 102 361-1 Table B.8): the i-th transmitted
 * dibit is dibit k_interleave[i] of the encoder output. */
static const uint8_t k_interleave[98] = {
    0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64,
    65, 72, 73, 80, 81, 88, 89, 96, 97, 2,  3,  10, 11, 18, 19, 26, 27,
    34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91, 4,
    5,  12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69,
    76, 77, 84, 85, 92, 93, 6,  7,  14, 15, 22, 23, 30, 31, 38, 39, 46,
    47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95
};

/* Encoder state transition table (Table B.7): constellation point sent for
 * [state = previous tribit][input tribit]. */
static const uint8_t k_encode[8][8] = {
    { 0, 8, 4, 12, 2, 10, 6, 14 }, { 4, 12, 2, 10, 6, 14, 0, 8 },
    { 1, 9, 5, 13, 3, 11, 7, 15 }, { 5, 13, 3, 11, 7, 15, 1, 9 },
    { 3, 11, 7, 15, 1, 9, 5, 13 }, { 7, 15, 1, 9, 5, 13, 3, 11 },
    { 2, 10, 6, 14, 0, 8, 4, 12 }, { 6, 14, 0, 8, 4, 12, 2, 10 },
};

/* Constellation point -> dibit pair, as symbol levels (Table B.9). */
static const int8_t k_point[16][2] = {
    { +1, -1 }, { -1, -1 }, { +3, -3 }, { -3, -3 }, { -3, -1 }, { +3, -1 },
    { -1, -3 }, { +1, -3 }, { -3, +3 }, { +3, +3 }, { -1, +1 }, { +1, +1 },
    { +1, +3 }, { -1, +3 }, { +3, +1 }, { -3, +1 },
};

/* Bit pair -> level, same convention as the demodulator: 01 = +3, 00 = +1,
 * 10 = -1, 11 = -3. */
static int8_t bits_to_level(uint8_t b1, uint8_t b2)
{
    static const int8_t lv[4] = { +1, +3, -1, -3 };
    return lv[((b1 & 1u) << 1) | (b2 & 1u)];
}

static void level_to_bits(int8_t lv, uint8_t *b1, uint8_t *b2)
{
    switch (lv) {
        case +3:
            *b1 = 0;
            *b2 = 1;
            break;
        case +1:
            *b1 = 0;
            *b2 = 0;
            break;
        case -1:
            *b1 = 1;
            *b2 = 0;
            break;
        default:
            *b1 = 1;
            *b2 = 1;
            break;
    }
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

int dmr_trellis34_decode(const uint8_t in[196], uint8_t out[18])
{
    int8_t dib[98];
    uint16_t metric[8], next[8];
    uint8_t pred[49][8];
    uint8_t tribit[49];
    int state;

    for (int i = 0; i < 98; i++) {
        dib[k_interleave[i]] = bits_to_level(in[2 * i], in[2 * i + 1]);
    }

    for (int s = 0; s < 8; s++) {
        metric[s] = (s == 0) ? 0 : 0x3FFF; /* the encoder starts in state 0 */
    }
    for (int k = 0; k < 49; k++) {
        const int r1 = dib[2 * k], r2 = dib[2 * k + 1];
        for (int j = 0; j < 8; j++) {
            next[j] = 0xFFFF;
        }
        for (int s = 0; s < 8; s++) {
            if (metric[s] >= 0x3FFF) {
                continue;
            }
            for (int j = 0; j < 8; j++) {
                const int8_t *e = k_point[k_encode[s][j]];
                uint16_t m =
                    (uint16_t)(metric[s] + iabs(r1 - e[0]) + iabs(r2 - e[1]));
                if (m < next[j]) {
                    next[j] = m;
                    pred[k][j] = (uint8_t)s;
                }
            }
        }
        for (int j = 0; j < 8; j++) {
            metric[j] = (next[j] == 0xFFFF) ? 0x3FFF : next[j];
        }
    }

    /* the 49th tribit is the flushing zero: the path must end in state 0 */
    state = 0;
    for (int k = 48; k >= 0; k--) {
        tribit[k] = (uint8_t)state;
        state = pred[k][state];
    }

    memset(out, 0, 18);
    for (int k = 0; k < 48; k++) {
        for (int b = 0; b < 3; b++) {
            int bit = k * 3 + b;
            if ((tribit[k] >> (2 - b)) & 1u) {
                out[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
            }
        }
    }
    return metric[0];
}

void dmr_trellis34_encode(const uint8_t in[18], uint8_t out[196])
{
    int8_t dib[98];
    int state = 0;

    for (int k = 0; k < 49; k++) {
        int t = 0;
        if (k < 48) {
            for (int b = 0; b < 3; b++) {
                int bit = k * 3 + b;
                t = (t << 1) | ((in[bit >> 3] >> (7 - (bit & 7))) & 1);
            }
        }
        const int8_t *e = k_point[k_encode[state][t]];
        dib[2 * k] = e[0];
        dib[2 * k + 1] = e[1];
        state = t;
    }
    for (int i = 0; i < 98; i++) {
        level_to_bits(dib[k_interleave[i]], &out[2 * i], &out[2 * i + 1]);
    }
}
