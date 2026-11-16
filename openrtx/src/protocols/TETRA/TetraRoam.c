/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <string.h>
#include "protocols/TETRA/TetraCell.h"
#include "protocols/TETRA/TetraRoam.h"

static pthread_mutex_t roamMutex = PTHREAD_MUTEX_INITIALIZER;
static struct tetraRoamInfo roam; /* Under roamMutex */
static uint32_t movedHz;          /* Under roamMutex */

/*
 * The neighbours of the cell received that are in the network: those of
 * the cell's own network are announced without one (0xffff).
 */
static int networkNeighbours(struct tetraNeighbourInfo *nb, uint16_t mcc,
                             uint16_t mnc)
{
    int n = tetraCell_neighbours(nb, TETRA_NEIGHBOURS_KEPT);
    int kept = 0;
    for (int i = 0; i < n; i++) {
        bool same = (nb[i].mcc == 0xffff)
                 || (nb[i].mcc == mcc && nb[i].mnc == mnc);
        if (same && nb[i].dlHz != 0)
            nb[kept++] = nb[i];
    }
    return kept;
}

/* Under roamMutex */
static int findCell(uint32_t hz)
{
    for (int i = 0; i < roam.count; i++) {
        if (roam.hz[i] == hz)
            return i;
    }
    return -1;
}

/* Under roamMutex: add a cell if new and there is room */
static void addCell(uint32_t hz, uint8_t cellId)
{
    if (findCell(hz) >= 0 || roam.count >= TETRA_ROAM_MAX)
        return;
    roam.hz[roam.count] = hz;
    roam.cellId[roam.count] = cellId;
    roam.count++;
}

/* Under roamMutex: the table from a cell and its neighbours */
static void setTable(uint32_t hz, const struct tetraNeighbourInfo *nb, int n)
{
    roam.count = 0;
    addCell(hz, TETRA_SCAN_SERVING);
    for (int i = 0; i < n; i++)
        addCell(nb[i].dlHz, nb[i].cellId);
    roam.cellHz = hz;
    roam.awaiting = false;
}

bool tetraRoam_start(uint32_t hz)
{
    static struct tetraNeighbourInfo nb[TETRA_NEIGHBOURS_KEPT];
    struct tetraCellInfo c;
    tetraCell_get(&c);
    if (!c.valid)
        return false;
    int n = networkNeighbours(nb, c.mcc, c.mnc);

    pthread_mutex_lock(&roamMutex);
    uint32_t moves = roam.moves;
    memset(&roam, 0, sizeof(roam));
    /* The UI keeps counting from where it was */
    roam.moves = moves;
    roam.state = TETRA_ROAM_LISTENING;
    roam.mcc = c.mcc;
    roam.mnc = c.mnc;
    setTable(hz, nb, n);
    movedHz = 0;
    pthread_mutex_unlock(&roamMutex);
    return true;
}

void tetraRoam_stop(void)
{
    pthread_mutex_lock(&roamMutex);
    uint32_t moves = roam.moves;
    memset(&roam, 0, sizeof(roam));
    /* The UI keeps counting from where it was */
    roam.moves = moves;
    pthread_mutex_unlock(&roamMutex);
}

bool tetraRoam_on(void)
{
    pthread_mutex_lock(&roamMutex);
    bool on = (roam.state != TETRA_ROAM_OFF);
    pthread_mutex_unlock(&roamMutex);
    return on;
}

void tetraRoam_get(struct tetraRoamInfo *out)
{
    pthread_mutex_lock(&roamMutex);
    *out = roam;
    pthread_mutex_unlock(&roamMutex);
}

uint32_t tetraRoam_moves(uint32_t *hz)
{
    pthread_mutex_lock(&roamMutex);
    uint32_t moves = roam.moves;
    *hz = movedHz;
    pthread_mutex_unlock(&roamMutex);
    return moves;
}

void tetraRoam_refresh(uint32_t hz)
{
    static struct tetraNeighbourInfo nb[TETRA_NEIGHBOURS_KEPT];

    pthread_mutex_lock(&roamMutex);
    const bool on = (roam.state != TETRA_ROAM_OFF);
    const uint16_t mcc = roam.mcc;
    const uint16_t mnc = roam.mnc;
    pthread_mutex_unlock(&roamMutex);
    if (!on)
        return;

    /* Only a cell of the network, received correctly */
    struct tetraCellInfo c;
    tetraCell_get(&c);
    if (!c.valid || !c.sysinfo || c.mcc != mcc || c.mnc != mnc)
        return;
    int n = networkNeighbours(nb, mcc, mnc);

    pthread_mutex_lock(&roamMutex);
    if (roam.state == TETRA_ROAM_OFF || roam.mcc != mcc || roam.mnc != mnc) {
        /* Stopped or restarted meanwhile */
    } else if (hz == roam.cellHz) {
        /* The same cell: the neighbours announced since are added */
        for (int i = 0; i < n; i++)
            addCell(nb[i].dlHz, nb[i].cellId);
    } else if (n > 0) {
        /* A new cell with its neighbours: the table is now its own */
        setTable(hz, nb, n);
    } else {
        /* A new cell, its neighbours not announced yet: the old table is
         * kept until they are, with the new cell in it */
        roam.awaiting = true;
        if (findCell(hz) < 0) {
            if (roam.count >= TETRA_ROAM_MAX)
                roam.count--;
            addCell(hz, TETRA_SCAN_SERVING);
        }
    }
    pthread_mutex_unlock(&roamMutex);
}

int tetraRoam_cells(uint32_t *hz, uint8_t *cellId)
{
    pthread_mutex_lock(&roamMutex);
    int n = roam.count;
    memcpy(hz, roam.hz, n * sizeof(*hz));
    memcpy(cellId, roam.cellId, n * sizeof(*cellId));
    pthread_mutex_unlock(&roamMutex);
    return n;
}

bool tetraRoam_network(uint16_t mcc, uint16_t mnc)
{
    pthread_mutex_lock(&roamMutex);
    bool same = (roam.state != TETRA_ROAM_OFF) && (roam.mcc == mcc)
             && (roam.mnc == mnc);
    pthread_mutex_unlock(&roamMutex);
    return same;
}

void tetraRoam_setState(uint8_t state, long long retryAt)
{
    pthread_mutex_lock(&roamMutex);
    if (roam.state != TETRA_ROAM_OFF) {
        roam.state = state;
        roam.retryAt = retryAt;
    }
    pthread_mutex_unlock(&roamMutex);
}

void tetraRoam_moved(uint32_t hz)
{
    pthread_mutex_lock(&roamMutex);
    if (roam.state != TETRA_ROAM_OFF) {
        roam.moves++;
        movedHz = hz;
    }
    pthread_mutex_unlock(&roamMutex);
}
