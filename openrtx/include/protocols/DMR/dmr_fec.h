/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_H
#define DMR_FEC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR (ETSI TS 102 361-1) forward error correction, CRCs and block codes
 * needed for phase 1 (metadata only, no voice).
 *
 * Every code is built from its generator polynomial rather than copied from
 * a table:
 *   Hamming(7,4,3)    cyclic, g = x^3+x+1                     (CACH TACT)
 *   Hamming(15,11,3)  cyclic, g = x^4+x+1                     (BPTC rows)
 *   Hamming(13,9,3)   (15,11) shortened                       (BPTC columns)
 *   Hamming(16,11,4)  (15,11) + overall parity                (embedded LC rows)
 *   Golay(20,8,8)     extended Golay(24,12) shortened,
 *                     g = x^11+x^10+x^6+x^5+x^4+x^2+1         (slot type)
 *   QR(16,7,6)        QR(17,9) shortened + overall parity,
 *                     g = x^8+x^5+x^4+x^3+1                   (EMB)
 * test/host_dmr/dmr_fec_test.c checks every codeword of all six against the
 * generator matrices of an independent implementation (dsd-fme's fec.c).
 *
 * Bit convention: a codeword is a uint32_t with the first transmitted bit in
 * the most significant used position - data bits first, then parity.
 */

/* ---- Block codes ---------------------------------------------------------- */
uint32_t dmr_hamming_7_4_encode(uint32_t data4);
uint32_t dmr_hamming_15_11_encode(uint32_t data11);
uint32_t dmr_hamming_13_9_encode(uint32_t data9);
uint32_t dmr_hamming_16_11_encode(uint32_t data11);
uint32_t dmr_golay_20_8_encode(uint32_t data8);
uint32_t dmr_qr_16_7_encode(uint32_t data7);

/* Decoders correct the codeword in place (up to the code's capacity) and
 * return true if the result is a valid codeword. */
bool dmr_hamming_7_4_decode(uint32_t *cw);
bool dmr_hamming_15_11_decode(uint32_t *cw);
bool dmr_hamming_13_9_decode(uint32_t *cw);
bool dmr_hamming_16_11_decode(uint32_t *cw);
bool dmr_golay_20_8_decode(uint32_t *cw);
bool dmr_qr_16_7_decode(uint32_t *cw);

/* ---- BPTC(196,96): data bursts (headers, terminators, CSBK...) -------------
 * in: 196 info bits in transmitted order (burst bits 0..97 then 166..263).
 * out: 96 data bits. Returns the number of rows/columns that could not be
 * corrected (0 = clean). */
int dmr_bptc196_decode(const uint8_t in[196], uint8_t out[96]);
void dmr_bptc196_encode(const uint8_t in[96], uint8_t out[196]);

/* ---- Embedded LC: BPTC 8x16 over the 4 x 32 bits of voice bursts B..E ------
 * in: 128 bits in transmitted order (B's 32, C's, D's, E's).
 * out: 72 LC bits. Returns true if all rows decode and the 5-bit checksum
 * matches. */
bool dmr_emb_lc_decode(const uint8_t in[128], uint8_t lc[72]);
void dmr_emb_lc_encode(const uint8_t lc[72], uint8_t out[128]);

/* ---- CRCs / checksums --------------------------------------------------------- */
/* Full LC (voice LC header / terminator): RS(12,9) over GF(256), with the
 * parity bytes XOR-masked (0x969696 header, 0x999999 terminator). */
#define DMR_RS_MASK_VOICE_HEADER 0x969696u
#define DMR_RS_MASK_TERMINATOR 0x999999u
bool dmr_rs_12_9_check(const uint8_t bytes[12], uint32_t mask);
/* As check, but first corrects a single erroneous byte in place. */
bool dmr_rs_12_9_decode(uint8_t bytes[12], uint32_t mask);
void dmr_rs_12_9_parity(const uint8_t data[9], uint32_t mask,
                        uint8_t parity[3]);

/* CSBK: CRC-CCITT (x^16+x^12+x^5+1, init 0, inverted), masked 0xA5A5. */
#define DMR_CRC_MASK_CSBK 0xA5A5u
uint16_t dmr_crc_ccitt(const uint8_t *bits, int nbits);

/* Embedded LC 5-bit checksum: sum of the 9 LC bytes mod 31. */
uint8_t dmr_emb_crc5(const uint8_t lc[72]);

/* Helpers */
uint32_t dmr_bits_to_u32(const uint8_t *bits, int n);
void dmr_u32_to_bits(uint32_t v, int n, uint8_t *bits);

#ifdef __cplusplus
}
#endif

#endif /* DMR_FEC_H */
