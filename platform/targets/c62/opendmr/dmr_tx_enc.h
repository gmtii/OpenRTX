/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C62_DMR_TX_ENC_H
#define C62_DMR_TX_ENC_H

/*
 * Microphone to AMBE+2 for DMR transmission: a thread of its own captures the
 * microphone at 8 kHz, 60 ms (one burst, three 20 ms frames) at a time, and
 * encodes it with OpenDMR. The transmitting mode takes the frames three at a
 * time, one voice burst every 60 ms.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start capturing and encoding.
 *
 * @return false if the microphone could not be opened.
 */
bool dmrTxEnc_start(void);

/**
 * The next three encoded frames, if there are.
 *
 * @param ambe: three 72-bit frames, DMR interleaved order (dmr_tx_voice()).
 * @return false if none is ready yet.
 */
bool dmrTxEnc_pop(uint8_t ambe[3][9]);

/**
 * Stop capturing and encoding, and release the microphone.
 */
void dmrTxEnc_stop(void);

/**
 * Figures of the last transmission, for the log.
 *
 * @param bursts: bursts encoded.
 * @param maxUs: longest encoding of a burst (three frames), elapsed time:
 * preemption by higher priority threads included.
 * @param dropped: bursts dropped because the transmitter did not take them.
 */
void dmrTxEnc_stats(uint32_t *bursts, uint32_t *maxUs, uint32_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* C62_DMR_TX_ENC_H */
