/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The OpenDMR encoder is C++ and includes mbelib.h, whose functions it calls
 * to keep the decoder state the quantisation predicts from. The C62 links
 * the mbelib of the DMR voice, a C library whose header has no extern "C":
 * this one, first on the encoder's include path, adds it.
 */
#ifndef C62_OPENDMR_MBELIB_H
#define C62_OPENDMR_MBELIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next "mbelib.h"

#ifdef __cplusplus
}
#endif

#endif /* C62_OPENDMR_MBELIB_H */
