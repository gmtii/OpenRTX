/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_DEMOD_H
#define OPENRTX_TETRA_DEMOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Streaming TETRA pi/4-DQPSK demodulator for FM discriminator audio.
 *
 * The input is the 48 kHz output of an FM discriminator (the radio's flat
 * RX audio). Its integral over one symbol (integrate-and-dump) is the
 * symbol's phase change, which is what pi/4-DQPSK carries. Steps:
 *
 *  1. interpolate to 144 kHz (8 samples per symbol)
 *  2. integrate over one symbol (moving sum of 8 samples)
 *  3. gain and carrier offset: running least-squares fit to the decisions
 *  4. timing: running decision error at the 8 phases of a symbol, the loop
 *     steers to the minimum (acquires first, then tracks clock drift, and
 *     jumps when the signal's timing jumps, e.g. after lost samples)
 *  5. 9-tap symbol-spaced decision-directed equalizer
 *  6. soft bits for libtetra (int8, > 0 means '1'); spectrum polarity is
 *     detected from the synchronisation training sequence
 */

#define TETRA_DEMOD_IN_RATE 48000
#define TETRA_DEMOD_SPS 8
#define TETRA_DEMOD_HIST 64   /* 144 kHz samples kept, power of two */
#define TETRA_DEMOD_INTERP 12 /* FIR taps per polyphase branch */
#define TETRA_DEMOD_EQ_TAPS 9
/* Symbols kept for the symbol page of the UI */
#define TETRA_DEMOD_SCOPE 256

struct tetraDemod {
    /* Interpolator */
    float fir[3][TETRA_DEMOD_INTERP];
    float in[TETRA_DEMOD_INTERP];
    unsigned inPos;
    /* Integrated signal at 144 kHz */
    float sum[TETRA_DEMOD_SPS];
    unsigned sumPos;
    float hist[TETRA_DEMOD_HIST];
    uint32_t written; /* 144 kHz samples written to hist */
    uint32_t nextInt; /* Position of the next symbol, in 144 kHz samples: */
    float nextFrac;   /* integer and fractional parts */
    /* Gain and offset fit, phi = gain * (d - offset) */
    float gain;
    float offset;
    float sx, sxx, st, sxt; /* Running moments */
    /* Timing */
    float cost[TETRA_DEMOD_SPS];
    bool acquired;
    float drift; /* Total timing correction, samples */
    float rate;  /* Timing correction per symbol (clock offset) */
    /* Equalizer */
    float eqIn[TETRA_DEMOD_EQ_TAPS];
    float eq[TETRA_DEMOD_EQ_TAPS];
    float eqBias;
    float eqInit[TETRA_DEMOD_EQ_TAPS]; /* Restored when re-acquiring */
    /* Polarity */
    uint64_t srNorm;
    uint64_t srInv;
    uint32_t hitsNorm;
    uint32_t hitsInv;
    uint32_t polaritySymbols;
    bool inverted;
    /*
     * Rotation: the fit can settle a quarter turn per symbol off (+-4.5 kHz),
     * where every symbol still lands on a constellation point. The sync
     * sequence turned by +pi/2, pi and -pi/2, both polarities, tells it.
     */
    uint64_t srRot[3][2];
    uint32_t sinceSync; /* Symbols since the sync sequence came right */
    uint32_t rotations; /* Quarter-turn corrections made */
    /* Statistics */
    uint32_t symbols;
    float errPow;        /* Running mean square phase error, rad^2 */
    float errFast;       /* The same over a few ms: fades, glitches */
    uint32_t badSymbols; /* Consecutive symbols with a high phase error */
    uint32_t reacquired; /* Number of re-acquisitions */
    uint32_t slips;      /* Number of timing jumps followed */
    /* Last symbols, for the UI: integrated discriminator value, and the
     * phase change decided from it (NAN while warming up) */
    float scopeD[TETRA_DEMOD_SCOPE];
    float scopeY[TETRA_DEMOD_SCOPE];
    uint32_t scopePos;
};

/**
 * Initialise or reset the demodulator.
 */
void tetraDemod_init(struct tetraDemod *dm);

/**
 * Set the initial equalizer taps, measured on the radio's RX path, so the
 * adaptive equalizer starts converged. Call after tetraDemod_init().
 *
 * @param taps: TETRA_DEMOD_EQ_TAPS values, oldest symbol first.
 */
void tetraDemod_setEqualizer(struct tetraDemod *dm, const float *taps);

/**
 * Process 48 kHz discriminator samples.
 *
 * @param dm: demodulator.
 * @param in: input samples.
 * @param n: number of input samples.
 * @param soft: output soft bits, room for at least n * 3 / 4 + 4 values.
 * @return number of soft bits written (two per symbol).
 */
size_t tetraDemod_process(struct tetraDemod *dm, const int16_t *in, size_t n,
                          int8_t *soft);

/**
 * @return RMS phase error, in percent of pi/4 (lower is better: about 20%
 * on a clean signal, decoding fails above 50%).
 */
unsigned tetraDemod_phaseError(const struct tetraDemod *dm);

/**
 * @return carrier offset in Hz, given the discriminator gain.
 */
int tetraDemod_carrierOffset(const struct tetraDemod *dm);

/**
 * @return number of times the demodulator lost the signal and acquired it
 * again (the phase error stayed high), plus the timing jumps it followed.
 */
uint32_t tetraDemod_reacquisitions(const struct tetraDemod *dm);

/**
 * @return number of times the carrier offset was corrected by a quarter
 * turn per symbol (4.5 kHz steps) because the sync sequence came rotated.
 */
uint32_t tetraDemod_rotations(const struct tetraDemod *dm);

/**
 * A snapshot of the last symbols, for display: read from another thread,
 * a symbol or two may be torn, which does not matter on a plot.
 *
 * @param dm: demodulator.
 * @param d: integrated discriminator values, TETRA_DEMOD_SCOPE entries.
 * @param y: phase changes in radians (NAN if not decided yet), as many.
 * @param levels: the four values of d the fit expects for -3, -1, 1 and 3
 * times pi/4.
 */
void tetraDemod_scope(const struct tetraDemod *dm, float *d, float *y,
                      float *levels);

/**
 * @return the demodulator of the TETRA mode, for display.
 */
const struct tetraDemod *tetra_getDemod(void);

/**
 * @return sample clock offset relative to the transmitter, in ppm.
 */
int tetraDemod_clockOffset(const struct tetraDemod *dm);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_DEMOD_H */
