/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <hwconfig.h>
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/NXDN/nxdn_rx.h"
#include "rtx/OpMode_NXDN.hpp"

#define NXDN_IN_RATE 48000
/* 30 ms per half buffer, as in DMR */
#define HALF_SAMPLES 1440

static int16_t baseband[2 * HALF_SAMPLES];

OpMode_NXDN::OpMode_NXDN()
    : startRx(false), dataErrors(0), basebandPath(-1), basebandId(-1)
{
}

OpMode_NXDN::~OpMode_NXDN()
{
    disable();
}

void OpMode_NXDN::enable()
{
    nxdn_rx_reset();
    dmrSpeech_start();
    startRx = true;
}

void OpMode_NXDN::disable()
{
    startRx = false;
    stopSampling();
    dmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    nxdn_rx_reset();
}

void OpMode_NXDN::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   NXDN_IN_RATE,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_NXDN::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_NXDN::update(rtxStatus_t *const rtx, const bool newCfg)
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
        nxdn_rx_reset();

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    dataBlock_t block = inputStream_getData(basebandId);
    const uint32_t now = (uint32_t)getTick();
    if (block.data == NULL) {
        if (dataErrors++ == 0)
            nxdn_rx_overrun();
        else
            sleepFor(0, 10);
        nxdn_rx_tick(now);
        return;
    }
    dataErrors = 0;

    size_t n = block.len;
    if (n > HALF_SAMPLES)
        n = HALF_SAMPLES;
    nxdn_rx_feed(block.data, (unsigned)n, now);
    nxdn_rx_noteRssi((int)radio_getRssi());
    nxdn_rx_tick(now);

    struct nxdnStatus st;
    nxdn_rx_status(&st);
    if (st.sync)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
