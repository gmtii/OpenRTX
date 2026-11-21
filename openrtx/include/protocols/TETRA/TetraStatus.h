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

    bool uplinkWanted;   /**< The UI asked for the uplink */
    bool uplink;         /**< Listening to the uplink of the cell */
    bool ulPresent;      /**< An uplink burst is being received */
    uint32_t ulHz;       /**< The uplink frequency, if uplink */
    uint16_t ulPdus;     /**< Uplink signalling and voice decoded */
    uint32_t ulSrc;      /**< Radio of the last uplink activity, 0 none */
    uint32_t ulDst;      /**< Party it called, 0 if none or unknown */
    bool ulGroup;        /**< ulDst is a group */
    uint8_t ulWhat;      /**< enum tetraUplinkWhat */
    long long ulTime;    /**< Tick of the last uplink activity */
};

/** Last activity heard on the uplink */
enum tetraUplinkWhat {
    TETRA_UL_NONE = 0,
    TETRA_UL_SETUP,     /**< Call set-up */
    TETRA_UL_TX_DEMAND, /**< Asks to talk */
    TETRA_UL_CALL,      /**< Other call control */
    TETRA_UL_SDS,       /**< SDS or status */
    TETRA_UL_VOICE,     /**< Speech */
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
 * A site discovery scan is over: the radio is back on the main carrier,
 * whose burst synchronisation starts again, and no call is followed.
 */
void tetraStatus_scanDone(void);

/**
 * Count a demodulator re-acquisition or restart. libtetra keeps its burst
 * synchronisation: it reports a real loss of lock itself.
 */
void tetraStatus_demodRestart(void);

/**
 * Update the demodulator figures.
 *
 * @param present: on the uplink, a burst is being received.
 */
void tetraStatus_setDemod(unsigned phaseErr, int carrierHz, bool present);

/**
 * @return the frequency correction applied to the receiver, Hz (TETRA
 * automatic frequency correction, CONFIG_TETRA_AFC); 0 if off or none.
 */
int32_t tetra_afcOffset(void);

/**
 * Ask for the uplink of the cell, or the downlink back. From the UI; the
 * TETRA mode does it, if a cell was heard. A new frequency gives the
 * downlink back.
 */
void tetraStatus_setUplink(bool on);

/**
 * @return true if the UI asks for the uplink. For the TETRA mode.
 */
bool tetraStatus_uplinkWanted(void);

/**
 * The TETRA mode is on the uplink, or back on the downlink. The uplink
 * stops any call following.
 *
 * @param on: true on the uplink.
 * @param hz: the uplink frequency.
 */
void tetraStatus_uplinkActive(bool on, uint32_t hz);

/**
 * @return true while locked to a TMO downlink.
 */
bool tetraStatus_sync(void);

/* Groups whose calls only are played */
#define TETRA_SELECTED_MAX 8

/**
 * Select a group, or take it back: with groups selected only their calls
 * are played and followed to another carrier, as the ESP32-P4 SDR's pin;
 * the other calls are still shown. None selected, every clear call is
 * played. A group selected is no longer excluded. Kept until changed,
 * across frequencies.
 *
 * @param gssi: the group.
 * @return true if the group is now selected; false if it was taken back,
 * or there is no room for another one.
 */
bool tetraStatus_toggleSelected(uint32_t gssi);

/**
 * @return true if the group is selected.
 */
bool tetraStatus_selected(uint32_t gssi);

/**
 * @return number of groups selected, 0 if every clear call is played.
 */
int tetraStatus_selectedCount(void);

/**
 * Copy the groups selected, for the UI to list them also when they are no
 * longer among the groups heard.
 *
 * @param out: destination, TETRA_SELECTED_MAX entries.
 * @return number of groups copied.
 */
int tetraStatus_selectedList(uint32_t *out);

/* Groups whose calls are never played */
#define TETRA_EXCLUDED_MAX 8

/**
 * Exclude a group, or take it back: its calls are still shown, but neither
 * played nor followed to another carrier, as those outside the groups
 * selected. A group excluded is no longer selected. Kept until changed,
 * across frequencies.
 *
 * @param gssi: the group.
 * @return true if the group is now excluded; false if it was taken back,
 * or there is no room for another one.
 */
bool tetraStatus_toggleExcluded(uint32_t gssi);

/**
 * @return true if the group is excluded.
 */
bool tetraStatus_excluded(uint32_t gssi);

/**
 * Play the calls whose set-up was not heard (under way when the radio
 * tuned in or synced) once their speech passes the CRC three full slots in
 * a row. Off by default: end-to-end encrypted speech passes the CRC too,
 * and would be played as noise. Turning it off drops the calls taken so.
 * Kept until changed, across frequencies.
 */
void tetraStatus_setLateCalls(bool on);

/**
 * @return true if the calls whose set-up was not heard are played.
 */
bool tetraStatus_lateCalls(void);

/**
 * Play the encrypted calls too, and those whose encryption is not known
 * (their set-up or connect not heard): end-to-end encrypted speech plays as
 * noise. Off by default; the call card still shows them as encrypted.
 * Kept until changed, across frequencies.
 */
void tetraStatus_setPlayEncrypted(bool on);

/**
 * @return true if the encrypted calls are played.
 */
bool tetraStatus_playEncrypted(void);

/**
 * @return number of groups excluded.
 */
int tetraStatus_excludedCount(void);

/**
 * Copy the groups excluded, as tetraStatus_selectedList().
 *
 * @param out: destination, TETRA_EXCLUDED_MAX entries.
 * @return number of groups copied.
 */
int tetraStatus_excludedList(uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_STATUS_H */
