/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Groups and radios heard, after the ESP32-P4 SDR's tables
 * (main/tetra/tetra_l3.c, group_seen() and radio_seen()), fed with
 * libtetra's events.
 */

#include <pthread.h>
#include <string.h>
#include <tetra/tetra.h>
#include "interfaces/delays.h"
#include "protocols/TETRA/TetraHeard.h"
#include "protocols/TETRA/TetraSds.h"

/* Group and individual addresses from 0xfffffc on are special */
#define SSI_SPECIAL 0xfffffcu

static pthread_mutex_t heardMutex = PTHREAD_MUTEX_INITIALIZER;
static struct tetraRadio radios[TETRA_HEARD_RADIOS]; /* Under heardMutex */
static struct tetraGroup groups[TETRA_HEARD_GROUPS];

/**
 * The entry of a radio, a new one if needed: a free one, else the one
 * heard longest ago.
 */
static struct tetraRadio *radioGet(uint32_t issi, long long now)
{
    struct tetraRadio *r = NULL, *spare = NULL;
    for (int i = 0; i < TETRA_HEARD_RADIOS; i++) {
        struct tetraRadio *e = &radios[i];
        if (e->issi == issi)
            return e;
        if (spare == NULL || e->issi == 0
            || (spare->issi != 0 && e->last < spare->last))
            spare = e;
    }
    r = spare;
    memset(r, 0, sizeof(*r));
    r->issi = issi;
    r->first = now;
    return r;
}

static struct tetraGroup *groupGet(uint32_t gssi, long long now)
{
    struct tetraGroup *g = NULL, *spare = NULL;
    for (int i = 0; i < TETRA_HEARD_GROUPS; i++) {
        struct tetraGroup *e = &groups[i];
        if (e->gssi == gssi)
            return e;
        if (spare == NULL || e->gssi == 0
            || (spare->gssi != 0 && e->last < spare->last))
            spare = e;
    }
    g = spare;
    memset(g, 0, sizeof(*g));
    g->gssi = gssi;
    g->first = now;
    return g;
}

/**
 * Record activity of a radio. Under heardMutex.
 */
static struct tetraRadio *radioSeen(uint32_t issi, uint8_t activity,
                                    uint32_t group, long long now)
{
    if (issi == 0 || issi >= SSI_SPECIAL)
        return NULL;

    struct tetraRadio *r = radioGet(issi, now);
    r->last = now;
    r->activity = activity;
    if (group != 0)
        r->lastGroup = group;
    return r;
}

/**
 * Record a radio in a group, if new to it. Under heardMutex.
 */
static void groupAddRadio(struct tetraGroup *g, uint32_t issi)
{
    if (issi == 0 || issi >= SSI_SPECIAL)
        return;
    for (int i = 0; i < g->nradios; i++) {
        if (g->radios[i] == issi)
            return;
    }
    if (g->nradios < TETRA_GROUP_RADIOS)
        g->radios[g->nradios++] = issi;
}

void tetraHeard_reset(void)
{
    pthread_mutex_lock(&heardMutex);
    memset(radios, 0, sizeof(radios));
    memset(groups, 0, sizeof(groups));
    pthread_mutex_unlock(&heardMutex);
}

void tetraHeard_call(const struct tetra_call *c)
{
    bool setup = (c->what == TETRA_CALL_SETUP);
    bool grant = (c->what == TETRA_CALL_TX_GRANTED);
    if (!setup && !grant && c->what != TETRA_CALL_INFO)
        return;

    long long now = getTick();
    uint32_t group = c->group ? c->dst_ssi : 0;

    pthread_mutex_lock(&heardMutex);
    struct tetraRadio *r = radioSeen(
        c->src_ssi, grant ? TETRA_RADIO_TALK : TETRA_RADIO_CALL, group, now);
    if (r != NULL && (setup || grant) && r->calls < UINT16_MAX)
        r->calls++;

    /* The called radio of an individual call is heard of too */
    if (!c->group && setup)
        radioSeen(c->dst_ssi, TETRA_RADIO_CALL, 0, now);

    if (group != 0 && group < SSI_SPECIAL) {
        struct tetraGroup *g = groupGet(group, now);
        g->last = now;
        g->encrypted = c->encrypted;
        if (setup && g->calls < UINT16_MAX)
            g->calls++;
        if (grant && g->tx < UINT16_MAX)
            g->tx++;
        if (c->src_ssi != 0 && c->src_ssi < SSI_SPECIAL) {
            g->lastRadio = c->src_ssi;
            groupAddRadio(g, c->src_ssi);
        }
    }
    pthread_mutex_unlock(&heardMutex);
}

void tetraHeard_sds(const struct tetraSds *m)
{
    long long now = getTick();

    pthread_mutex_lock(&heardMutex);
    struct tetraRadio *r = radioSeen(m->from, TETRA_RADIO_SDS, 0, now);
    if (r != NULL) {
        if (r->sds < UINT16_MAX)
            r->sds++;
        if (m->pos) {
            r->pos = true;
            r->lat = m->lat;
            r->lon = m->lon;
        }
    }
    pthread_mutex_unlock(&heardMutex);
}

void tetraHeard_mm(const struct tetra_mm *mm)
{
    if (mm->type != TETRA_MM_LOC_UPDATE_ACCEPT
        && mm->type != TETRA_MM_GROUP_ATTACH_DETACH
        && mm->type != TETRA_MM_GROUP_ATTACH_DETACH_ACK)
        return;

    long long now = getTick();
    uint32_t issi = (mm->new_ssi != 0) ? mm->new_ssi : mm->ssi;

    pthread_mutex_lock(&heardMutex);
    radioSeen(issi, TETRA_RADIO_REG, 0, now);
    for (int i = 0; i < mm->ngroups && i < TETRA_MM_MAX_GROUPS; i++) {
        const struct tetra_mm_group *a = &mm->groups[i];
        uint32_t gssi = (a->gssi != 0) ? a->gssi : a->vgssi;
        if (a->detach || gssi == 0 || gssi >= SSI_SPECIAL)
            continue;
        struct tetraGroup *g = groupGet(gssi, now);
        g->last = now;
        groupAddRadio(g, issi);
    }
    pthread_mutex_unlock(&heardMutex);
}

int tetraHeard_radios(struct tetraRadio *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&heardMutex);
    for (int i = 0; i < TETRA_HEARD_RADIOS && n < max; i++) {
        if (radios[i].issi != 0)
            out[n++] = radios[i];
    }
    pthread_mutex_unlock(&heardMutex);

    /* Most recent first */
    for (int i = 1; i < n; i++) {
        struct tetraRadio e = out[i];
        int j = i;
        for (; j > 0 && out[j - 1].last < e.last; j--)
            out[j] = out[j - 1];
        out[j] = e;
    }
    return n;
}

int tetraHeard_groups(struct tetraGroup *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&heardMutex);
    for (int i = 0; i < TETRA_HEARD_GROUPS && n < max; i++) {
        if (groups[i].gssi != 0)
            out[n++] = groups[i];
    }
    pthread_mutex_unlock(&heardMutex);

    /* Most recent first */
    for (int i = 1; i < n; i++) {
        struct tetraGroup e = out[i];
        int j = i;
        for (; j > 0 && out[j - 1].last < e.last; j--)
            out[j] = out[j - 1];
        out[j] = e;
    }
    return n;
}
