/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Fast cosf() and rand() for mbelib, force-included (-include) into the mbelib sources
 * only (see CMakeLists.txt) - mbelib itself is not modified.
 *
 * Why: with uvquality 3 mbelib's speech synthesis calls cosf() ~21000 times
 * per 20 ms frame (~1 M calls/s). On the ESP32-P4 newlib's cosf is a software
 * routine of roughly 100-200 cycles, i.e. 30-55 % of a core for cosines
 * alone - enough to starve the DMR task and make the voice choppy.
 *
 * This version: Cody-Waite reduction to [-pi, pi], fold to [0, pi/2] by symmetry,
 * then a Taylor polynomial to x^12 (remainder < 1e-8 on that interval).
 * About 15 FPU operations. Host test: decoded speech vs. libm cosf -> see
 * test/host_dmr (identical AMBE frames, audio difference far below audibility).
 */
#ifndef MBE_FASTMATH_H
#define MBE_FASTMATH_H

/*
 * The Zephyr build compiles libraries as strict C99 (-std=c99,
 * -D_ANSI_SOURCE), which hides the POSIX constants mbelib uses, and adds -Os
 * after the library's own options: set both here, ahead of mbelib's code.
 */
#pragma GCC optimize("O2")

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

static inline float mbe_fast_cosf(float x)
{
    float k = x * 0.15915494309f; /* 1 / (2 pi) */
    float s = 1.0f, x2;
    k = (float)(int)(k + (k >= 0.0f ? 0.5f : -0.5f));
    /* Cody-Waite reduction: 2*pi = C1 + C2 with C1 = 6.28125 exact in 8 bits,
     * so k*C1 is exact for |k| < 65536 and only the tiny C2 term rounds. */
    x -= k * 6.28125f;
    x -= k * 1.9353071795864769e-3f; /* [-pi, pi] */
    if (x < 0.0f) {
        x = -x;                      /* cos is even */
    }
    if (x > 1.57079632679f) {
        x = 3.14159265359f - x; /* cos(x) = -cos(pi - x) */
        s = -1.0f;
    }
    x2 = x * x;
    /* Taylor series to x^12, evaluated by Horner's rule */
    float p = 2.08767570e-9f;
    p = p * x2 - 2.75573192e-7f;
    p = p * x2 + 2.48015873e-5f;
    p = p * x2 - 1.38888889e-3f;
    p = p * x2 + 4.16666667e-2f;
    p = p * x2 - 0.5f;
    p = p * x2 + 1.0f;
    return s * p;
}

#define cosf mbe_fast_cosf

/*
 * Fast rand() for mbelib's unvoiced synthesis (mbe_rand(), up to ~15000 calls
 * per frame with uvquality 3). newlib's rand() goes through __getreent(), which
 * masks interrupts to read the current task: on the P4 that made the vocoder
 * hog its core under the Pluto (task watchdog, IQ skips). The noise only needs
 * to sound random: xorshift32, same 0..RAND_MAX range. One state shared by the
 * single DMR task; a race would only perturb the noise.
 */
#include <stdlib.h> /* declares the real rand() before the macro below */
#include <stdint.h>

static inline int mbe_fast_rand(void)
{
    static uint32_t st = 0x9e3779b9u;
    uint32_t x = st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    st = x;
    return (int)(x & RAND_MAX);
}

#define rand mbe_fast_rand

#endif /* MBE_FASTMATH_H */
