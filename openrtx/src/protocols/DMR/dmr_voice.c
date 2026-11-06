/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_voice.h"
#include "dmr_port.h"

#include <string.h>

static volatile int s_pref = DMR_VOICE_PREF_AUTO;
static volatile int s_playing = -1;

void dmr_voice_set_pref(int pref)
{
    s_pref = (pref == 0 || pref == 1) ? pref : DMR_VOICE_PREF_AUTO;
    if (s_pref != DMR_VOICE_PREF_AUTO && s_playing != s_pref) {
        s_playing = -1; /* re-choose on the next voice burst */
    }
}

int dmr_voice_get_pref(void)
{
    return s_pref;
}

int dmr_voice_get_playing(void)
{
    return s_playing;
}

#ifndef DMR_VOICE_ENABLED
/* ---- mbelib not enabled: metadata-only build ------------------------------ */
void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us)
{
    *frames = 0;
    *avg_us = 0;
}
uint32_t dmr_voice_get_concealed(void)
{
    return 0;
}
bool dmr_voice_available(void)
{
    return false;
}
void dmr_voice_set_sink(dmr_voice_sink_t sink)
{
    (void)sink;
}
void dmr_voice_reset(void)
{
    s_playing = -1;
}
void dmr_voice_burst(int slot, const uint8_t burst[132], int voice_idx,
                     uint32_t t_ms)
{
    (void)slot;
    (void)burst;
    (void)voice_idx;
    (void)t_ms;
}
void dmr_voice_call_end(int slot)
{
    (void)slot;
}
void dmr_voice_set_encrypted(int slot, bool enc)
{
    (void)slot;
    (void)enc;
}
int dmr_voice_queued(void)
{
    return 0;
}
void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops)
{
    *max_us = 0;
    *queue_drops = 0;
}
bool dmr_voice_decode_next(void)
{
    return false;
}

#else
/* ---- mbelib voice ------------------------------------------------------------ */
#include "mbelib.h"
#include <math.h>

/*
 * DMR AMBE+2 interleave schedule, from DSD (Copyright (C) 2010 DSD Author,
 * ISC licence): dibit i of a 72-bit frame goes to ambe_fr[rW[i]][rX[i]] (its
 * high bit) and ambe_fr[rY[i]][rZ[i]] (low bit).
 */
static const uint8_t rW[36] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                                0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
                                0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2 };
static const uint8_t rX[36] = { 23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
                                17, 4,  16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
                                11, 9,  10, 8, 9,  7, 8,  6, 7,  5, 6,  4 };
static const uint8_t rY[36] = { 0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
                                1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
                                1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3 };
static const uint8_t rZ[36] = { 5,  3,  4,  2,  3,  1, 2,  0, 1,  13, 0,  12,
                                22, 11, 21, 10, 20, 9, 19, 8, 18, 7,  17, 6,
                                16, 5,  15, 4,  14, 3, 13, 2, 12, 1,  11, 0 };

#define VOICE_TIMEOUT_MS 500 /* no voice burst for this long ends the call */
#ifndef DMR_VOICE_UVQUALITY
#define DMR_VOICE_UVQUALITY \
    3 /* mbelib unvoiced synthesis quality (DSD default) */
#endif
/* mbelib float synthesis output -> ~+-1 full scale */
#define OUT_SCALE \
    (1.0f / 4096.0f)        /* host-measured: speech peaks ~0.95, rms ~0.09 */

/*
 * Burst concealment. On weak or fading signals mbelib sometimes synthesises a
 * frame whose parameters were corrupted beyond what its FEC reports: it still
 * counts a few corrected errors (errs2 1-3), but the output is a loud 20 ms
 * squawk, sometimes at full scale. So: a frame WITH corrected errors whose
 * rms jumps above CONCEAL_RATIO x the recent clean level is scaled down to
 * that level. Error-free frames (and errored frames of normal energy, usually
 * decoded correctly) are left untouched; only error-free frames update the
 * level.
 */
#define CONCEAL_RATIO 2.0f
#define CONCEAL_FLOOR 0.02f /* never treat near-silence as a reference */
#define LEVEL_ALPHA 0.1f    /* per 20 ms frame */

