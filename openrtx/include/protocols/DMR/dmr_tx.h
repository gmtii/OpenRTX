/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TX_H
#define DMR_TX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR voice transmission as a mobile station: the bursts of a call and their
 * 4FSK baseband, for an FM transmitter fed at 48 kHz.
 *
 * A call is a voice LC header (sent more than once), voice superframes of
 * six bursts (A with the voice sync, B..E with the four fragments of the
 * embedded LC, F with a null one) and a terminator with LC. A repeater that
 * sleeps is woken up first with a CSBK BS_Dwn_Act. Bursts are 132 dibits in
 * the receiver's format (dmr_frame_t), so dmr_proto can check them.
 *
 * The modulator turns dibits into 4800 symbols/s (+1, +3, -1, -3) shaped by
 * a root raised cosine (alpha 0.2, the receiver's matched filter), at
 * 48 kHz, ten samples per symbol.
 */

#define DMR_TX_BURST_DIBITS 132
#define DMR_TX_SPS 10
#define DMR_TX_RRC_TAPS 81
#define DMR_TX_AMBE_BYTES 9

/* A call being built */
typedef struct {
    uint8_t lc[72];      /* Full link control, one bit per byte */
    uint8_t emb_lc[128]; /* Embedded LC, the B..E fragments */
    uint8_t cc;          /* Colour code */
    int voice_idx;       /* Next voice burst, 0..5 = A..F */
} dmr_tx_t;

/**
 * \brief Start a call.
 *
 * @param tx: call state.
 * @param src: own DMR ID (24 bits).
 * @param dst: talkgroup or called ID (24 bits).
 * @param group: true for a group call, false for a private (unit) call.
 * @param cc: colour code, 0..15.
 */
void dmr_tx_setup(dmr_tx_t *tx, uint32_t src, uint32_t dst, bool group,
                  uint8_t cc);

/**
 * \brief Voice LC header burst.
 *
 * @param tx: call state.
 * @param dibits: the burst.
 */
void dmr_tx_header(const dmr_tx_t *tx, uint8_t dibits[DMR_TX_BURST_DIBITS]);

/**
 * \brief Terminator with LC burst.
 *
 * @param tx: call state.
 * @param dibits: the burst.
 */
void dmr_tx_terminator(const dmr_tx_t *tx,
                       uint8_t dibits[DMR_TX_BURST_DIBITS]);

/**
 * \brief Repeater wake-up: a CSBK BS_Dwn_Act burst, which a sleeping
 * repeater takes at any timing and answers by transmitting.
 *
 * @param tx: call state (colour code).
 * @param bs: repeater address (0xFFFFFF: any).
 * @param src: own DMR ID.
 * @param dibits: the burst.
 */
void dmr_tx_wakeup(const dmr_tx_t *tx, uint32_t bs, uint32_t src,
                   uint8_t dibits[DMR_TX_BURST_DIBITS]);

/**
 * \brief Next voice burst of the superframe.
 *
 * @param tx: call state, advances to the next burst.
 * @param ambe: three AMBE+2 frames of 72 bits, first bit transmitted in the
 * MSB of the first byte (DMR interleaved order).
 * @param dibits: the burst.
 */
void dmr_tx_voice(dmr_tx_t *tx, const uint8_t ambe[3][DMR_TX_AMBE_BYTES],
                  uint8_t dibits[DMR_TX_BURST_DIBITS]);

/* 4FSK modulator */
typedef struct {
    float taps[DMR_TX_RRC_TAPS];
    float hist[16]; /* Last symbols, newest at hist_pos */
    unsigned hist_pos;
    bool invert;
} dmr_mod_t;

/**
 * \brief Initialise the modulator.
 *
 * @param mod: modulator.
 * @param gain: output for a long run of symbols +1 (a run of +3 gives three
 * times this). Peaks of the filtered signal are saturated to int16.
 * @param invert: negate the output, for a transmitter that inverts.
 */
void dmr_mod_init(dmr_mod_t *mod, float gain, bool invert);

/**
 * \brief Modulate dibits, DMR_TX_SPS samples each.
 *
 * @param mod: modulator.
 * @param dibits: dibits to send, or NULL for silence (symbol 0, no
 * deviation: the gaps between the bursts, and the end of the filter tail).
 * @param n: number of dibits, or of silent symbols.
 * @param out: n * DMR_TX_SPS samples.
 */
void dmr_mod_dibits(dmr_mod_t *mod, const uint8_t *dibits, int n,
                    int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TX_H */
