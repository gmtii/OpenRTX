/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/TETRA/TetraSpeech.h"

#ifdef CONFIG_TETRA_SPEECH

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <openacelp.h>
#include <tetra/tetra.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/delays.h"
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define STATS_PRINT printk
#else
#include <stdio.h>
#define STATS_PRINT printf
#endif

#define QUEUE_FRAMES 16    /* 480 ms */
#define PREBUFFER_FRAMES 5 /* 150 ms: frame 18 leaves a 113 ms gap */
#define SILENCE_FRAMES 34  /* About one second of 30 ms frames */
#define THREAD_STACK 4096
#define STATS_MS 10000

struct frame {
    uint8_t bits[OA_FRAME_BITS];
    uint8_t bfi;
};

/* Queue from the rtx thread (libtetra callback) to the playback thread */
static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static struct frame queue[QUEUE_FRAMES];
static unsigned queueHead;
static unsigned queueCount;

static struct tetra_speech_ws channelWs; /* rtx thread only */
static oa_dec_t decoder;                 /* Playback thread only */
static stream_sample_t pcm[2 * OA_FRAME_SAMPLES];

static pthread_t thread;
static pthread_attr_t attr;
static volatile bool running;
static volatile bool stopRequest;

/*
 * Console figures, every STATS_MS while traffic arrives: traffic slots, the
 * ones not played and why, speech frames queued (bad ones among them),
 * frames played and speaker openings that failed.
 */
static volatile uint32_t statSlots, statEnc, statData, statUnknown;
static volatile uint32_t statQueued, statBad, statPlayed, statNoPath;
static uint32_t statLastSlots;
static long long statNext;

void tetraSpeech_getStats(struct tetraSpeechStats *out)
{
    out->slots = statSlots;
    out->encrypted = statEnc;
    out->data = statData;
    out->unknown = statUnknown;
    out->queued = statQueued;
    out->bad = statBad;
    out->played = statPlayed;
    out->noPath = statNoPath;
}

static void printStats(void)
{
    long long now = getTick();
    if (now < statNext)
        return;
    statNext = now + STATS_MS;
    if (statSlots == statLastSlots)
        return;
    statLastSlots = statSlots;

    STATS_PRINT("TETRA speech: %lu slots (encrypted %lu, data %lu, "
                "unknown call %lu), %lu frames queued (bad %lu), "
                "%lu played, speaker busy %lu\n",
                (unsigned long)statSlots, (unsigned long)statEnc,
                (unsigned long)statData, (unsigned long)statUnknown,
                (unsigned long)statQueued, (unsigned long)statBad,
                (unsigned long)statPlayed, (unsigned long)statNoPath);
}

static unsigned queued(void)
{
    pthread_mutex_lock(&queueMutex);
    unsigned n = queueCount;
    pthread_mutex_unlock(&queueMutex);
    return n;
}

static bool pop(struct frame *f)
{
    bool ok = false;
    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0) {
        *f = queue[queueHead];
        queueHead = (queueHead + 1) % QUEUE_FRAMES;
        queueCount--;
        ok = true;
    }
    pthread_mutex_unlock(&queueMutex);
    return ok;
}

/**
 * Play queued speech until a second of silence, then release the speaker.
 */
static void playBurst(void)
{
    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        statNoPath++;
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    memset(pcm, 0, sizeof(pcm));
    streamId stream = audioStream_start(path, pcm, 2 * OA_FRAME_SAMPLES, 8000,
                                        STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (stream < 0) {
        audioPath_release(path);
        sleepFor(0, 100);
        return;
    }

    oa_dec_init(&decoder, &oa_tables_tetra);
    outputStream_sync(stream, false);

    unsigned silence = 0;
    while (!stopRequest && silence < SILENCE_FRAMES
           && audioPath_getStatus(path) == PATH_OPEN) {
        stream_sample_t *out = outputStream_getIdleBuffer(stream);
        if (out == NULL)
            break;

        struct frame f;
        if (pop(&f)) {
            oa_dec_frame(&decoder, f.bits, f.bfi, out);
            statPlayed++;
            silence = 0;
        } else {
            memset(out, 0, OA_FRAME_SAMPLES * sizeof(*out));
            silence++;
        }
        outputStream_sync(stream, true);
    }

    audioStream_stop(stream);
    audioPath_release(path);
}

static void *playback(void *arg)
{
    (void)arg;
    while (!stopRequest) {
        if (queued() >= PREBUFFER_FRAMES)
            playBurst();
        else
            sleepFor(0, 10);
        printStats();
    }
    return NULL;
}

void tetraSpeech_start(void)
{
    if (running)
        return;

    pthread_mutex_lock(&queueMutex);
    queueHead = queueCount = 0;
    pthread_mutex_unlock(&queueMutex);
    stopRequest = false;
    statSlots = statEnc = statData = statUnknown = 0;
    statQueued = statBad = statPlayed = statNoPath = 0;
    statLastSlots = 0;

    pthread_attr_init(&attr);
#ifdef __ZEPHYR__
    void *stack = malloc(THREAD_STACK);
    if (stack == NULL) {
        STATS_PRINT("TETRA speech: no memory for the playback thread\n");
        return;
    }
    pthread_attr_setstack(&attr, stack, THREAD_STACK);
#else
    pthread_attr_setstacksize(&attr, THREAD_STACK);
#endif
    running = pthread_create(&thread, &attr, playback, NULL) == 0;
    if (!running)
        STATS_PRINT("TETRA speech: playback thread not started\n");
#ifdef __ZEPHYR__
    if (!running)
        free(stack);
#endif
}

void tetraSpeech_stop(void)
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

void tetraSpeech_traffic(const struct tetra_traffic *t,
                         enum tetraTrafficCall call)
{
    if (!running)
        return;

    statSlots++;
    switch (call) {
        case TETRA_TRAFFIC_CLEAR:
            break;
        case TETRA_TRAFFIC_ENCRYPTED:
            statEnc++;
            return;
        case TETRA_TRAFFIC_DATA:
            statData++;
            return;
        default:
            statUnknown++;
            return;
    }

    struct tetra_speech sp;
    tetra_speech_decode(&channelWs, t, &sp);

    pthread_mutex_lock(&queueMutex);
    for (int i = 0; i < 2; i++) {
        /* On overflow drop the oldest frame */
        if (queueCount == QUEUE_FRAMES) {
            queueHead = (queueHead + 1) % QUEUE_FRAMES;
            queueCount--;
        }
        struct frame *f = &queue[(queueHead + queueCount) % QUEUE_FRAMES];
        memcpy(f->bits, sp.bits[i], OA_FRAME_BITS);
        f->bfi = sp.bfi[i];
        queueCount++;
        statQueued++;
        if (sp.bfi[i])
            statBad++;
    }
    pthread_mutex_unlock(&queueMutex);
}

#else /* CONFIG_TETRA_SPEECH */

#include <string.h>

void tetraSpeech_start(void)
{
}

void tetraSpeech_stop(void)
{
}

void tetraSpeech_getStats(struct tetraSpeechStats *out)
{
    memset(out, 0, sizeof(*out));
}

void tetraSpeech_traffic(const struct tetra_traffic *t,
                         enum tetraTrafficCall call)
{
    (void)t;
    (void)call;
}

#endif /* CONFIG_TETRA_SPEECH */
