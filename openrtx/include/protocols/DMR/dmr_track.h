/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TRACK_H
#define DMR_TRACK_H

#include <stdint.h>

/*
 * Station position tracks feed a map in the receiver this decoder comes
 * from. OpenRTX has no map: positions stay in the slot info and the call
 * log, and these hooks do nothing.
 */

static inline void dmr_track_add(uint32_t src, float lat, float lon,
                                 int16_t speed_kmh, int16_t dir,
                                 const char *alias)
{
    (void)src;
    (void)lat;
    (void)lon;
    (void)speed_kmh;
    (void)dir;
    (void)alias;
}

static inline void dmr_track_set_alias(uint32_t src, const char *alias)
{
    (void)src;
    (void)alias;
}

#endif /* DMR_TRACK_H */
