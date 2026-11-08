/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_HEARD_H
#define OPENRTX_TETRA_HEARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Groups and radios heard on a TETRA cell, as on the ESP32-P4 SDR: the
 * identities seen in clear signalling (call set-up, floor grants, SDS,
 * status, location reports, registrations and group attachments), most
 * recent first, kept until the mode is reset (a new frequency).
 */

#define TETRA_HEARD_RADIOS 32
#define TETRA_HEARD_GROUPS 32
#define TETRA_GROUP_RADIOS 8 /**< Distinct radios kept per group */
#define TETRA_HEARD_CALLS 16
#define TETRA_CALL_TALKERS 4 /**< Distinct talkers kept per call */

/** Last thing a radio was seen doing */
enum tetraRadioActivity {
    TETRA_RADIO_CALL = 0, /**< Set up a call */
    TETRA_RADIO_TALK,     /**< Granted the floor */
    TETRA_RADIO_SDS,      /**< Sent an SDS, a status or a location */
    TETRA_RADIO_REG,      /**< Registered or attached to groups */
};

struct tetraRadio {
    uint32_t issi;
    long long first;    /**< Tick when first heard */
    long long last;     /**< Tick when last heard */
    uint8_t activity;   /**< enum tetraRadioActivity, the last one */
    uint32_t lastGroup; /**< Last group it called or talked in, 0 none */
    uint16_t calls;     /**< Calls set up and floor grants */
    uint16_t sds;       /**< SDS, status and location reports sent */
    bool pos;           /**< Position known */
    int32_t lat;        /**< Last latitude, 1e-5 degrees */
    int32_t lon;        /**< Last longitude, 1e-5 degrees */
};

struct tetraGroup {
    uint32_t gssi;
    long long first;    /**< Tick when first heard */
    long long last;     /**< Tick when last heard */
    uint32_t lastRadio; /**< Last caller or talker, 0 unknown */
    uint16_t calls;     /**< Calls set up */
    uint16_t tx;        /**< Floor grants */
    uint8_t nradios;    /**< Entries in radios[] */
    uint32_t radios[TETRA_GROUP_RADIOS];
    bool encrypted;     /**< Last call encrypted */
};

struct tetraCallInfo {
    uint16_t id;      /**< Call identifier */
    uint32_t dst;     /**< Called group or radio, 0 unknown */
    bool group;
    uint32_t talker;  /**< Last talking party, 0 unknown */
    uint8_t ntalkers; /**< Entries in talkers[] */
    uint32_t talkers[TETRA_CALL_TALKERS];
    long long start;  /**< Tick of its first signalling */
    long long last;   /**< Tick of its last signalling */
    bool active;      /**< Not released yet */
    bool encrypted;
};

struct tetra_call;
struct tetra_mm;
struct tetraSds;

/**
 * Forget every group and radio.
 */
void tetraHeard_reset(void);

/**
 * Account a call control event.
 */
void tetraHeard_call(const struct tetra_call *c);

/**
 * Account a short data message from the SDS history.
 */
void tetraHeard_sds(const struct tetraSds *m);

/**
 * Account a mobility management event: registrations, group attachments.
 */
void tetraHeard_mm(const struct tetra_mm *mm);

/**
 * Copy the radios heard, most recent first.
 *
 * @return number of entries copied.
 */
int tetraHeard_radios(struct tetraRadio *out, int max);

/**
 * Copy the calls heard, most recent first.
 *
 * @return number of entries copied.
 */
int tetraHeard_calls(struct tetraCallInfo *out, int max);

/**
 * Copy the groups heard, most recent first.
 *
 * @return number of entries copied.
 */
int tetraHeard_groups(struct tetraGroup *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_HEARD_H */
