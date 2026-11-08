/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <string.h>
#include <tetra/tetra.h>
#include "interfaces/delays.h"
#include "protocols/TETRA/TetraCell.h"

static pthread_mutex_t cellMutex = PTHREAD_MUTEX_INITIALIZER;
static struct tetraCellInfo cell; /* Under cellMutex */
static struct tetraNeighbourInfo neighbours[TETRA_NEIGHBOURS_KEPT];
static int nNeighbours;

/* Network time: UTC seconds since the start of the year at a tick */
static bool timeKnown;
static uint16_t timeYear;
static uint32_t timeUtc;
static int16_t timeOffset;
static long long timeTick;

static const int32_t offsetHz[4] = { 0, 6250, -6250, 12500 };

/* TS 100 392-15 table 2: duplex spacing in kHz per (code, band), -1 none */
static const int32_t duplexKhz[8][10] = {
    { -1, 1600, 10000, 10000, 10000, 10000, 10000, -1, -1, -1 },
    { -1, 4500, -1, 36000, 7000, -1, -1, -1, 45000, 45000 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -1, -1, -1, 8000, 8000, -1, -1, -1, 18000, 18000 },
    { -1, -1, -1, 18000, 5000, -1, 30000, 30000, -1, 39000 },
    { -1, -1, -1, -1, 9500, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};

static uint32_t dlHz(uint8_t band, uint16_t carrier, uint8_t offset)
{
    if (band == 0 || band > 9)
        return 0;
    return (uint32_t)band * 100000000u + (uint32_t)carrier * 25000u
         + (uint32_t)offsetHz[offset & 3];
}

void tetraCell_reset(void)
{
    pthread_mutex_lock(&cellMutex);
    memset(&cell, 0, sizeof(cell));
    memset(neighbours, 0, sizeof(neighbours));
    nNeighbours = 0;
    pthread_mutex_unlock(&cellMutex);
}

void tetraCell_onCell(const struct tetra_cell *c)
{
    pthread_mutex_lock(&cellMutex);
    cell.valid = true;
    cell.mcc = c->mcc;
    cell.mnc = c->mnc;
    cell.colourCode = c->colour_code;
    cell.systemCode = c->system_code;
    cell.sharing = c->sharing_mode;
    cell.hyperframe = c->hyperframe;
    cell.cck = c->cck_id;
    if (c->have_sysinfo) {
        cell.sysinfo = true;
        cell.la = c->location_area;
        cell.carrier = c->main_carrier;
        cell.band = c->band;
        cell.offset = c->offset;
        cell.duplex = c->duplex_spacing;
        cell.reverse = c->reverse_operation;
        cell.dlHz = c->dl_freq_hz;
        cell.services = c->service_details;
        if (c->air_encryption)
            cell.services |= TETRA_SERV_AIE;

        int32_t sp = (c->band <= 9) ?
                         duplexKhz[c->duplex_spacing & 7][c->band] :
                         -1;
        if (cell.dlHz != 0 && sp >= 0)
            cell.ulHz = c->reverse_operation ? cell.dlHz + sp * 1000 :
                                               cell.dlHz - sp * 1000;
        else
            cell.ulHz = 0;
    }
    pthread_mutex_unlock(&cellMutex);
}

void tetraCell_onNetwork(const struct tetra_network *n)
{
    long long now = getTick();

    pthread_mutex_lock(&cellMutex);
    cell.load = n->load;

    if (n->has_time && n->time_valid) {
        timeKnown = true;
        timeYear = n->year;
        timeUtc = n->utc_s;
        timeOffset = n->offset_valid ? n->offset_min : 0;
        timeTick = now;
    }

    for (int i = 0; i < n->nneighbours && i < TETRA_MAX_NEIGHBOURS; i++) {
        const struct tetra_neighbour *a = &n->neighbour[i];
        uint32_t f = a->dl_freq_hz;
        if (f == 0) {
            /* Band and offset of the serving cell when not given */
            f = a->has_ext ? dlHz(a->band, a->main_carrier, a->offset) :
                             dlHz(cell.band, a->main_carrier, cell.offset);
        }

        /* The same cell again, else a free entry, else the oldest */
        int k;
        for (k = 0; k < nNeighbours; k++) {
            if (neighbours[k].cellId == a->cell_id && neighbours[k].dlHz == f)
                break;
        }
        if (k == nNeighbours) {
            if (nNeighbours < TETRA_NEIGHBOURS_KEPT) {
                nNeighbours++;
            } else {
                k = 0;
                for (int j = 1; j < nNeighbours; j++) {
                    if (neighbours[j].last < neighbours[k].last)
                        k = j;
                }
            }
        }

        struct tetraNeighbourInfo *e = &neighbours[k];
        e->cellId = a->cell_id;
        e->dlHz = f;
        e->load = a->load;
        e->synchronised = a->synchronised;
        e->mcc = a->mcc;
        e->mnc = a->mnc;
        e->la = a->la;
        e->last = now;
    }
    pthread_mutex_unlock(&cellMutex);
}

void tetraCell_get(struct tetraCellInfo *out)
{
    pthread_mutex_lock(&cellMutex);
    *out = cell;
    pthread_mutex_unlock(&cellMutex);
}

int tetraCell_neighbours(struct tetraNeighbourInfo *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&cellMutex);
    for (int i = 0; i < nNeighbours && n < max; i++)
        out[n++] = neighbours[i];
    pthread_mutex_unlock(&cellMutex);

    /* By frequency */
    for (int i = 1; i < n; i++) {
        struct tetraNeighbourInfo e = out[i];
        int j = i;
        for (; j > 0 && out[j - 1].dlHz > e.dlHz; j--)
            out[j] = out[j - 1];
        out[j] = e;
    }
    return n;
}

static bool leap(int y)
{
    return ((y % 4) == 0 && (y % 100) != 0) || ((y % 400) == 0);
}

bool tetraCell_time(struct tetraTime *out)
{
    pthread_mutex_lock(&cellMutex);
    bool known = timeKnown;
    int year = timeYear;
    int64_t s = (int64_t)timeUtc + (getTick() - timeTick) / 1000
              + (int64_t)timeOffset * 60;
    int16_t offset = timeOffset;
    pthread_mutex_unlock(&cellMutex);
    if (!known)
        return false;

    /* Seconds since the start of the year, into the right year */
    while (s < 0) {
        year--;
        s += (leap(year) ? 366 : 365) * 86400LL;
    }
    while (s >= (leap(year) ? 366 : 365) * 86400LL) {
        s -= (leap(year) ? 366 : 365) * 86400LL;
        year++;
    }

    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31 };
    int day = (int)(s / 86400);
    int month = 0;
    while (month < 11) {
        int dm = days[month] + ((month == 1 && leap(year)) ? 1 : 0);
        if (day < dm)
            break;
        day -= dm;
        month++;
    }

    int32_t sod = (int32_t)(s % 86400);
    out->year = (uint16_t)year;
    out->month = (uint8_t)(month + 1);
    out->day = (uint8_t)(day + 1);
    out->hour = (uint8_t)(sod / 3600);
    out->minute = (uint8_t)((sod / 60) % 60);
    out->second = (uint8_t)(sod % 60);
    out->offsetMin = offset;
    return true;
}
