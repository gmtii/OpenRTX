/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * imbe_vocoder of OpenDMR without the heap: its analysis state (about 4 KB)
 * in static SRAM instead of new, which on the C62 lands in the slow PSRAM.
 * Replaces OpenDMR's imbe_vocoder.cc; there is only one encoder.
 */
#include <new>
#include "imbe_vocoder_impl.h"
#include "imbe_vocoder.h"

alignas(8) static unsigned char implStorage[sizeof(imbe_vocoder_impl)];

imbe_vocoder::imbe_vocoder()
{
    Impl = new (implStorage) imbe_vocoder_impl();
}

imbe_vocoder::~imbe_vocoder()
{
    Impl->~imbe_vocoder_impl();
}

void imbe_vocoder::imbe_encode(int16_t *frame_vector, int16_t *snd)
{
    Impl->imbe_encode(frame_vector, snd);
}

void imbe_vocoder::encode_4400(int16_t *snd, uint8_t *imbe)
{
    Impl->encode_4400(snd, imbe);
}

const IMBE_PARAM *imbe_vocoder::param(void)
{
    return Impl->param();
}
