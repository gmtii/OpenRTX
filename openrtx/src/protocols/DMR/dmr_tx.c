/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR voice transmission: bursts and 4FSK baseband (see dmr_tx.h). The burst
 * layout is the one dmr_proto.c reads: 264 bits, dibit d = bits 2d, 2d+1.
 *
 *   data burst (header, terminator, CSBK): info 0..97, slot type 98..107,
 *   sync 108..155, slot type 156..165, info 166..263
 *   voice burst: AMBE 0..107, sync or EMB 108..115 + embedded LC fragment
 *   116..147 + EMB 148..155, AMBE 156..263
 */

#include <math.h>
#include <string.h>
#include "dmr_fec.h"
#include "dmr_tx.h"

/* Mobile station syncs, 24 dibits */
#define SYNC_MS_VOICE 0x7F7D5DD57DFDull
#define SYNC_MS_DATA 0xD5D7F77FD757ull

/* Data types of the slot type */
#define DT_VOICE_LC_HEADER 1u
#define DT_TERMINATOR_LC 2u
#define DT_CSBK 3u

/* Full LC opcodes */
#define FLCO_GROUP 0x00u
#define FLCO_UNIT 0x03u

/* CSBK opcode, with the last block bit */
#define CSBKO_BS_DWN_ACT 0x38u
#define CSBK_LAST_BLOCK 0x80u

static void put_sync(uint8_t bits[264], uint64_t sync)
{
    for (int i = 0; i < 48; i++)
        bits[108 + i] = (uint8_t)((sync >> (47 - i)) & 1u);
}

static void bits_to_dibits(const uint8_t bits[264],
                           uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    for (int i = 0; i < DMR_TX_BURST_DIBITS; i++)
        dibits[i] = (uint8_t)((bits[2 * i] << 1) | bits[2 * i + 1]);
}

/* Bytes to one bit per byte, MSB first */
static void bytes_to_bits(const uint8_t *bytes, int nbits, uint8_t *bits)
{
    for (int i = 0; i < nbits; i++)
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - i % 8)) & 1u);
}

