/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_RX_H
#define OPENRTX_NXDN_RX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NXDN48 receiver (2400 baud 4FSK, 6.25 kHz channels): frame sync, LICH,
 * SACCH and FACCH1 decoding, the voice call header (Layer 3 VCALL) and the
 * AMBE+2 speech, handed to the DMR vocoder queue (dmr_voice_ambe72()).
 */

/** RF channel type (LICH RFCT) */
enum nxdnChannel {
    NXDN_RCCH = 0,   /**< Trunked control channel */
    NXDN_RTCH = 1,   /**< Trunked traffic channel */
    NXDN_RDCH = 2,   /**< Conventional channel */
    NXDN_RTCH_C = 3  /**< Composite control/traffic channel */
};

/** What the receiver has decoded, for the UI */
struct nxdnStatus {
    bool sync;          /**< A frame with a good LICH in the last second */
    bool call;          /**< A voice call is on */
    bool voice;         /**< Speech queued in the last half second */
    bool idsValid;      /**< src, dst, group and cipher are known */
    bool group;         /**< Group call (else individual) */
    bool encrypted;     /**< The call declares a cipher: not played */
    uint8_t cipher;     /**< Cipher type: 0 none, 1 scrambler, 2 DES, 3 AES */
    uint8_t keyId;      /**< Key ID of an encrypted call */
    uint8_t ran;        /**< Radio access number, -1 (0xff) if unknown */
    uint8_t channel;    /**< enum nxdnChannel of the last frame */
    uint16_t src;       /**< Source unit ID */
    uint16_t dst;       /**< Talkgroup or unit called */
    int16_t level;      /**< Outer symbol level of the last sync */
    uint32_t frames;    /**< Frames with a good LICH since the mode started */
    uint32_t lichBad;   /**< Syncs whose LICH parity failed */
    uint32_t overruns;  /**< Sample blocks lost */
    uint32_t serial;    /**< Changes whenever a shown field changes */
};

/**
 * Start afresh: new frequency or mode start.
 */
void nxdn_rx_reset(void);

/**
 * Feed discriminator samples.
 *
 * @param samples: 48 kHz audio from the radio's FM discriminator.
 * @param n: number of samples.
 * @param now_ms: tick at the end of the block.
 */
void nxdn_rx_feed(const int16_t *samples, unsigned n, uint32_t now_ms);

/**
 * Count a lost block of samples.
 */
void nxdn_rx_overrun(void);

/**
 * Note the received signal strength, for the console figures.
 *
 * @param dbm: RSSI, dBm.
 */
void nxdn_rx_noteRssi(int dbm);

/**
 * Age the call state. Call regularly from the receiving thread.
 *
 * @param now_ms: current tick.
 */
void nxdn_rx_tick(uint32_t now_ms);

/**
 * Copy the receiver status.
 *
 * @param out: destination.
 */
void nxdn_rx_status(struct nxdnStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_RX_H */
