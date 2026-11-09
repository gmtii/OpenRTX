/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_ACARS_H
#define OPENRTX_ACARS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ACARS receiver (ARINC 618): the AM-detected audio of a VHF air band
 * channel, 2400 bit/s MSK on 1200 and 2400 Hz tones, decoded in software.
 *
 *   audio, 48 kHz -> DC removal -> delay-line discriminator (the product
 *   of the signal and itself 7 samples earlier: positive on 1200 Hz,
 *   negative on 2400 Hz) -> one-bit integrator (20 samples) -> bit clock
 *   (the phase with the largest output, tracked) -> tone decisions.
 *
 * The tones become bits one of six ways (the tone itself, its change, or
 * the running XOR of the tones as coherent MSK sends them, each either
 * polarity); all six are searched for the SYN SYN sync word and the frame
 * is read with the one that has it. Characters are 7-bit ASCII
 * with odd parity, least significant bit first; a frame runs from SOH to
 * ETX or ETB, then a 16-bit CRC (CCITT, reflected) that must leave a zero
 * remainder. A frame with one or two characters failing parity is
 * repaired when flipping bits in them makes the CRC good.
 *
 * Valid frames go into a history of ACARS_MAX_MSGS messages, newest first.
 */

#define ACARS_IN_RATE 48000
#define ACARS_MAX_MSGS 16
#define ACARS_TEXT 220

struct acarsMsg {
    uint32_t seq;    /**< Increments with every message */
    long long tick;  /**< Tick of reception */
    char mode;       /**< Mode character */
    char address[8]; /**< Aircraft registration, leading dots removed */
    char label[3];   /**< Message label, "_d" for the general response */
    char blockId;    /**< Block identifier */
    bool downlink;   /**< From an aircraft (digit block identifier) */
    bool repaired;   /**< Parity errors corrected with the CRC */
    char flight[7];  /**< Flight number (downlinks), may be empty */
    char text[ACARS_TEXT];
};

struct acarsStats {
    uint32_t frames;    /**< Valid frames */
    uint32_t repaired;  /**< Of which repaired */
    uint32_t crcErrors; /**< Frames dropped on a bad CRC */
    uint32_t syncs;     /**< Sync words found */
    bool inFrame;       /**< A frame is being received */
};

/**
 * Forget the messages and restart the receiver.
 */
void acars_reset(void);

/**
 * Process AM-detected audio at ACARS_IN_RATE, any scale. Call from one
 * thread only.
 */
void acars_process(const int16_t *samples, size_t n);

/**
 * Copy the messages received, newest first.
 *
 * @return number of messages copied.
 */
int acars_get(struct acarsMsg *out, int max);

/**
 * Sequence number of the newest message, 0 if none yet.
 */
uint32_t acars_lastSeq(void);

void acars_getStats(struct acarsStats *out);

/**
 * Short description of a message label, or NULL if not known.
 */
const char *acars_labelName(const char *label);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_ACARS_H */
