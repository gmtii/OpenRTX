/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/DmrSpeech.h"

#ifdef DMR_VOICE_ENABLED

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define STATS_PRINT printk
#else
#define STATS_PRINT printf
#endif
#include <string.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/delays.h"
#include "dmr_voice.h"

#define FRAME_SAMPLES 160 /* 20 ms at 8 kHz */
#define BURST_FRAMES 3    /* One voice burst carries 60 ms of speech */
#define BURST_SAMPLES (BURST_FRAMES * FRAME_SAMPLES)
/* Speech arrives in 60 ms bursts: wait for three before playing */
#define PREBUFFER_BURSTS 3
#define SILENCE_BURSTS 17 /* About one second */
/* mbelib's synthesis keeps a few KB of arrays on the stack */
#define THREAD_STACK 8192
#define STATS_MS 10000

/*
 * The audio driver plays the circular buffer continuously, like a DMA: a
 * half not refilled in time is played again. Each half holds a whole burst,
 * decoded straight into it, so the vocoder has the 60 ms of the other half
 * to decode three frames.
 */
static stream_sample_t pcm[2 * BURST_SAMPLES];
static stream_sample_t *target; /* Half being filled by the vocoder sink */
static unsigned targetFrames;   /* Frames written to it */

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;
static volatile bool playing; /* Speech on the speaker */

static uint32_t lateBursts;   /* Halves refilled after they started playing */
static long long nextStats;

/**
 * Vocoder sink: one 20 ms frame, +-1.0 float, to 16-bit samples.
 */
static void sink(const float *in, int n)
{
    if (target == NULL || targetFrames >= BURST_FRAMES || n != FRAME_SAMPLES)
        return;

    stream_sample_t *out = target + targetFrames++ * FRAME_SAMPLES;
    for (int i = 0; i < n; i++) {
        float v = in[i] * 32767.0f;
        if (v > 32767.0f)
            v = 32767.0f;
        else if (v < -32768.0f)
            v = -32768.0f;
        out[i] = (stream_sample_t)v;
    }
}

/**
 * Decode the next queued burst into a buffer half.
 *
 * @return false if no speech is queued (the half is then silent).
 */
static bool fillBurst(stream_sample_t *out)
{
    target = out;
    targetFrames = 0;
    bool ok = dmr_voice_decode_next();
    target = NULL;

    if (!ok)
        targetFrames = 0;
    if (targetFrames < BURST_FRAMES)
        memset(out + targetFrames * FRAME_SAMPLES, 0,
               (BURST_FRAMES - targetFrames) * FRAME_SAMPLES * sizeof(*out));
    return ok;
}

/**
 * Vocoder and playback figures on the console, every STATS_MS.
 */
static void printStats(void)
{
    long long now = getTick();
    if (now < nextStats)
        return;
    nextStats = now + STATS_MS;

    uint32_t frames, avgUs, maxUs, drops;
    dmr_voice_get_timing(&frames, &avgUs);
    dmr_voice_get_debug(&maxUs, &drops);
    STATS_PRINT("DMR voice: %lu frames, %lu us avg, %lu us max (budget 20000), "
                "late %lu, queue drops %lu, concealed %lu\n",
                (unsigned long)frames, (unsigned long)avgUs,
                (unsigned long)maxUs, (unsigned long)lateBursts,
                (unsigned long)drops, (unsigned long)dmr_voice_get_concealed());
}

/**
 * Play queued speech until a second of silence, then release the speaker.
 */
static void playBurst(void)
{
    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    /* Both halves start with speech: the first two bursts */
    fillBurst(pcm);
    fillBurst(pcm + BURST_SAMPLES);
    streamId stream = audioStream_start(path, pcm, 2 * BURST_SAMPLES, 8000,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    outputStream_sync(stream, false);
    playing = true;

    unsigned silence = 0;
    while (!stopRequest && silence < SILENCE_BURSTS
           && audioPath_getStatus(path) == PATH_OPEN) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        long long t0 = getTick();
        if (fillBurst(out))
            silence = 0;
        else
            silence++;
        if (getTick() - t0 >= 60)
            lateBursts++;

        if (!outputStream_sync(stream, true))
            break;
        printStats();
    }

    playing = false;
    audioStream_stop(stream);
    audioPath_release(path);
}

bool dmrSpeech_playing(void)
{
    return playing;
}

static void *playback(void *arg)
{
    (void)arg;
    dmr_voice_set_sink(sink);
    while (!stopRequest) {
        if (dmr_voice_queued() >= PREBUFFER_BURSTS)
            playBurst();
        else
            sleepFor(0, 10);
        printStats();
    }
    dmr_voice_set_sink(NULL);
    return NULL;
}

void dmrSpeech_start(void)
{
    if (running)
        return;

    stopRequest = false;

    pthread_attr_init(&attr);
#ifdef __ZEPHYR__
    void *stack = malloc(THREAD_STACK);
    if (stack == NULL)
        return;
    pthread_attr_setstack(&attr, stack, THREAD_STACK);
#else
    pthread_attr_setstacksize(&attr, THREAD_STACK);
#endif
    running = pthread_create(&thread, &attr, playback, NULL) == 0;
#ifdef __ZEPHYR__
    if (!running)
        free(stack);
#endif
}

void dmrSpeech_stop(void)
{
    if (!running)
        return;

    stopRequest = true;
    pthread_join(thread, NULL);
    running = false;
#ifdef __ZEPHYR__
    void *stack;
    size_t size;
    pthread_attr_getstack(&attr, &stack, &size);
    free(stack);
#endif
}

#else  /* DMR_VOICE_ENABLED */

bool dmrSpeech_playing(void)
{
    return false;
}

void dmrSpeech_start(void)
{
}

void dmrSpeech_stop(void)
{
}

#endif /* DMR_VOICE_ENABLED */
