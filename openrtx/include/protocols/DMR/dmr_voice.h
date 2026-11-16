/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_VOICE_H
#define DMR_VOICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR voice, phase 2: AMBE+2 (3600x2450) decoding with mbelib.
 *
 * mbelib is an OPTIONAL component, off by default: the AMBE+2 algorithm is
 * covered by patents in some countries, so the library is not shipped in this
 * repository. To enable voice on the C62, fetch it with
 * scripts/c62/fetch_mbelib.sh: the build then turns voice on (C62_DMR_VOICE).
 * Without it everything below compiles to no-ops and DMR stays metadata-only
 * (dmr_voice_available() returns false).
 *
 * Each voice burst (A..F) carries three 72-bit AMBE+2 frames: dibits 0-35,
 * 36-53 + 78-95 (split around the centre sync/EMB field) and 96-131. Every
 * frame is de-interleaved into mbelib's ambe_fr[4][24] with the DMR AMBE
 * interleave schedule from DSD (ISC licence, credited in dmr_voice.c).
 *
 * Only one slot is heard at a time: in AUTO the first slot to start talking
 * is followed until its call ends (terminator, or no voice for 500 ms); a
 * preference forces TS1 or TS2. Calls flagged as encrypted (privacy bit in
 * the LC service options) are not played.
 */

#define DMR_VOICE_RATE 8000
#define DMR_VOICE_PREF_AUTO (-1)

/* Receives decoded audio: 8 kHz float, nominally within +-1.0. */
typedef void (*dmr_voice_sink_t)(const float *pcm, int n);

bool dmr_voice_available(void);
void dmr_voice_set_sink(dmr_voice_sink_t sink);
void dmr_voice_reset(void);

/* From dmr_proto: every voice burst of a slot (voice_idx 0 = A ... 5 = F). */
void dmr_voice_burst(int slot, const uint8_t burst[132], int voice_idx,
                     uint32_t t_ms);
void dmr_voice_call_end(int slot);
void dmr_voice_set_encrypted(int slot, bool enc);

/* Slot preference: 0 = TS1, 1 = TS2, DMR_VOICE_PREF_AUTO. */
void dmr_voice_set_pref(int pref);
int dmr_voice_get_pref(void);
/* Slot currently being played, or -1. */
int dmr_voice_get_playing(void);

/* Vocoder frames decoded since boot and their average decode time (us) -
 * a 20 ms frame must take well under 20 ms, shared with everything else. */
void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us);

/* Frames attenuated by the burst concealment (corrupted-frame squawks). */
uint32_t dmr_voice_get_concealed(void);

/* Playback side. dmr_voice_burst() only queues the bursts of the slot being
 * heard; the vocoder runs in the caller of dmr_voice_decode_next(), which
 * decodes the oldest queued burst into three 20 ms frames handed to the sink.
 * Returns false if nothing was queued. */
bool dmr_voice_decode_next(void);
/* Voice bursts waiting for the vocoder (60 ms each). */
int dmr_voice_queued(void);
/* Longest vocoder frame so far (us), and bursts dropped because the queue
 * was full (the vocoder fell behind). */
void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops);

/**
 * Queue one 20 ms AMBE+2 frame given as its 49 data bits, FEC already
 * removed (System Fusion V/D mode 2). Played by the same speech thread as
 * DMR; frames are grouped three per queued burst.
 *
 * @param bits: 49 bits, one per byte, in mbelib's ambe_d order.
 * @param errs: bits corrected by the caller's FEC, for the vocoder's
 * repeat-or-mute decision.
 * @param t_ms: arrival time, ms; a gap of more than 500 ms starts a new
 * call with a fresh vocoder state.
 */
void dmr_voice_ambe49(const uint8_t bits[49], unsigned errs, uint32_t t_ms);

/**
 * Queue one 20 ms AMBE+2 frame in its 72-bit channel coding (NXDN): the
 * Golay (24,12) word a, the scrambled Golay (23,12) word b and the 25
 * unprotected bits c, packed MSB first in that order into 9 bytes. The
 * vocoder corrects and descrambles it as a DMR frame. Grouped three per
 * burst with the frames of dmr_voice_ambe49().
 *
 * @param abc: the 72 bits.
 * @param t_ms: arrival time, ms, as in dmr_voice_ambe49().
 */
void dmr_voice_ambe72(const uint8_t abc[9], uint32_t t_ms);

/**
 * Queue the frames held back for a partial burst: end of a transmission.
 */
void dmr_voice_ambe49_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOICE_H */
