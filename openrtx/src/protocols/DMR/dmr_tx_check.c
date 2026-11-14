/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include "protocols/DMR/dmr_tx_check.h"

/* Heard within DMR_TX_CHECK_FRESH_MS; the subtraction wraps with the clock */
static bool fresh(const dmr_slot_info_t *s, uint32_t now_ms)
{
    return s->activity != DMR_ACT_NONE
           && (uint32_t)(now_ms - s->last_ms) < DMR_TX_CHECK_FRESH_MS;
}

dmr_tx_check_t dmr_tx_check(const dmr_slot_info_t slots[2], int slot_idx,
                            uint8_t cc, uint32_t src, uint32_t now_ms)
{
    /* The colour code is the repeater's: either slot tells it */
    for (int i = 0; i < 2; i++) {
        if (fresh(&slots[i], now_ms) && slots[i].cc >= 0
            && slots[i].cc != (int)cc)
            return DMR_TX_CHECK_CC;
    }

    if (slot_idx < 0 || slot_idx > 1)
        return DMR_TX_CHECK_OK;

    /* Voice or data of someone else; idle and CSBK bursts are the
     * repeater's own and leave the slot free */
    const dmr_slot_info_t *s = &slots[slot_idx];
    if (!fresh(s, now_ms))
        return DMR_TX_CHECK_OK;
    if (s->activity != DMR_ACT_VOICE && s->activity != DMR_ACT_DATA)
        return DMR_TX_CHECK_OK;
    if (s->ids_valid && s->src == src)
        return DMR_TX_CHECK_OK;
    return DMR_TX_CHECK_BUSY;
}
