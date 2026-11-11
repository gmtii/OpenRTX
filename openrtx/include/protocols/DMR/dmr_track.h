/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TRACK_H
#define DMR_TRACK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stations heard with an LRRP position: the last fix of each radio, for the
 * UI. The receiver this decoder comes from drew them on a map; OpenRTX
 * lists them. Fed by the decoder in the rtx thread, read by the UI.
 */

/* Radios kept; the one heard longest ago makes room for a new one */
#define DMR_TRACK_MAX 16

#define DMR_TRACK_ALIAS 32

typedef struct {
    uint32_t src;               /**< Radio ID */
    char alias[DMR_TRACK_ALIAS]; /**< Talker alias, "" if none */
    float lat, lon;             /**< Degrees, + = N / E */
    int16_t speed_kmh;          /**< -1 unknown */
    int16_t dir;                /**< Heading in degrees, -1 unknown */
    long long tick;             /**< getTick() of the last fix */
    uint16_t reports;           /**< Fixes received */
} dmr_station_t;

/**
 * Record a position report.
 *
 * @param src: reporting radio.
 * @param alias: its talker alias, NULL or "" if unknown (the one known is
 * kept).
 */
void dmr_track_add(uint32_t src, float lat, float lon, int16_t speed_kmh,
                   int16_t dir, const char *alias);

/**
 * A talker alias was decoded: name the station, if it is in the table.
 */
void dmr_track_set_alias(uint32_t src, const char *alias);

/**
 * Copy the stations, the most recent fix first.
 *
 * @param out: destination.
 * @param max: entries in out.
 * @return number of stations copied.
 */
int dmr_track_get(dmr_station_t *out, int max);

/**
 * @return number of stations, for a change test without a copy.
 */
int dmr_track_count(void);

/**
 * Forget every station (new channel, mode left).
 */
void dmr_track_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TRACK_H */