/*
 * Threads. The receiver (dmr_voice_burst(), from dmr_proto on the rtx
 * thread) picks the slot to play and queues its voice bursts; the playback
 * thread runs the vocoder (dmr_voice_decode_next()). The vocoder can take a
 * good part of a burst's 60 ms, and running it on the receive side would
 * make the demodulator lose samples.
 *
 * Receiver side: the call bookkeeping (enc, last_voice_ms).
 * Playback side: the vocoder state (mbe parameters, concealment level).
 */
typedef struct {
    bool enc;
    uint32_t last_voice_ms;
} vcall_t;

typedef struct {
    mbe_parms cur, prev, prev_enh;
    float level; /* running rms of recent error-free frames (concealment reference) */
} vdec_t;

/* One queued voice burst: its three AMBE frames, 36 dibits each */
typedef struct {
    uint8_t frames[3][36];
    uint8_t slot;
    bool new_call; /* reset the slot's vocoder state first */
} vburst_t;

#define QUEUE_BURSTS 12     /* 720 ms */

static vcall_t s_call[2];                /* receiver side */
static EXT_RAM_BSS_ATTR vdec_t s_dec[2]; /* playback side, 7 KB */
static vburst_t s_queue[QUEUE_BURSTS];
static int s_q_head, s_q_count;
static volatile bool s_dec_reset; /* reset all vocoder state on next pop */
DMR_LOCK_DECLARE(s_q_lock);

static volatile uint32_t s_frames;
static volatile uint64_t s_frame_us_total;
static volatile uint32_t s_frame_us_max;
static volatile uint32_t s_queue_drops;
static volatile uint32_t s_concealed;
static dmr_voice_sink_t s_sink;

/* Soft limiter: linear to 0.7, then a smooth knee toward 1.0 (no hard clip). */
static inline float soft_limit(float v)
{
    const float a = v < 0.0f ? -v : v;
    if (a <= 0.7f) {
        return v;
    }
    {
        const float over = (a - 0.7f) / 0.3f;
        const float y =
            0.7f + 0.3f * (over / (1.0f + over)); /* -> 1.0 asymptotically */
        return v < 0.0f ? -y : y;
    }
}

bool dmr_voice_available(void)
{
    return true;
}

uint32_t dmr_voice_get_concealed(void)
{
    return s_concealed;
}

void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us)
{
    uint32_t n = s_frames;
    *frames = n;
    *avg_us = n ? (uint32_t)(s_frame_us_total / n) : 0;
}

void dmr_voice_set_sink(dmr_voice_sink_t sink)
{
    s_sink = sink;
}

static void dec_init(int slot)
{
    memset(&s_dec[slot], 0, sizeof(s_dec[slot]));
    mbe_initMbeParms(&s_dec[slot].cur, &s_dec[slot].prev,
                     &s_dec[slot].prev_enh);
}

void dmr_voice_reset(void)
{
    memset(s_call, 0, sizeof(s_call));
    s_playing = -1;
    DMR_LOCK(s_q_lock);
    s_q_head = s_q_count = 0;
    s_dec_reset = true;
    DMR_UNLOCK(s_q_lock);
}

void dmr_voice_set_encrypted(int slot, bool enc)
{
    s_call[slot & 1].enc = enc;
}

void dmr_voice_call_end(int slot)
{
    if (s_playing == slot) {
        s_playing = -1;
    }
    s_call[slot & 1].enc = false;
}

int dmr_voice_queued(void)
{
    int n;
    DMR_LOCK(s_q_lock);
    n = s_q_count;
    DMR_UNLOCK(s_q_lock);
    return n;
}

