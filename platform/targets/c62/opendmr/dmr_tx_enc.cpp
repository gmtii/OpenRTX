/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Microphone to AMBE+2 for DMR transmission (see dmr_tx_enc.h).
 *
 * The encoder takes about 6 ms per 20 ms frame (11.7 ms at worst, timed on
 * the C62), so a burst of three takes up to 35 ms of the 60 it lasts: too
 * long for the rtx thread, which must feed the transmitter every 30 ms. It
 * runs here, with the 8 KB of stack its analysis needs, and hands the
 * frames over in a queue.
 */
#include <new>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "hwconfig.h"
#include "mbeenc.h"
#include "dmr_tx_enc.h"

#define FRAME_SAMPLES 160
#define BURST_SAMPLES (3 * FRAME_SAMPLES) /* 60 ms */

struct burstFrames {
    uint8_t f[3][9];
};

K_MSGQ_DEFINE(encQueue, sizeof(struct burstFrames), 4, 4);
K_SEM_DEFINE(encRun, 0, 1);
K_SEM_DEFINE(encDone, 0, 1);

alignas(8) static unsigned char encStorage[sizeof(MBEEncoder)];
static MBEEncoder *encoder;
static int16_t micBuf[2 * BURST_SAMPLES];
static pathId micPath = -1;
static streamId micStream = -1;
static volatile bool running;
static uint32_t statBursts, statMaxUs, statDropped;

static void encThread(void *, void *, void *)
{
    for (;;) {
        k_sem_take(&encRun, K_FOREVER);
        /* DC block and the microphone gain of M17 (audio_codec.c) */
        float dcIn = 0.0f, dcOut = 0.0f;
        while (running) {
            dataBlock_t block = inputStream_getData(micStream);
            if (block.data == NULL || block.len < BURST_SAMPLES) {
                if (!running)
                    break;
                k_msleep(5);
                continue;
            }
            int16_t pcm[BURST_SAMPLES];
            for (size_t i = 0; i < BURST_SAMPLES; i++) {
                const float x = (float)block.data[i];
                dcOut = x - dcIn + 0.996f * dcOut;
                dcIn = x;
                float y = dcOut * CONFIG_MIC_GAIN;
                if (y > 32767.0f)
                    y = 32767.0f;
                if (y < -32768.0f)
                    y = -32768.0f;
                pcm[i] = (int16_t)y;
            }
            struct burstFrames b;
            const uint32_t t0 = k_cycle_get_32();
            for (int k = 0; k < 3; k++)
                encoder->encode_dmr_frame(&pcm[k * FRAME_SAMPLES], b.f[k]);
            const uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
            if (us > statMaxUs)
                statMaxUs = us;
            statBursts++;
            /* The transmitter is behind: the newest audio wins */
            while (k_msgq_put(&encQueue, &b, K_NO_WAIT) != 0) {
                struct burstFrames old;
                k_msgq_get(&encQueue, &old, K_NO_WAIT);
                statDropped++;
            }
        }
        k_sem_give(&encDone);
    }
}

/* Above the UI, like the speech threads; it takes a third of the CPU */
K_THREAD_DEFINE(dmr_tx_enc, 8192, encThread, NULL, NULL, NULL, 13, K_FP_REGS,
                0);

bool dmrTxEnc_start(void)
{
    if (running)
        return true;
    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0)
        return false;
    micStream = audioStream_start(micPath, micBuf, 2 * BURST_SAMPLES, 8000,
                                  STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (micStream < 0) {
        audioPath_release(micPath);
        micPath = -1;
        return false;
    }
    k_msgq_purge(&encQueue);
    if (encoder != nullptr)
        encoder->~MBEEncoder();
    encoder = new (encStorage) MBEEncoder(); /* A fresh analysis state */
    encoder->set_dmr_mode();
    statBursts = statMaxUs = statDropped = 0;
    k_sem_reset(&encDone);
    running = true;
    k_sem_give(&encRun);
    return true;
}

bool dmrTxEnc_pop(uint8_t ambe[3][9])
{
    struct burstFrames b;
    if (k_msgq_get(&encQueue, &b, K_NO_WAIT) != 0)
        return false;
    memcpy(ambe, b.f, sizeof(b.f));
    return true;
}

void dmrTxEnc_stop(void)
{
    if (!running)
        return;
    running = false;
    if (micStream >= 0)
        audioStream_terminate(micStream);
    k_sem_take(&encDone, K_MSEC(300));
    if (micPath >= 0)
        audioPath_release(micPath);
    micStream = -1;
    micPath = -1;
    k_msgq_purge(&encQueue);
}

void dmrTxEnc_stats(uint32_t *bursts, uint32_t *maxUs, uint32_t *dropped)
{
    *bursts = statBursts;
    *maxUs = statMaxUs;
    *dropped = statDropped;
}
