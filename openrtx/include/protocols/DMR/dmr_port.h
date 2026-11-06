/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_PORT_H
#define DMR_PORT_H

/*
 * Platform shim of the DMR receiver, so the same demodulator and decoder
 * build in OpenRTX and in a plain host test (gcc, no OS). All DSP and
 * protocol work runs in one thread (the rtx thread in OpenRTX); only the
 * snapshots the UI reads cross thread boundaries, guarded by
 * DMR_LOCK()/DMR_UNLOCK().
 */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#ifdef __ZEPHYR__
#include <pthread.h>

#define DMR_LOCK_DECLARE(name) \
    static pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define DMR_LOCK(name) pthread_mutex_lock(&(name))
#define DMR_UNLOCK(name) pthread_mutex_unlock(&(name))
#else
#define DMR_LOCK_DECLARE(name) static int name
#define DMR_LOCK(name) ((void)(name))
#define DMR_UNLOCK(name) ((void)(name))
#endif

/*
 * Large, rarely touched state (call log, vocoder state). On the C62 it goes
 * to PSRAM. The section is NOLOAD, not zeroed at boot: everything placed
 * there must be cleared by its reset function before use.
 */
#ifdef PLATFORM_C62
#define EXT_RAM_BSS_ATTR __attribute__((section(".psram_section")))
#else
#define EXT_RAM_BSS_ATTR
#endif

/* Large buffers (the demodulator's sample ring) */
static inline void *dmr_port_calloc_large(size_t n, size_t sz)
{
    return calloc(n, sz);
}

/*
 * Short intervals, for the vocoder timing figures: a free running counter
 * (wraps, so only differences of nearby readings mean anything) and the
 * conversion of a difference to microseconds.
 */
#ifdef __ZEPHYR__
#include <zephyr/kernel.h>

static inline uint32_t dmr_port_cycles(void)
{
    return k_cycle_get_32();
}

static inline uint32_t dmr_port_cycles_to_us(uint32_t cycles)
{
    return k_cyc_to_us_floor32(cycles);
}
#else
static inline uint32_t dmr_port_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
}

static inline uint32_t dmr_port_cycles_to_us(uint32_t cycles)
{
    return cycles;
}
#endif

#endif /* DMR_PORT_H */
