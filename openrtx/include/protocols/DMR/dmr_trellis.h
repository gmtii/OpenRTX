/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TRELLIS_H
#define DMR_TRELLIS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR rate 3/4 trellis code (ETSI TS 102 361-1, Annex B.2): the FEC of rate
 * 3/4 packet data blocks (data type 8).
 *
 * 144 data bits -> 48 tribits (+ one flushing zero tribit) -> 8-state
 * trellis encoder -> 49 constellation points -> 98 dibits -> interleaver ->
 * the 196 info bits of a data burst.
 *
 * The decoder is a full Viterbi over the 8 states with a Manhattan distance
 * branch metric on the (+-1, +-3) dibit levels, forced to end in state 0 (the
 * flushing tribit). Tables are checked in test/host_dmr_lrrp against the
 * on-air vectors of ok-dmrlib (test_trellis.py) and the constellation of
 * dsd-fme's dmr_34.c.
 */

/* in: 196 info bits in transmitted order (burst bits 0..97 then 166..263).
 * out: 18 bytes (144 bits, MSB first).
 * Returns the path metric of the decoded sequence (0 = error free); every
 * symbol error adds at least 2. */
int dmr_trellis34_decode(const uint8_t in[196], uint8_t out[18]);

/* Encoder, for tests: 18 bytes -> 196 info bits. */
void dmr_trellis34_encode(const uint8_t in[18], uint8_t out[196]);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TRELLIS_H */
