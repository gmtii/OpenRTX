/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "interfaces/delays.h"
#include "dmr_port.h"
#include "dmr_track.h"

DMR_LOCK_DECLARE(s_lock);

/* Under s_lock. The table is in PSRAM, not zeroed at boot: s_ready says it
 * was cleared. */
static dmr_station_t s_st[DMR_TRACK_MAX] EXT_RAM_BSS_ATTR;
static int s_count;
static bool s_ready;

void dmr_track_clear(void)
{
    DMR_LOCK(s_lock);
    memset(s_st, 0, sizeof(s_st));
    s_count = 0;
    s_ready = true;
    DMR_UNLOCK(s_lock);
}

static dmr_station_t *find(uint32_t src)
{
    for (int i = 0; i < s_count; i++) {
        if (s_st[i].src == src)
            return &s_st[i];
    }
    return NULL;
}

void dmr_track_add(uint32_t src, float lat, float lon, int16_t speed_kmh,
                   int16_t dir, const char *alias)
{
    long long now = getTick();

    DMR_LOCK(s_lock);
    if (!s_ready) {
        memset(s_st, 0, sizeof(s_st));
        s_count = 0;
        s_ready = true;
    }

    dmr_station_t *s = find(src);
    if (s == NULL) {
        if (s_count < DMR_TRACK_MAX) {
            s = &s_st[s_count++];
        } else {
            s = &s_st[0];
            for (int i = 1; i < s_count; i++) {
                if (s_st[i].tick < s->tick)
                    s = &s_st[i];
            }
        }
        memset(s, 0, sizeof(*s));
        s->src = src;
    }
    s->lat = lat;
    s->lon = lon;
    s->speed_kmh = speed_kmh;
    s->dir = dir;
    s->tick = now;
    if (s->reports < UINT16_MAX)
        s->reports++;
    if (alias != NULL && alias[0] != '\0')
        snprintf(s->alias, sizeof(s->alias), "%s", alias);
    DMR_UNLOCK(s_lock);
}

void dmr_track_set_alias(uint32_t src, const char *alias)
{
    if (alias == NULL || alias[0] == '\0')
        return;

    DMR_LOCK(s_lock);
    dmr_station_t *s = s_ready ? find(src) : NULL;
    if (s != NULL)
        snprintf(s->alias, sizeof(s->alias), "%s", alias);
    DMR_UNLOCK(s_lock);
}

int dmr_track_get(dmr_station_t *out, int max)
{
    DMR_LOCK(s_lock);
    int n = s_ready ? s_count : 0;

    /* Newest first: the table itself is kept in that order, insertion
     * sort, few entries */
    for (int i = 1; i < n; i++) {
        dmr_station_t e = s_st[i];
        int j = i;
        for (; j > 0 && s_st[j - 1].tick < e.tick; j--)
            s_st[j] = s_st[j - 1];
        s_st[j] = e;
    }

    if (n > max)
        n = max;
    memcpy(out, s_st, (size_t)n * sizeof(*out));
    DMR_UNLOCK(s_lock);
    return n;
}

int dmr_track_count(void)
{
    DMR_LOCK(s_lock);
    int n = s_ready ? s_count : 0;
    DMR_UNLOCK(s_lock);
    return n;
}
