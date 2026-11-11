/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_SCAN_H
#define OPENRTX_TETRA_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include "protocols/TETRA/TetraCell.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TETRA site discovery: the serving cell and the neighbours it announces
 * are visited one by one, measuring the RSSI and trying to synchronise to
 * each. Requested by the UI; the TETRA mode runs it in the RTX thread, in
 * place of normal reception (see OpMode_TETRA).
 */

/* The serving cell and each neighbour */
#define TETRA_SCAN_MAX (TETRA_NEIGHBOURS_KEPT + 1)

/* cellId of the serving cell's entry */
#define TETRA_SCAN_SERVING 0xff

struct tetraScanResult {
    uint8_t cellId;    /**< Neighbour cell identifier, or TETRA_SCAN_SERVING */
    uint32_t dlHz;     /**< Downlink frequency */
    bool measured;     /**< Visited: the fields below are valid */
    int16_t rssi;      /**< Mean RSSI, dBm */
    bool sync;         /**< Burst synchronisation reached */
    uint16_t phaseErr; /**< Mean phase error while in sync, % of pi/4 */
    bool cell;         /**< mcc, mnc and colourCode are valid */
    uint16_t mcc;
    uint16_t mnc;
    uint8_t colourCode;
    bool sysinfo;      /**< la is valid */
    uint16_t la;
};

/**
 * Start a scan of the serving cell and its neighbours; nothing while one
 * runs. From the UI.
 */
void tetraScan_start(void);

/**
 * Stop the scan in progress: the entries already visited are kept.
 * From the UI.
 */
void tetraScan_cancel(void);

/**
 * @return true while a scan is wanted or running.
 */
bool tetraScan_running(void);

/**
 * Copy the scan results, sorted: the cells reached first, then by RSSI,
 * strongest first. Entries not visited yet come last, in visiting order.
 *
 * @param out: destination, TETRA_SCAN_MAX entries.
 * @param done: if not NULL, number of entries visited.
 * @return number of entries, 0 if no scan was made.
 */
int tetraScan_results(struct tetraScanResult *out, int *done);

/**
 * @return number of scan entries, 0 if no scan was made.
 */
int tetraScan_count(void);

/**
 * Forget the results (new frequency, mode change).
 */
void tetraScan_reset(void);

/* For the TETRA mode */
struct tetra_event;

/**
 * Begin the scan requested: build the list of cells to visit.
 *
 * @param servingHz: frequency the radio is tuned to.
 * @return number of cells to visit, 0 if there is nothing to scan (the scan
 * is then over).
 */
int tetraScan_begin(uint32_t servingHz);

/**
 * @param i: entry to visit.
 * @return its downlink frequency.
 */
uint32_t tetraScan_freq(int i);

/**
 * Record the RSSI of the entry being visited.
 */
void tetraScan_setRssi(int i, int16_t rssi);

/**
 * libtetra event callback during the scan: records the synchronisation and
 * the cell identity on the entry being visited.
 */
void tetraScan_onEvent(void *user, const struct tetra_event *ev);

/**
 * @return true once the entry being visited reached synchronisation.
 */
bool tetraScan_synced(int i);

/**
 * @return true once the entry being visited sent its system information.
 */
bool tetraScan_sysinfo(int i);

/**
 * Close the visit of an entry.
 *
 * @param i: entry.
 * @param phaseErr: mean phase error while in sync.
 */
void tetraScan_visited(int i, uint16_t phaseErr);

/**
 * The scan is over (all visited, cancelled, or the mode left).
 */
void tetraScan_end(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_SCAN_H */
