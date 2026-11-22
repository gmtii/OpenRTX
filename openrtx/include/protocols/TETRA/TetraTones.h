/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_TONES_H
#define OPENRTX_TETRA_TONES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tones of the TETRA calls, played on the speaker by the speech playback
 * (tetraSpeech_tone()). The sounds, 8 kHz mono, are made from WAV files by
 * scripts/c62/tetra_tones.py, into TetraTones.c.
 */
enum tetraTone {
    TETRA_TONE_SETUP = 0, /**< A call set up */
    TETRA_TONE_RELEASED,  /**< The call released */
    TETRA_TONE_PRIORITY,  /**< A priority or emergency call set up */
    TETRA_TONE_ROAMING,   /**< The roaming tuned another cell */
    TETRA_TONE_NUM
};

struct tetraToneSound {
    const int16_t *samples; /**< 8 kHz */
    size_t len;
};

extern const struct tetraToneSound tetraToneSounds[TETRA_TONE_NUM];

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_TONES_H */