void dmr_tx_setup(dmr_tx_t *tx, uint32_t src, uint32_t dst, bool group,
                  uint8_t cc)
{
    const uint8_t bytes[9] = {
        group ? FLCO_GROUP : FLCO_UNIT, 0x00, 0x00, /* FID, options */
        (uint8_t)(dst >> 16), (uint8_t)(dst >> 8), (uint8_t)dst,
        (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src,
    };
    bytes_to_bits(bytes, 72, tx->lc);
    dmr_emb_lc_encode(tx->lc, tx->emb_lc);
    tx->cc = cc & 0xFu;
    tx->voice_idx = 0;
}

/* A data burst of 96 information bits, BPTC(196,96) */
static void info_burst(uint8_t cc, uint32_t dt, const uint8_t info[96],
                       uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    uint8_t coded[196], bits[264], st[20];

    dmr_bptc196_encode(info, coded);
    memcpy(bits, coded, 98);
    memcpy(&bits[166], &coded[98], 98);
    dmr_u32_to_bits(dmr_golay_20_8_encode(((uint32_t)cc << 4) | dt), 20, st);
    memcpy(&bits[98], st, 10);
    memcpy(&bits[156], &st[10], 10);
    put_sync(bits, SYNC_MS_DATA);
    bits_to_dibits(bits, dibits);
}

/* Header or terminator: the LC with its RS(12,9) parity */
static void lc_burst(const dmr_tx_t *tx, uint32_t dt, uint32_t rs_mask,
                     uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    uint8_t bytes[12], info[96];

    memset(bytes, 0, sizeof(bytes));
    for (int i = 0; i < 72; i++)
        bytes[i / 8] |= (uint8_t)(tx->lc[i] << (7 - i % 8));
    dmr_rs_12_9_parity(bytes, rs_mask, &bytes[9]);
    bytes_to_bits(bytes, 96, info);
    info_burst(tx->cc, dt, info, dibits);
}

void dmr_tx_header(const dmr_tx_t *tx, uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    lc_burst(tx, DT_VOICE_LC_HEADER, DMR_RS_MASK_VOICE_HEADER, dibits);
}

void dmr_tx_terminator(const dmr_tx_t *tx,
                       uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    lc_burst(tx, DT_TERMINATOR_LC, DMR_RS_MASK_TERMINATOR, dibits);
}

void dmr_tx_wakeup(const dmr_tx_t *tx, uint32_t bs, uint32_t src,
                   uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    /* CSBK BS_Dwn_Act (ETSI TS 102 361-2): last block, FID 0, the base
     * station and the source addresses; CRC-CCITT masked for a CSBK */
    const uint8_t bytes[10] = {
        CSBK_LAST_BLOCK | CSBKO_BS_DWN_ACT, 0x00, 0x00, 0x00,
        (uint8_t)(bs >> 16), (uint8_t)(bs >> 8), (uint8_t)bs,
        (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src,
    };
    uint8_t info[96];

    bytes_to_bits(bytes, 80, info);
    const uint16_t crc = (uint16_t)(dmr_crc_ccitt(info, 80)
                                    ^ DMR_CRC_MASK_CSBK);
    dmr_u32_to_bits(crc, 16, &info[80]);
    info_burst(tx->cc, DT_CSBK, info, dibits);
}

void dmr_tx_voice(dmr_tx_t *tx, const uint8_t ambe[3][DMR_TX_AMBE_BYTES],
                  uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    uint8_t bits[264], f[3][72];

    for (int k = 0; k < 3; k++)
        bytes_to_bits(ambe[k], 72, f[k]);
    memcpy(bits, f[0], 72);
    memcpy(&bits[72], f[1], 36);
    memcpy(&bits[156], &f[1][36], 36);
    memcpy(&bits[192], f[2], 72);

    const int idx = tx->voice_idx;
    if (idx == 0) {
        put_sync(bits, SYNC_MS_VOICE);
    } else {
        /* EMB: colour code, PI 0, LCSS; B first, C and D continuation, E
         * last fragment of the embedded LC, F a single (null) one */
        static const uint8_t lcss[6] = { 0, 1, 3, 3, 2, 0 };
        uint8_t emb[16];
        const uint32_t cw = dmr_qr_16_7_encode(((uint32_t)tx->cc << 3)
                                               | lcss[idx]);
        dmr_u32_to_bits(cw, 16, emb);
        memcpy(&bits[108], emb, 8);
        memcpy(&bits[148], &emb[8], 8);
        if (idx <= 4)
            memcpy(&bits[116], &tx->emb_lc[32 * (idx - 1)], 32);
        else
            memset(&bits[116], 0, 32);
    }
    tx->voice_idx = (idx + 1) % 6;
    bits_to_dibits(bits, dibits);
}

void dmr_mod_init(dmr_mod_t *mod, float gain, bool invert)
{
    const float pi = 3.14159265358979f;
    const float a = 0.2f;
    float sum = 0.0f;

    /* The receiver's matched filter (dmr_demod.c): root raised cosine,
     * alpha 0.2, +-4 symbols */
    for (int i = 0; i < DMR_TX_RRC_TAPS; i++) {
        const float t = ((float)i - (DMR_TX_RRC_TAPS - 1) / 2.0f) / DMR_TX_SPS;
        float h;
        if (fabsf(t) < 1e-6f) {
            h = 1.0f - a + 4.0f * a / pi;
        } else if (fabsf(fabsf(t) - 1.0f / (4.0f * a)) < 1e-6f) {
            h = a / sqrtf(2.0f)
                * ((1.0f + 2.0f / pi) * sinf(pi / (4.0f * a))
                   + (1.0f - 2.0f / pi) * cosf(pi / (4.0f * a)));
        } else {
            h = (sinf(pi * t * (1.0f - a))
                 + 4.0f * a * t * cosf(pi * t * (1.0f + a)))
                / (pi * t * (1.0f - (4.0f * a * t) * (4.0f * a * t)));
        }
        mod->taps[i] = h;
        sum += h;
    }
    /* The taps sum to gain * DMR_TX_SPS: a run of one symbol meets one tap
     * in DMR_TX_SPS, so it comes out at gain times its value */
    for (int i = 0; i < DMR_TX_RRC_TAPS; i++)
        mod->taps[i] *= gain * DMR_TX_SPS / sum;
    memset(mod->hist, 0, sizeof(mod->hist));
    mod->hist_pos = 0;
    mod->invert = invert;
}

void dmr_mod_dibits(dmr_mod_t *mod, const uint8_t *dibits, int n,
                    int16_t *out)
{
    /* Dibit to symbol: 01 = +3, 00 = +1, 10 = -1, 11 = -3 */
    static const float sym[4] = { +1.0f, +3.0f, -1.0f, -3.0f };
    const int span = (DMR_TX_RRC_TAPS + DMR_TX_SPS - 1) / DMR_TX_SPS;

    for (int k = 0; k < n; k++) {
        mod->hist_pos = (mod->hist_pos + 1) & 15u;
        mod->hist[mod->hist_pos] = dibits ? sym[dibits[k] & 3u] : 0.0f;
        for (int p = 0; p < DMR_TX_SPS; p++) {
            /* Zero stuffed symbols through the filter: only one tap in ten
             * meets a symbol */
            float y = 0.0f;
            for (int j = 0; j < span; j++) {
                const int t = p + DMR_TX_SPS * j;
                if (t >= DMR_TX_RRC_TAPS)
                    break;
                y += mod->taps[t] * mod->hist[(mod->hist_pos - j) & 15u];
            }
            if (mod->invert)
                y = -y;
            if (y > 32767.0f)
                y = 32767.0f;
            if (y < -32768.0f)
                y = -32768.0f;
            out[k * DMR_TX_SPS + p] = (int16_t)y;
        }
    }
}
