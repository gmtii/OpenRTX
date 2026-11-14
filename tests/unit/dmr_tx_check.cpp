/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "protocols/DMR/dmr_tx_check.h"
}

static const uint8_t OWN_CC = 1;
static const uint32_t OWN_ID = 2140001;
static const uint32_t OTHER_ID = 2140002;
static const uint32_t NOW = 100000;

/* Both slots as a repeater idling on CC 1, last heard now */
static void idleRepeater(dmr_slot_info_t slots[2])
{
    memset(slots, 0, 2 * sizeof(dmr_slot_info_t));
    for (int i = 0; i < 2; i++) {
        slots[i].activity = DMR_ACT_IDLE;
        slots[i].cc = OWN_CC;
        slots[i].last_ms = NOW - 30;
    }
}

static void talking(dmr_slot_info_t *s, uint32_t src)
{
    s->activity = DMR_ACT_VOICE;
    s->ids_valid = true;
    s->src = src;
}

TEST_CASE("DMR TX check: an idle repeater on our colour code",
          "[dmr][tx_check]")
{
    dmr_slot_info_t slots[2];
    idleRepeater(slots);
    REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW) == DMR_TX_CHECK_OK);
    REQUIRE(dmr_tx_check(slots, 1, OWN_CC, OWN_ID, NOW) == DMR_TX_CHECK_OK);

    slots[0].activity = DMR_ACT_CSBK;
    REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW) == DMR_TX_CHECK_OK);
}

TEST_CASE("DMR TX check: nothing heard", "[dmr][tx_check]")
{
    dmr_slot_info_t slots[2];
    memset(slots, 0, sizeof(slots));
    slots[0].cc = slots[1].cc = -1;
    REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW) == DMR_TX_CHECK_OK);
}

TEST_CASE("DMR TX check: another colour code", "[dmr][tx_check]")
{
    dmr_slot_info_t slots[2];
    idleRepeater(slots);

    SECTION("on our slot")
    {
        slots[0].cc = 3;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_CC);
    }
    SECTION("on the other slot")
    {
        slots[1].cc = 3;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_CC);
    }
    SECTION("before the busy slot")
    {
        slots[0].cc = 3;
        talking(&slots[0], OTHER_ID);
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_CC);
    }
    SECTION("unknown")
    {
        slots[0].cc = slots[1].cc = -1;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_OK);
    }
    SECTION("heard too long ago")
    {
        slots[1].cc = 3;
        slots[1].last_ms = NOW - DMR_TX_CHECK_FRESH_MS;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_OK);
    }
    SECTION("checked whatever the slot asked")
    {
        slots[1].cc = 3;
        REQUIRE(dmr_tx_check(slots, -1, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_CC);
    }
}

TEST_CASE("DMR TX check: a station on our slot", "[dmr][tx_check]")
{
    dmr_slot_info_t slots[2];
    idleRepeater(slots);

    SECTION("voice")
    {
        talking(&slots[1], OTHER_ID);
        REQUIRE(dmr_tx_check(slots, 1, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_BUSY);
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_OK);
    }
    SECTION("data")
    {
        slots[0].activity = DMR_ACT_DATA;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_BUSY);
    }
    SECTION("voice with no IDs decoded yet")
    {
        slots[0].activity = DMR_ACT_VOICE;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_BUSY);
    }
    SECTION("our own call relayed")
    {
        talking(&slots[0], OWN_ID);
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_OK);
    }
    SECTION("the call ended (terminator: idle)")
    {
        talking(&slots[0], OTHER_ID);
        slots[0].activity = DMR_ACT_IDLE;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_OK);
    }
    SECTION("last heard at the edge of the window")
    {
        talking(&slots[0], OTHER_ID);
        slots[0].last_ms = NOW - (DMR_TX_CHECK_FRESH_MS - 1);
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_BUSY);
        slots[0].last_ms = NOW - DMR_TX_CHECK_FRESH_MS;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, NOW)
                == DMR_TX_CHECK_OK);
    }
    SECTION("across the wrap of the stream clock")
    {
        talking(&slots[0], OTHER_ID);
        slots[0].last_ms = 0xFFFFFF00u;
        REQUIRE(dmr_tx_check(slots, 0, OWN_CC, OWN_ID, 0x40u)
                == DMR_TX_CHECK_BUSY);
    }
}
