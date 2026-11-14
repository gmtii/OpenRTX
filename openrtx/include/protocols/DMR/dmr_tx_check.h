/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TX_CHECK_H
#define DMR_TX_CHECK_H

#include <stdint.h>
#include "protocols/DMR/dmr_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether a call may start through a repeater, from what the receiver heard
 * of its downlink just before the PTT: a repeater on another colour code
 * would ignore the call, and a station talking on our timeslot would be
 * blocked by it. Only what was heard lately counts (DMR_TX_CHECK_FRESH_MS):
 * a repeater not heard at all is for the wake-up to deal with.
 */

/* How long a slot's last burst stays relevant, ms */
#define DMR_TX_CHECK_FRESH_MS 500u

typedef enum {
    DMR_TX_CHECK_OK = 0, /* Nothing against the call */
    DMR_TX_CHECK_CC,     /* The repeater uses another colour code */
    DMR_TX_CHECK_BUSY,   /* Another station is on our timeslot */
} dmr_tx_check_t;

/**
 * \brief Check whether a call may be transmitted on a timeslot.
 *
 * @param slots: state of both timeslots, as from dmr_proto_get_slot()
 * (index 0 = TS1, or the single channel of an MS stream; 1 = TS2).
 * @param slot_idx: our timeslot, 0 or 1; any other value checks only the
 * colour code.
 * @param cc: our colour code, 0..15.
 * @param src: our DMR ID: a call of our own relayed by the repeater does not
 * make the slot busy.
 * @param now_ms: current stream time, the clock of the slots' last_ms
 * (dmr_demod_status_t.now_ms).
 * @return DMR_TX_CHECK_OK, or the first reason against the call: the colour
 * code before the busy slot.
 */
dmr_tx_check_t dmr_tx_check(const dmr_slot_info_t slots[2], int slot_idx,
                            uint8_t cc, uint32_t src, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TX_CHECK_H */
