/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <string.h>
#include <tetra/tetra.h>
#include "protocols/TETRA/TetraScan.h"

static pthread_mutex_t scanMutex = PTHREAD_MUTEX_INITIALIZER;

/* Under scanMutex */
static struct tetraScanResult results[TETRA_SCAN_MAX];
static int count;      /* Entries in results */
static int visiting;   /* Entry the events belong to, -1 none */
static bool wanted;    /* Requested by the UI, until the scan is over */

void tetraScan_start(void)
{
    pthread_mutex_lock(&scanMutex);
    wanted = true;
    pthread_mutex_unlock(&scanMutex);
}

void tetraScan_cancel(void)
{
    pthread_mutex_lock(&scanMutex);
    wanted = false;
    pthread_mutex_unlock(&scanMutex);
}

bool tetraScan_running(void)
{
    pthread_mutex_lock(&scanMutex);
    bool w = wanted;
    pthread_mutex_unlock(&scanMutex);
    return w;
}

/* Reached first, then strongest; not visited last, in visiting order */
static bool before(const struct tetraScanResult *a,
                   const struct tetraScanResult *b)
{
    if (a->measured != b->measured)
        return a->measured;
    if (!a->measured)
        return false;
    if (a->sync != b->sync)
        return a->sync;
    return a->rssi > b->rssi;
}

int tetraScan_results(struct tetraScanResult *out, int *done)
{
    pthread_mutex_lock(&scanMutex);
    int n = count;
    memcpy(out, results, n * sizeof(*out));
    pthread_mutex_unlock(&scanMutex);

    int visited = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].measured)
            visited++;
    }
    if (done != NULL)
        *done = visited;

    /* Insertion sort, stable: few entries */
    for (int i = 1; i < n; i++) {
        struct tetraScanResult e = out[i];
        int j = i;
        for (; j > 0 && before(&e, &out[j - 1]); j--)
            out[j] = out[j - 1];
        out[j] = e;
    }
    return n;
}

int tetraScan_count(void)
{
    pthread_mutex_lock(&scanMutex);
    int n = count;
    pthread_mutex_unlock(&scanMutex);
    return n;
}

void tetraScan_reset(void)
{
    pthread_mutex_lock(&scanMutex);
    count = 0;
    visiting = -1;
    wanted = false;
    pthread_mutex_unlock(&scanMutex);
}

int tetraScan_begin(uint32_t servingHz)
{
    static struct tetraNeighbourInfo nb[TETRA_NEIGHBOURS_KEPT];
    int n = tetraCell_neighbours(nb, TETRA_NEIGHBOURS_KEPT);

    pthread_mutex_lock(&scanMutex);
    memset(results, 0, sizeof(results));
    results[0].cellId = TETRA_SCAN_SERVING;
    results[0].dlHz = servingHz;
    count = 1;
    for (int i = 0; i < n && count < TETRA_SCAN_MAX; i++) {
        /* A frequency unknown, or the serving one, is not visited */
        if (nb[i].dlHz == 0 || nb[i].dlHz == servingHz)
            continue;
        results[count].cellId = nb[i].cellId;
        results[count].dlHz = nb[i].dlHz;
        count++;
    }
    visiting = -1;
    pthread_mutex_unlock(&scanMutex);
    return count;
}

uint32_t tetraScan_freq(int i)
{
    pthread_mutex_lock(&scanMutex);
    uint32_t hz = (i >= 0 && i < count) ? results[i].dlHz : 0;
    visiting = i;
    pthread_mutex_unlock(&scanMutex);
    return hz;
}

void tetraScan_setRssi(int i, int16_t rssi)
{
    pthread_mutex_lock(&scanMutex);
    if (i >= 0 && i < count)
        results[i].rssi = rssi;
    pthread_mutex_unlock(&scanMutex);
}

void tetraScan_onEvent(void *user, const struct tetra_event *ev)
{
    (void)user;

    pthread_mutex_lock(&scanMutex);
    if (visiting < 0 || visiting >= count) {
        pthread_mutex_unlock(&scanMutex);
        return;
    }
    struct tetraScanResult *r = &results[visiting];
    switch (ev->type) {
        case TETRA_EV_SYNC:
            if (ev->u.sync.locked)
                r->sync = true;
            break;

        case TETRA_EV_CELL: {
            const struct tetra_cell *c = ev->u.cell;
            r->sync = true;
            r->cell = true;
            r->mcc = c->mcc;
            r->mnc = c->mnc;
            r->colourCode = c->colour_code;
            if (c->have_sysinfo) {
                r->sysinfo = true;
                r->la = c->location_area;
            }
            break;
        }

        default:
            break;
    }
    pthread_mutex_unlock(&scanMutex);
}

bool tetraScan_synced(int i)
{
    pthread_mutex_lock(&scanMutex);
    bool s = (i >= 0 && i < count) && results[i].sync;
    pthread_mutex_unlock(&scanMutex);
    return s;
}

bool tetraScan_sysinfo(int i)
{
    pthread_mutex_lock(&scanMutex);
    bool s = (i >= 0 && i < count) && results[i].sysinfo;
    pthread_mutex_unlock(&scanMutex);
    return s;
}

void tetraScan_visited(int i, uint16_t phaseErr)
{
    pthread_mutex_lock(&scanMutex);
    if (i >= 0 && i < count) {
        results[i].measured = true;
        if (results[i].sync)
            results[i].phaseErr = phaseErr;
    }
    visiting = -1;
    pthread_mutex_unlock(&scanMutex);
}

void tetraScan_end(void)
{
    pthread_mutex_lock(&scanMutex);
    visiting = -1;
    wanted = false;
    pthread_mutex_unlock(&scanMutex);
}
