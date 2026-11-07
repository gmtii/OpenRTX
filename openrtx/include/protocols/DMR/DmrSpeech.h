/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_SPEECH_H
#define OPENRTX_DMR_SPEECH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR speech playback for the DMR receive mode. The receiver queues the
 * voice bursts of the slot being heard (dmr_voice.c); a playback thread
 * decodes them with mbelib (AMBE+2) and plays them on the speaker, taking
 * the speaker path when speech arrives and releasing it after one second of
 * silence.
 *
 * Built with DMR_VOICE_ENABLED (CMake option C62_DMR_VOICE); without it
 * these functions do nothing.
 */

/**
 * Start the playback thread.
 */
void dmrSpeech_start(void);

/**
 * Stop the playback thread and release the speaker.
 */
void dmrSpeech_stop(void);

/**
 * Check whether speech is being played, until a second after its end:
 * only clear calls are played. Wakes the screen like an open squelch.
 */
bool dmrSpeech_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_SPEECH_H */
