/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_SPEECH_H
#define OPENRTX_TETRA_SPEECH_H

#include <stdint.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tetra_traffic;

/** Call a traffic slot belongs to, as told by the signalling received */
enum tetraTrafficCall {
    TETRA_TRAFFIC_UNKNOWN = 0, /**< No call set up in clear on that slot */
    TETRA_TRAFFIC_CLEAR,       /**< Clear speech call */
    TETRA_TRAFFIC_ENCRYPTED,   /**< End-to-end or air encrypted call */
    TETRA_TRAFFIC_DATA,        /**< Circuit mode data call */
    TETRA_TRAFFIC_FILTERED,    /**< Clear speech outside the group filter */
};

/*
 * TETRA speech playback: libtetra's traffic slots -> speech frames (channel
 * decoding) -> OpenACELP -> 8 kHz audio on the speaker.
 *
 * A playback thread runs while the TETRA mode is enabled. It opens the
 * speaker path when speech arrives and releases it after one second of
 * silence.
 *
 * Built with CONFIG_TETRA_SPEECH (CMake option C62_TETRA_SPEECH); without it
 * these functions do nothing.
 */

/** Speech figures since the TETRA mode was enabled */
struct tetraSpeechStats {
    uint32_t slots;     /**< Traffic slots received */
    uint32_t encrypted; /**< Not played: encrypted call */
    uint32_t data;      /**< Not played: data call */
    uint32_t unknown;   /**< Not played: no call set up in clear */
    uint32_t filtered;  /**< Not played: outside the group filter */
    uint32_t queued;    /**< Speech frames queued for playback */
    uint32_t bad;       /**< Queued frames with errors (concealed) */
    uint32_t played;    /**< Frames played */
    uint32_t noPath;    /**< Speaker not available */
};

/**
 * Copy the speech figures.
 *
 * @param out: destination.
 */
void tetraSpeech_getStats(struct tetraSpeechStats *out);

/**
 * Start the playback thread.
 */
void tetraSpeech_start(void);

/**
 * Stop the playback thread and release the speaker.
 */
void tetraSpeech_stop(void);

/**
 * Check whether speech is being played, until a second after its end:
 * only clear calls are played. Wakes the screen like an open squelch.
 */
bool tetraSpeech_playing(void);

/**
 * Queue the speech frames of a traffic slot. Call from the libtetra event
 * callback. Only clear speech calls are played: the slots of encrypted,
 * data and unknown calls are ignored.
 *
 * @param t: traffic slot.
 * @param call: call the slot belongs to.
 */
void tetraSpeech_traffic(const struct tetra_traffic *t,
                         enum tetraTrafficCall call);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_SPEECH_H */
