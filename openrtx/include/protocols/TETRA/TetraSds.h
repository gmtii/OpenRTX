/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_SDS_H
#define OPENRTX_TETRA_SDS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TETRA short data history, ported from the ESP32-P4 SDR: D-STATUS and
 * D-SDS-DATA as libtetra hands them over, decoded into text (EN 300 392-2
 * clause 29, ETSI TS 100 392-18-1 for LIP):
 *
 * - pre-coded status and SDS-SHORT REPORT;
 * - user defined data 1 to 3, as hex;
 * - type 4: SDS-TL header, SDS-REPORT and SDS-ACK, text (simple, immediate,
 *   SDS-TL, with user data header), LIP positions;
 * - concatenated messages, by user data header (PID 0x8a) or by SDS-TL
 *   (PID 0x0c / 0x8c), stored once complete, or after TETRA_SDS_CONCAT_MS
 *   with the parts received;
 * - end-to-end encrypted messages (PID 0x08 / 0x88) are listed without
 *   their contents.
 *
 * Text is reduced to printable ASCII for the UI fonts.
 */

#define TETRA_SDS_MAX 10   /**< Messages kept, most recent first */
#define TETRA_SDS_TEXT 160 /**< Decoded text or summary, with the NUL */
#define TETRA_SDS_CONCAT_MS 120000

enum tetraSdsKind {
    TETRA_SDS_KIND_STATUS = 0, /**< Pre-coded status */
    TETRA_SDS_KIND_SHORT,      /**< User defined data 1 to 3 */
    TETRA_SDS_KIND_TEXT,       /**< Text message */
    TETRA_SDS_KIND_LIP,        /**< Location report */
    TETRA_SDS_KIND_REPORT,     /**< Delivery report or acknowledgement */
    TETRA_SDS_KIND_E2EE,       /**< End-to-end encrypted, not decoded */
    TETRA_SDS_KIND_OTHER,      /**< Any other protocol */
};

struct tetraSds {
    uint32_t seq;     /**< Increments with every new message */
    long long tick;   /**< Tick of reception */
    uint32_t from;    /**< Sender SSI, 0 if unknown */
    uint32_t to;      /**< Destination SSI */
    uint8_t kind;     /**< enum tetraSdsKind */
    uint8_t pid;      /**< Protocol identifier (user defined data 4) */
    uint8_t parts;    /**< Concatenated: parts, 0 for a single message */
    uint8_t partsGot; /**< Concatenated: parts received */
    char text[TETRA_SDS_TEXT];
};

struct tetra_sds;

/**
 * Forget every message and every partial concatenation.
 */
void tetraSds_reset(void);

/**
 * Decode a short data event from libtetra.
 *
 * @param s: the event.
 * @param out: filled with the message stored, if any.
 * @return true if a message was stored (a concatenation part alone is not).
 */
bool tetraSds_receive(const struct tetra_sds *s, struct tetraSds *out);

/**
 * Copy the stored messages, most recent first.
 *
 * @param out: destination, max entries.
 * @param max: room in out.
 * @return number of messages copied.
 */
int tetraSds_get(struct tetraSds *out, int max);

/**
 * Name of an SDS protocol identifier (EN 300 392-2 table 29.21).
 */
const char *tetraSds_pidName(uint8_t pid);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_SDS_H */
