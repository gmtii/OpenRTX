/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_ROAM_H
#define OPENRTX_TETRA_ROAM_H

#include <stdbool.h>
#include <stdint.h>
#include "protocols/TETRA/TetraScan.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TETRA roaming, while moving between cells of one network: started on a
 * cell received (its MCC and MNC are the network's), it keeps that cell
 * and the neighbours it announces as a table of cells. When the burst
 * synchronisation is lost, the TETRA mode scans the table, as the site scan
 * does, and tunes the strongest cell of the network found there; else it
 * tries again a while later.
 *
 * The table is not lost with the cell: it is replaced only when a new cell
 * of the network is received correctly (synchronisation, system
 * information) and has announced its neighbours. Cells of other networks
 * are never tuned.
 */

/* The cell and the neighbours it announces */
#define TETRA_ROAM_MAX TETRA_SCAN_MAX

enum tetraRoamState {
    TETRA_ROAM_OFF = 0,
    TETRA_ROAM_LISTENING, /**< On a cell, or waiting for it to come back */
    TETRA_ROAM_SCANNING,  /**< Scanning the table */
    TETRA_ROAM_WAITING,   /**< None found: scanning again later */
};

struct tetraRoamInfo {
    uint8_t state;     /**< enum tetraRoamState */
    uint16_t mcc;      /**< The network roamed in */
    uint16_t mnc;
    uint32_t cellHz;   /**< The cell the table comes from */
    bool awaiting;     /**< A new cell received: its neighbours awaited */
    int count;         /**< Cells in the table */
    uint32_t hz[TETRA_ROAM_MAX];  /**< First the cell, then its neighbours */
    uint8_t cellId[TETRA_ROAM_MAX];
    uint32_t moves;    /**< Cells tuned by roaming so far */
    long long retryAt; /**< While waiting: tick of the next scan */
};

/**
 * Start roaming from the cell received on a frequency: its network, and the
 * table from it and its neighbours. From the UI.
 *
 * @param hz: frequency the cell is received on.
 * @return false if no cell was received there yet.
 */
bool tetraRoam_start(uint32_t hz);

/**
 * Stop roaming, forgetting the table. From the UI, or the TETRA mode left.
 */
void tetraRoam_stop(void);

/**
 * @return true while roaming.
 */
bool tetraRoam_on(void);

/**
 * Copy the roaming state and table.
 */
void tetraRoam_get(struct tetraRoamInfo *out);

/**
 * The cell last tuned by roaming, for the UI to follow it.
 *
 * @param hz: its frequency, filled in.
 * @return number of cells tuned by roaming so far: a new one when it grows.
 */
uint32_t tetraRoam_moves(uint32_t *hz);

/* For the TETRA mode */

/**
 * The cell received on a frequency: if it is the network's, received
 * correctly, the table follows it (see above).
 *
 * @param hz: frequency the radio is tuned to.
 */
void tetraRoam_refresh(uint32_t hz);

/**
 * The table to scan.
 *
 * @param hz: frequencies, TETRA_ROAM_MAX entries.
 * @param cellId: their cell identifiers, TETRA_ROAM_MAX entries.
 * @return number of cells.
 */
int tetraRoam_cells(uint32_t *hz, uint8_t *cellId);

/**
 * Whether a cell found by a scan is in the network roamed in.
 */
bool tetraRoam_network(uint16_t mcc, uint16_t mnc);

/**
 * The state of the roaming, for the UI.
 *
 * @param state: enum tetraRoamState, not TETRA_ROAM_OFF.
 * @param retryAt: for TETRA_ROAM_WAITING, tick of the next scan.
 */
void tetraRoam_setState(uint8_t state, long long retryAt);

/**
 * A cell was tuned by roaming.
 */
void tetraRoam_moved(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_ROAM_H */
