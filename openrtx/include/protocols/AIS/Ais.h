/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_AIS_H
#define OPENRTX_AIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AIS receiver (ITU-R M.1371): the FM discriminator output of one AIS
 * channel (161.975 or 162.025 MHz), 9600 bit/s GMSK, decoded in software.
 *
 *   discriminator, 48 kHz -> low-pass -> centre and amplitude from the
 *   highest and lowest level over the last 13 bits (any carrier offset)
 *   -> three slicers (hysteresis 0, 1/8 and 3/8 of the amplitude), each
 *   with its own bit clock pulled by the transitions -> NRZI -> HDLC:
 *   training bits and flag, bit de-stuffing, CRC-16 (X.25).
 *
 * Bytes come least significant bit first, fields are numbered from the
 * most significant bit of the first byte. Valid reports update a table of
 * AIS_MAX_SHIPS stations, merged by MMSI: positions (messages 1, 2, 3, 4,
 * 18, 19, 21, 27) and static data (5, 19, 21, 24).
 */

#define AIS_IN_RATE 48000
#define AIS_MAX_SHIPS 32

enum aisClass {
    AIS_CLASS_A = 0,
    AIS_CLASS_B,
    AIS_BASE,  /**< Base station */
    AIS_ATON,  /**< Aid to navigation */
    AIS_SAR,   /**< SAR aircraft */
    AIS_OTHER, /**< Only static data heard */
};

struct aisShip {
    uint32_t mmsi;
    long long tick;    /**< Tick of the last report */
    uint32_t seq;      /**< Update sequence, see ais_lastSeq() */
    uint16_t reports;  /**< Valid reports from this station */
    uint8_t cls;       /**< enum aisClass */
    uint8_t msgType;   /**< Last message type */
    bool posValid;     /**< lat and lon hold a position */
    int32_t lat, lon;  /**< 1/10000 minute, north and east positive */
    uint16_t sog;      /**< Speed over ground, 1/10 knot, 1023 unknown */
    uint16_t cog;      /**< Course over ground, 1/10 deg, 3600 unknown */
    uint16_t heading;  /**< True heading, deg, 511 unknown */
    uint8_t navStatus; /**< 0..15, 15 not defined */
    uint8_t shipType;  /**< 0 unknown */
    uint16_t length;   /**< Metres, 0 unknown */
    uint8_t beam;      /**< Metres, 0 unknown */
    uint8_t draught;   /**< 1/10 m, 0 unknown */
    char name[21];
    char callsign[8];
    char destination[21];
};

struct aisStats {
    uint32_t frames;    /**< Valid reports */
    uint32_t crcErrors; /**< Frames dropped on a bad CRC */
    uint32_t syncs;     /**< Training bits and flag found */
    uint32_t ships;     /**< Stations in the table */
    bool inFrame;       /**< A frame is being received */
};

/**
 * Forget the stations and restart the receiver.
 */
void ais_reset(void);

/**
 * Process discriminator audio at AIS_IN_RATE, any scale or polarity. Call
 * from one thread only.
 */
void ais_process(const int16_t *samples, size_t n);

/**
 * Copy the stations, most recently heard first.
 *
 * @return number of stations copied.
 */
int ais_get(struct aisShip *out, int max);

/**
 * Sequence number of the newest valid report, 0 if none yet.
 */
uint32_t ais_lastSeq(void);

void ais_getStats(struct aisStats *out);

/**
 * Short text for a navigational status, NULL if not defined.
 */
const char *ais_navStatusName(uint8_t status);

/**
 * Short text for a ship type, NULL if unknown.
 */
const char *ais_shipTypeName(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_AIS_H */
