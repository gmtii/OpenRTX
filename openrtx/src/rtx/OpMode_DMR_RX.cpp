/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <hwconfig.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#endif
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "rtx/OpMode_DMR_RX.hpp"

#define DMR_IN_RATE 48000
/* 30 ms per half buffer: one burst period */
#define HALF_SAMPLES 1440

static int16_t baseband[2 * HALF_SAMPLES];
static float samples[HALF_SAMPLES];

/**
 * Call events (start, end, IDs, alias) on the console.
 */
static void printLine(const char *line)
{
#ifdef __ZEPHYR__
    printk("DMR %s\n", line); /* Unbuffered, safe from any thread */
#else
    printf("DMR %s\n", line);
#endif
}

OpMode_DMR_RX::OpMode_DMR_RX()
    : startRx(false), dataErrors(0), basebandPath(-1), basebandId(-1)
{
}

OpMode_DMR_RX::~OpMode_DMR_RX()
{
    disable();
}

void OpMode_DMR_RX::enable()
{
    dmr_demod_init();
    dmr_proto_set_printer(printLine);
    dmrSpeech_start();
    startRx = true;
}

void OpMode_DMR_RX::disable()
{
    startRx = false;
    stopSampling();
    dmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dmr_demod_reset();
}

void OpMode_DMR_RX::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   DMR_IN_RATE, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DMR_RX::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_DMR_RX::update(rtxStatus_t *const rtx, const bool newCfg)
{
    switch (rtx->opStatus) {
        case OFF:
            radio_disableRtx();
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

        case RX:
            break;

        default:
            /* Receive only */
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    /* A new frequency is a new channel: start from scratch */
    if (newCfg)
        dmr_demod_reset();

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples. A failed read means we were
     * late and samples were lost; the framer finds the bursts again by
     * itself. Do not lose more by sleeping, unless the stream keeps failing.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0)
            dmr_demod_note_overrun();
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    /* The demodulator takes any scale: levels come from the sync words */
    size_t n = block.len;
    if (n > HALF_SAMPLES)
        n = HALF_SAMPLES;
    for (size_t i = 0; i < n; i++)
        samples[i] = (float)block.data[i];
    dmr_demod_feed_disc(samples, (int)n);

    dmr_demod_status_t ds;
    dmr_demod_get_status(&ds);
    if (ds.locked)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
