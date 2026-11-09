/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <hwconfig.h>
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/ACARS/Acars.h"
#include "rtx/OpMode_ACARS.hpp"

/* 30 ms per half buffer */
#define HALF_SAMPLES 1440
/* The screen wakes this long after a message */
#define WAKE_MS 3000

static int16_t baseband[2 * HALF_SAMPLES];
static uint32_t lastSeq;    // Newest message seen by update()
static long long lastMsgAt; // Tick of its arrival

OpMode_ACARS::OpMode_ACARS()
    : startRx(false), dataErrors(0), basebandPath(-1), basebandId(-1)
{
}

OpMode_ACARS::~OpMode_ACARS()
{
    disable();
}

void OpMode_ACARS::enable()
{
    acars_reset();
    lastSeq = 0;
    lastMsgAt = 0;
    startRx = true;
}

void OpMode_ACARS::disable()
{
    startRx = false;
    stopSampling();
    radio_disableRtx();
    platform_ledOff(GREEN);
}

bool OpMode_ACARS::rxSquelchOpen()
{
    return (lastMsgAt != 0) && (getTick() - lastMsgAt < WAKE_MS);
}

void OpMode_ACARS::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   ACARS_IN_RATE,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_ACARS::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_ACARS::update(rtxStatus_t *const rtx, const bool newCfg)
{
    (void)newCfg; // The receiver finds the next frame by itself

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

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /* Next half buffer; a failed read lost samples, the receiver finds the
     * next frame by itself */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ > 0)
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;
    acars_process(block.data, block.len);

    uint32_t seq = acars_lastSeq();
    if (seq != lastSeq) {
        lastSeq = seq;
        lastMsgAt = getTick();
    }

    /* Green LED while a frame comes in */
    struct acarsStats st;
    acars_getStats(&st);
    if (st.inFrame)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
