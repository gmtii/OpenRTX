/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <hwconfig.h>
#include <tetra/tetra.h>
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/TETRA/TetraDemod.h"
#include "protocols/TETRA/TetraSpeech.h"
#include "protocols/TETRA/TetraStatus.h"
#include "rtx/OpMode_TETRA.hpp"

/* 30 ms per half buffer: room for libtetra's per-slot decoding */
#define HALF_SAMPLES 1440

/*
 * Blocks dropped after sampling starts or the frequency changes (240 ms): the
 * radio audio is still settling, and the demodulator measures its gain and
 * offset on what comes first.
 */
#define SETTLE_BLOCKS 8

/* Re-acquisitions this close together restart the demodulator afresh */
#define REACQ_STUCK 3
#define REACQ_WINDOW_MS 5000

/* Large state: static, there is only one TETRA mode */
static int16_t baseband[2 * HALF_SAMPLES];
static int8_t soft[HALF_SAMPLES * 3 / 4 + 8];
static struct tetraDemod demod;
static struct tetra_tmo_rx tmo;

const struct tetraDemod *tetra_getDemod(void)
{
    return &demod;
}

OpMode_TETRA::OpMode_TETRA()
    : startRx(false)
    , dataErrors(0)
    , demodJumps(0)
    , settleBlocks(0)
    , reacqCount(0)
    , reacqWindowStart(0)
    , basebandPath(-1)
    , basebandId(-1)
{
}

OpMode_TETRA::~OpMode_TETRA()
{
    disable();
}

void OpMode_TETRA::initDemod()
{
    tetraDemod_init(&demod);
#ifdef CONFIG_TETRA_EQ_INIT
    static const float eqInit[TETRA_DEMOD_EQ_TAPS] = CONFIG_TETRA_EQ_INIT;
    tetraDemod_setEqualizer(&demod, eqInit);
#endif
    demodJumps = tetraDemod_reacquisitions(&demod);
    reacqCount = 0;
}

void OpMode_TETRA::reset()
{
    initDemod();
    settleBlocks = SETTLE_BLOCKS;
    resync(false);
    tetraStatus_reset();
}

/*
 * Restart libtetra's burst synchronisation. After samples were lost the
 * symbol stream jumped by an unknown amount, and libtetra would otherwise
 * keep decoding on the old slot grid for seconds before noticing.
 */
void OpMode_TETRA::resync(bool overrun)
{
    struct tetra_rx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = tetraStatus_onEvent;
    cfg.promiscuous = true;
    tetra_tmo_init(&tmo, &cfg);
    tetraStatus_lostSync(overrun);
}

void OpMode_TETRA::enable()
{
    reset();
    tetraSpeech_start();
    startRx = true;
}

void OpMode_TETRA::disable()
{
    startRx = false;
    stopSampling();
    tetraSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    tetraStatus_reset();
}

void OpMode_TETRA::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   TETRA_DEMOD_IN_RATE,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
    settleBlocks = SETTLE_BLOCKS;
}

void OpMode_TETRA::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_TETRA::update(rtxStatus_t *const rtx, const bool newCfg)
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

    /* A new frequency is a new cell: start from scratch */
    if (newCfg)
        reset();

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples. A failed read means we were
     * late and samples were lost: the symbol stream jumped, so resync, and
     * do not lose more by sleeping (unless the stream keeps failing).
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0)
            resync(true);
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    if (settleBlocks > 0) {
        settleBlocks--;
        return;
    }

    size_t n = tetraDemod_process(&demod, block.data, block.len, soft);

    /*
     * The demodulator re-acquired or followed a timing jump. Both move the
     * sampling point by less than a symbol and keep the symbol stream
     * continuous, and libtetra looks for each burst +-2 symbols around where
     * it expects it: restarting libtetra here would drop the cell, the calls
     * and any SDS being reassembled from MAC fragments. libtetra reports a
     * real loss of lock itself.
     *
     * A re-acquisition keeps the clock offset and the polarity, and the fit
     * adapts only while the phase error is low: one made on a bad start
     * (radio audio settling, a fade) may never recover. Without burst
     * synchronisation there is nothing to keep, so start the demodulator
     * afresh instead; and also when re-acquisitions follow each other, which
     * a bad start does while libtetra still decodes the odd burst.
     */
    uint32_t jumps = tetraDemod_reacquisitions(&demod);
    if (jumps != demodJumps) {
        const uint32_t now = (uint32_t)getTick();
        if (reacqCount == 0 || (now - reacqWindowStart) > REACQ_WINDOW_MS) {
            reacqWindowStart = now;
            reacqCount = 0;
        }
        reacqCount++;
        if (!tetraStatus_sync() || reacqCount >= REACQ_STUCK)
            initDemod();
        else
            demodJumps = jumps;
        tetraStatus_demodRestart();
    }

    tetra_tmo_push_soft(&tmo, soft, n);

    tetraStatus_setDemod(tetraDemod_phaseError(&demod),
                         tetraDemod_carrierOffset(&demod));

    if (tetraStatus_sync())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