static void decode_frame(int slot, const uint8_t *dib)
{
    char ambe_fr[4][24], ambe_d[49], err_str[64];
    float pcm[160];
    int errs = 0, errs2 = 0;
    vdec_t *v = &s_dec[slot];

    memset(ambe_fr, 0, sizeof(ambe_fr));
    for (int i = 0; i < 36; i++) {
        ambe_fr[rW[i]][rX[i]] = (char)((dib[i] >> 1) & 1u);
        ambe_fr[rY[i]][rZ[i]] = (char)(dib[i] & 1u);
    }
    err_str[0] = '\0';
    {
        uint32_t t0 = dmr_port_cycles();
        uint32_t us;
        mbe_processAmbe3600x2450Framef(pcm, &errs, &errs2, err_str, ambe_fr,
                                       ambe_d, &v->cur, &v->prev, &v->prev_enh,
                                       DMR_VOICE_UVQUALITY);
        us = dmr_port_cycles_to_us(dmr_port_cycles() - t0);
        s_frame_us_total += us;
        if (us > s_frame_us_max) {
            s_frame_us_max = us;
        }
        s_frames++;
    }
    {
        float e = 0.0f, rms;
        for (int k = 0; k < 160; k++) {
            pcm[k] *= OUT_SCALE;
            e += pcm[k] * pcm[k];
        }
        rms = sqrtf(e / 160.0f);
        if (errs2 > 0 && v->level > 0.0f
            && rms > CONCEAL_RATIO * v->level + CONCEAL_FLOOR) {
            const float g = (v->level + CONCEAL_FLOOR) / rms;
            for (int k = 0; k < 160; k++) {
                pcm[k] *= g;
            }
            s_concealed++;
        } else if (errs2 == 0) {
            v->level += LEVEL_ALPHA * (rms - v->level);
        }
        for (int k = 0; k < 160; k++) {
            pcm[k] = soft_limit(pcm[k]);
        }
    }
    if (s_sink != NULL) {
        s_sink(pcm, 160);
    }
}

void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops)
{
    *max_us = s_frame_us_max;
    *queue_drops = s_queue_drops;
}

bool dmr_voice_decode_next(void)
{
    vburst_t b;
    bool reset;

    DMR_LOCK(s_q_lock);
    reset = s_dec_reset;
    s_dec_reset = false;
    if (s_q_count == 0) {
        DMR_UNLOCK(s_q_lock);
        if (reset) {
            dec_init(0);
            dec_init(1);
        }
        return false;
    }
    b = s_queue[s_q_head];
    s_q_head = (s_q_head + 1) % QUEUE_BURSTS;
    s_q_count--;
    DMR_UNLOCK(s_q_lock);

    if (reset) {
        dec_init(0);
        dec_init(1);
    }
    if (b.new_call) {
        dec_init(b.slot); /* new call on this slot: fresh vocoder state */
    }
    for (int i = 0; i < 3; i++) {
        decode_frame(b.slot, b.frames[i]);
    }
    return true;
}

void dmr_voice_burst(int slot, const uint8_t burst[132], int voice_idx,
                     uint32_t t_ms)
{
    vcall_t *c = &s_call[slot & 1];
    bool new_call = false;
    vburst_t *b;
    (void)voice_idx;

    slot &= 1;
    /* drop a stalled playing slot */
    if (s_playing >= 0
        && (t_ms - s_call[s_playing].last_voice_ms) > VOICE_TIMEOUT_MS) {
        s_playing = -1;
    }
    if (c->last_voice_ms == 0 || (t_ms - c->last_voice_ms) > VOICE_TIMEOUT_MS) {
        new_call = true;
    }
    c->last_voice_ms = t_ms;

    if (s_playing < 0) {
        if (s_pref == DMR_VOICE_PREF_AUTO || s_pref == slot) {
            s_playing = slot;
        }
    }
    if (s_playing != slot || c->enc) {
        return; /* only the slot being listened to costs CPU */
    }

    DMR_LOCK(s_q_lock);
    if (s_q_count == QUEUE_BURSTS) {
        /* The vocoder is not keeping up: drop the oldest burst */
        s_q_head = (s_q_head + 1) % QUEUE_BURSTS;
        s_q_count--;
        s_queue_drops++;
    }
    b = &s_queue[(s_q_head + s_q_count) % QUEUE_BURSTS];
    memcpy(b->frames[0], &burst[0], 36);
    memcpy(b->frames[1], &burst[36], 18);
    memcpy(&b->frames[1][18], &burst[78], 18);
    memcpy(b->frames[2], &burst[96], 36);
    b->slot = (uint8_t)slot;
    b->new_call = new_call;
    s_q_count++;
    DMR_UNLOCK(s_q_lock);
}
#endif
