/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_STATUS_H
#define OPENRTX_TETRA_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the TETRA receive mode has decoded, for the UI. Written by the rtx
 * thread, read with tetra_getStatus().
 */
struct tetraStatus {
    bool sync;           /**< Locked to a TMO downlink */
    bool cell;           /**< Fields below up to dlFreq are valid */
    uint16_t mcc;        /**< Mobile country code */
    uint16_t mnc;        /**< Mobile network code */
    uint8_t colourCode;  /**< Colour code */
    bool sysinfo;        /**< la and dlFreq are valid */
    uint16_t la;         /**< Location area */
    uint32_t dlFreq;     /**< Main carrier downlink frequency, Hz */
    bool airEncryption;  /**< Cell uses air interface encryption */

    bool call;           /**< A call is set up or traffic is flowing */
    bool callGroup;      /**< Group call */
    bool callEncrypted;  /**< Call or its traffic is encrypted */
    bool callUnknown;    /**< Traffic of no call set up in clear: not played */
    bool callFiltered;   /**< Traffic of a call outside the group filter */
    bool callOther;      /**< A second clear call, not played, meanwhile */
    bool voice;          /**< Traffic seen in the last second */
    uint16_t callId;     /**< Call identifier */
    uint32_t callDst;    /**< Called (group) SSI, 0 if unknown */
    uint32_t talker;     /**< Talking party SSI, 0 if unknown */

    uint32_t sdsSrc;     /**< Sender of the last SDS, 0 if none */
    char sds[64];        /**< Last SDS text or status, ASCII */
    long long sdsTime;   /**< Tick of the last SDS */

    uint16_t phaseErr;   /**< Demodulator RMS phase error, % of pi/4 */
    uint16_t overruns;   /**< Sample blocks lost: the radio was too slow */
    uint16_t resyncs;    /**< Burst sync and demodulator restarts */
    int16_t carrierHz;   /**< Carrier offset estimate */

    uint8_t knownCalls;  /**< Calls whose signalling was received */
    uint16_t lastCallId; /**< Last call signalled, 0 if none */
    uint8_t lastCallMarker;    /**< Its usage marker, 0 if unknown */
    uint8_t lastCallSlots;     /**< Its timeslots, bit 0 = TN1 */
    uint8_t lastTrafficMarker; /**< Usage marker of the last traffic slot */
    uint8_t lastTrafficTn;     /**< Timeslot of the last traffic slot */

    uint16_t followCarrier; /**< Carrier of the call followed, if followHz */
    uint32_t followHz;      /**< Frequency listened to there, 0 if none */
};

/**
 * Copy the current TETRA status.
 *
 * @param out: destination.
 */
void tetra_getStatus(struct tetraStatus *out);

/* For the TETRA mode */
struct tetra_event;

/**
 * Clear the status (new frequency, mode change).
 */
void tetraStatus_reset(void);

/**
 * libtetra event callback: updates the status. Pass it in the libtetra
 * receiver configuration.
 */
void tetraStatus_onEvent(void *user, const struct tetra_event *ev);

/**
 * Mark the burst synchronisation as lost, keeping the cell information.
 *
 * @param overrun: because samples were lost.
 */
void tetraStatus_lostSync(bool overrun);

/**
 * Frequency to listen to: that of a clear call set up on another carrier of
 * the cell, while it lasts, else the configured one. The carriers of a cell
 * share their timing and scrambling, so the demodulator and libtetra keep
 * going across the change: only the radio frequency moves.
 *
 * @param mainHz: configured receive frequency, the cell's main carrier.
 * @return frequency to tune the receiver to, in Hz.
 */
uint32_t tetraStatus_followHz(uint32_t mainHz);

/**
 * Count a demodulator re-acquisition or restart. libtetra keeps its burst
 * synchronisation: it reports a real loss of lock itself.
 */
void tetraStatus_demodRestart(void);

/**
 * Update the demodulator figures.
 */
void tetraStatus_setDemod(unsigned phaseErr, int carrierHz);

/**
 * @return true while locked to a TMO downlink.
 */
bool tetraStatus_sync(void);

/**
 * Play only the calls to one group, as the ESP32-P4 SDR's pin: the other
 * calls are still shown, but not played. Kept until changed, across
 * frequencies.
 *
 * @param gssi: the group, 0 to play every clear call again.
 */
void tetraStatus_setFilter(uint32_t gssi);

/**
 * @return the group filter, 0 if none.
 */
uint32_t tetraStatus_filter(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_STATUS_H */
