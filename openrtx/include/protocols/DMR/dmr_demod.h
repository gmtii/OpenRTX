/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_DEMOD_H
#define DMR_DEMOD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR 4FSK demodulator and burst framer (4800 baud, 10 samples/symbol at
 * 48 kHz). Output: one dmr_frame_t per 30 ms (repeater / direct mode) or
 * 60 ms (MS simplex) burst, handed to dmr_proto_frame().
 *
 *   IQ front end (on the radio):  48 kS/s IQ -> fs/4 rotation (the channel
 *     sits 12 kHz off centre, see sdr.h FREQ_CONV_OFFSET) -> 41-tap channel
 *     filter (7 kHz) -> FM discriminator in Hz.
 *   Core (also fed directly by the host test with real discriminator
 *     captures, any scale):
 *     - RRC matched filter (alpha 0.2, +-4 symbols)
 *     - frame sync: normalized correlation (Pearson) of 24 symbol-spaced
 *       samples against the 8 sync patterns, both polarities
 *     - level/offset from the sync symbols (least-squares fit x = dc + A*s),
 *       so the 4-level slicer needs no AGC and ignores tuning offset
 *     - once locked, bursts are cut at fixed 30/60 ms spacing; each burst's
 *       sync field is re-searched +-4 samples around where it is expected to
 *       re-centre timing and refresh levels.
 * Lock is dropped after a run of bursts in which no FEC-protected field
 * decodes (dmr_proto_frame() returns false).
 */

typedef struct {
    bool locked;
    bool bs;       /* locked onto a BS sourced (repeater) stream */
    bool ms;       /* MS sourced (simplex / hotspot uplink) */
    bool inverted; /* symbols arrive with inverted polarity */
    float level; /* outer-symbol deviation (Hz with the IQ front end; ~1944 nominal) */
    float dc; /* RF tuning error (Hz with the IQ front end), sign-corrected for polarity */
    uint32_t syncs;  /* sync words found */
    uint32_t now_ms; /* stream time (sample clock) */
    uint32_t
        overruns; /* sample blocks lost by the caller (dmr_demod_note_overrun) */
} dmr_demod_status_t;

/* Allocates the sample ring (PSRAM on the P4) and designs the filters.
 * Returns false on allocation failure. */
bool dmr_demod_init(void);
void dmr_demod_reset(void);

/* fs/4 rotation of the IQ front end. true (default): the channel sits 12 kHz
 * off centre (sdr.h IF_MODE_OFFSET) and is rotated to 0 Hz first. false: the
 * channel is already centred (IF_MODE_ZERO / IF_MODE_LOWIF). sdr.c sets it on
 * every block, before feeding. */
void dmr_demod_set_fs4_rotation(bool on);

/* 48 kS/s IQ exactly as sdr.c's NFM branch receives it. */
void dmr_demod_feed_iq(const float *i, const float *q, int n);

/* 48 kS/s discriminator samples, any scale/offset (host tests). */
void dmr_demod_feed_disc(const float *x, int n);

void dmr_demod_get_status(dmr_demod_status_t *out);
/* The caller lost a block of samples (it could not keep up): counted for
 * the status. The framer finds the bursts again by itself. */
void dmr_demod_note_overrun(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_DEMOD_H */
