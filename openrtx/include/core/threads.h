/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef THREADS_H
#define THREADS_H

#include <stddef.h>

/**
 * Threads' stack sizes
 */
#define UI_THREAD_STKSIZE     2048
#if defined(PLATFORM_C62) && (defined(CONFIG_TETRA) || defined(CONFIG_DMR_RX))
/*
 * The TETRA and DMR receive modes decode in the rtx thread: libtetra needs
 * up to about 1.5 KB (Viterbi, MAC, SDS), the DMR decoder about 1 KB of
 * burst buffers on top of its call chain (frame, burst handlers, BPTC, RS,
 * LC), plus the event callbacks and newlib's snprintf: 4 KB left little
 * margin, the heap has plenty.
 */
#define RTX_THREAD_STKSIZE    6144
#elif defined(PLATFORM_C62)
#define RTX_THREAD_STKSIZE    1024
#else
#define RTX_THREAD_STKSIZE    512
#endif
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE  512

/**
 * Thread priority levels, UNIX-like: lower level, higher thread priority
 */
#ifdef _MIOSIX
#define THREAD_PRIO_RT      0
#define THREAD_PRIO_HIGH    1
#define THREAD_PRIO_NORMAL  2
#define THREAD_PRIO_LOW     3
#endif

/**
 * Spawn all the threads for the various functionalities.
 */
void create_threads();

#endif /* THREADS_H */
