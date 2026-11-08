/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_CELL_H
#define OPENRTX_TETRA_CELL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TETRA serving cell, network time and neighbour cells, as on the ESP32-P4
 * SDR: from libtetra's SYNC / SYSINFO and D-NWRK-BROADCAST events.
 *
 * The network time keeps running on the radio's tick once received, and
 * is not cleared with the cell: the radio has no clock of its own.
 */

#define TETRA_NEIGHBOURS_KEPT 12

/* BS service details (EN 300 392-2 clause 21.4.4.1), 12 bits */
#define TETRA_SERV_REG (1u << 11)
#define TETRA_SERV_DEREG (1u << 10)
#define TETRA_SERV_PRIORITY (1u << 9)
#define TETRA_SERV_MIGRATION (1u << 7)
#define TETRA_SERV_SYS_WIDE (1u << 6)
#define TETRA_SERV_VOICE (1u << 5)
#define TETRA_SERV_CSD (1u << 4)
#define TETRA_SERV_PACKET (1u << 2)
#define TETRA_SERV_AIE (1u << 1)
#define TETRA_SERV_ADV_LINK (1u << 0)

struct tetraCellInfo {
    bool valid;   /**< A SYNC was received */
    bool sysinfo; /**< The fields from la on are valid */
    uint16_t mcc;
    uint16_t mnc;
    uint8_t colourCode;
    uint8_t systemCode; /**< 3: V3.x */
    uint8_t sharing;    /**< 0 continuous, 1 carrier, 2 MCCH, 3 traffic */
    uint16_t la;
    uint16_t carrier;   /**< Main carrier number */
    uint8_t band;
    uint8_t offset;     /**< 0: 0, 1: +6.25k, 2: -6.25k, 3: +12.5k */
    uint8_t duplex;     /**< Duplex spacing code */
    bool reverse;       /**< Reverse operation */
    uint32_t dlHz;      /**< Main carrier downlink */
    uint32_t ulHz;      /**< Main carrier uplink, 0 if unknown */
    uint16_t hyperframe;
    uint16_t cck;
    uint16_t services; /**< TETRA_SERV_* */
    uint8_t load;      /**< 0 unknown, 1 low .. 3 high */
};

struct tetraNeighbourInfo {
    uint8_t cellId;
    uint32_t dlHz; /**< 0 if unknown */
    uint8_t load;  /**< 0 unknown, 1 low .. 3 high */
    bool synchronised;
    uint16_t mcc;  /**< 0xffff: same as the serving cell */
    uint16_t mnc;
    uint16_t la;
    long long last; /**< Tick when last announced */
};

/** Local network time, broken down */
struct tetraTime {
    uint16_t year;
    uint8_t month; /**< 1..12 */
    uint8_t day;   /**< 1..31 */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    int16_t offsetMin; /**< Local time minus UTC */
};

struct tetra_cell;
struct tetra_network;

/**
 * Forget the cell and its neighbours (not the time).
 */
void tetraCell_reset(void);

void tetraCell_onCell(const struct tetra_cell *c);
void tetraCell_onNetwork(const struct tetra_network *n);

void tetraCell_get(struct tetraCellInfo *out);

/**
 * Copy the neighbours announced, by downlink frequency.
 *
 * @return number of entries copied.
 */
int tetraCell_neighbours(struct tetraNeighbourInfo *out, int max);

/**
 * Current local network time.
 *
 * @return false if no network time was ever received.
 */
bool tetraCell_time(struct tetraTime *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_CELL_H */
