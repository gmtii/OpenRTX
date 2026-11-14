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
#include "protocols/TETRA/TetraCell.h"
#include "protocols/TETRA/TetraDemod.h"
#include "protocols/TETRA/TetraScan.h"
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

/*
 * Site discovery, per cell visited: the RSSI is the mean of the second half
 * of the settling time; below SCAN_NOISE_DBM there is nothing to sync to.
 * Otherwise wait up to SCAN_SYNC_MS for the burst synchronisation (the
 * sync burst comes about once a second), then up to SCAN_SYSINFO_MS more
 * for the system information, measuring the phase error meanwhile.
 */
#define SCAN_NOISE_DBM -118
#define SCAN_SYNC_MS 2500
#define SCAN_SYSINFO_MS 1500
#define SCAN_ERR_BLOCKS 10 /* Phase error blocks wanted before leaving */
/* Uplink below the downlink when the cell gave no duplex spacing */
#define UPLINK_SPACING_HZ 10000000

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
    , tunedHz(0)
    , following(false)
    , scanning(false)
    , scanSync(false)
    , scanIdx(0)
    , scanCount(0)
    , scanBlocks(0)
    , scanRssiSum(0)
    , scanErrSum(0)
    , scanErrBlocks(0)
    , scanDeadline(0)
    , uplink(false)
    , ulMcc(0)
    , ulMnc(0)
    , ulColour(0)
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
    /* Still on a followed carrier: keep holding */
    tetraDemod_setHold(&demod, following);
}

void OpMode_TETRA::reset()
{
    scanning = false;
    following = false;
    uplink = false;
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
    initTmo(tetraStatus_onEvent);
    if (uplink)
        tetra_tmo_set_uplink_cell(&tmo, ulMcc, ulMnc, ulColour);
    tetraStatus_lostSync(overrun);
}

void OpMode_TETRA::initTmo(void (*cb)(void *, const struct tetra_event *))
{
    struct tetra_rx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = cb;
    cfg.promiscuous = true;
    tetra_tmo_init(&tmo, &cfg);
}

/*
 * Site discovery: visit the serving cell and each neighbour it announces,
 * measuring the RSSI and trying to synchronise to it. It runs in place of
 * normal reception, through the same demodulator and libtetra receiver,
 * whose events go to TetraScan instead: the serving cell, the calls and the
 * SDS heard are kept. No speech and no call following meanwhile.
 */
void OpMode_TETRA::scanBegin(rtxStatus_t *const rtx)
{
    scanCount = tetraScan_begin(rtx->rxFrequency);
    if (scanCount == 0) {
        tetraScan_end();
        return;
    }
    scanning = true;
    following = false;
    platform_ledOff(GREEN);
    scanVisit(0);
}

void OpMode_TETRA::scanVisit(int i)
{
    scanIdx = i;
    const uint32_t hz = tetraScan_freq(i);
    radio_retuneRx(hz);
    tunedHz = hz;
    initDemod();
    initTmo(tetraScan_onEvent);
    scanSync = false;
    scanBlocks = 0;
    scanRssiSum = 0;
    scanErrSum = 0;
    scanErrBlocks = 0;
}

void OpMode_TETRA::scanNext(rtxStatus_t *const rtx)
{
    const uint16_t err = scanErrBlocks ? scanErrSum / scanErrBlocks : 0;
    tetraScan_visited(scanIdx, err);
    if (scanIdx + 1 < scanCount)
        scanVisit(scanIdx + 1);
    else
        scanEnd(rtx);
}

void OpMode_TETRA::scanEnd(rtxStatus_t *const rtx)
{
    tetraScan_end();
    scanning = false;
    radio_retuneRx(rtx->rxFrequency);
    tunedHz = rtx->rxFrequency;
    initDemod();
    settleBlocks = SETTLE_BLOCKS;
    resync(false);
    tetraStatus_scanDone();
}

void OpMode_TETRA::scanStep(rtxStatus_t *const rtx, const dataBlock_t &block)
{
    /* Cancelled from the UI */
    if (!tetraScan_running()) {
        scanEnd(rtx);
        return;
    }

    /* Settling: the RSSI from its second half */
    if (scanBlocks < SETTLE_BLOCKS) {
        scanBlocks++;
        if (scanBlocks > SETTLE_BLOCKS / 2)
            scanRssiSum += radio_getRssi();
        if (scanBlocks < SETTLE_BLOCKS)
            return;

        const int16_t rssi = scanRssiSum / (SETTLE_BLOCKS / 2);
        tetraScan_setRssi(scanIdx, rssi);
        if (rssi < SCAN_NOISE_DBM) {
            scanNext(rtx);
            return;
        }
        scanDeadline = getTick() + SCAN_SYNC_MS;
        return;
    }

    size_t n = tetraDemod_process(&demod, block.data, block.len, soft);
    tetra_tmo_push_soft(&tmo, soft, n);

    const long long now = getTick();
    if (tetraScan_synced(scanIdx)) {
        if (!scanSync) {
            scanSync = true;
            scanDeadline = now + SCAN_SYSINFO_MS;
        }
        scanErrSum += tetraDemod_phaseError(&demod);
        scanErrBlocks++;
        if (tetraScan_sysinfo(scanIdx) && scanErrBlocks >= SCAN_ERR_BLOCKS) {
            scanNext(rtx);
            return;
        }
    }
    if (now >= scanDeadline)
        scanNext(rtx);
}

/*
 * Uplink: what the radios send to the cell, on the uplink frequency of its
 * main carrier. libtetra decodes it while not synchronised to a downlink,
 * each burst aligned on its own training sequence, given the cell for the
 * scrambling. The radios' bursts come and go, each with its own timing:
 * the demodulator goes to burst mode, holding what it learned on the
 * downlink (clock, gain, offset, equalizer).
 */
void OpMode_TETRA::uplinkBegin(rtxStatus_t *const rtx)
{
    struct tetraCellInfo c;
    tetraCell_get(&c);
    if (!c.valid) {
        /* No cell heard: nothing to descramble with */
        tetraStatus_setUplink(false);
        return;
    }

    /* The cell's duplex spacing, else the usual 10 MHz below */
    int32_t spacing = UPLINK_SPACING_HZ;
    if (c.sysinfo && c.dlHz != 0 && c.ulHz != 0)
        spacing = (int32_t)(c.dlHz - c.ulHz);
    const uint32_t ulHz = (uint32_t)((int32_t)rtx->rxFrequency - spacing);

    ulMcc = c.mcc;
    ulMnc = c.mnc;
    ulColour = c.colourCode;
    uplink = true;
    following = false;
    tetraStatus_uplinkActive(true, ulHz);

    radio_retuneRx(ulHz);
    tunedHz = ulHz;
    tetraDemod_setHold(&demod, true);
    tetraDemod_setBurst(&demod, true);
    tetra_tmo_resync(&tmo);
    tetra_tmo_set_uplink_cell(&tmo, ulMcc, ulMnc, ulColour);
    settleBlocks = SETTLE_BLOCKS;
}

void OpMode_TETRA::uplinkEnd(rtxStatus_t *const rtx)
{
    uplink = false;
    radio_retuneRx(rtx->rxFrequency);
    tunedHz = rtx->rxFrequency;
    tetraDemod_setBurst(&demod, false);
    tetraDemod_setHold(&demod, false);
    demodJumps = tetraDemod_reacquisitions(&demod);
    reacqCount = 0;
    tetra_tmo_resync(&tmo);
    tetraStatus_uplinkActive(false, 0);
    settleBlocks = SETTLE_BLOCKS;
}

void OpMode_TETRA::enable()
{
    reset();
    tetraSpeech_start();
    startRx = true;
}

void OpMode_TETRA::disable()
{
    if (scanning) {
        scanning = false;
        tetraScan_end();
    }
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
        tunedHz = rtx->rxFrequency;
        startRx = false;
    }

    /* A new frequency is a new cell: start from scratch. The radio driver
     * tuned the configured frequency again. */
    if (newCfg) {
        reset();
        tunedHz = rtx->rxFrequency;
    }

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
        /* A scan visit has no slot grid to lose */
        if (dataErrors++ == 0 && !scanning)
            resync(true);
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    /* Site discovery, in place of normal reception: from the downlink */
    if (!scanning && tetraScan_running()) {
        if (uplink) {
            tetraStatus_setUplink(false);
            uplinkEnd(rtx);
        }
        scanBegin(rtx);
    }
    if (scanning) {
        scanStep(rtx, block);
        return;
    }

    /* The uplink asked for, or the downlink back */
    const bool wantUplink = tetraStatus_uplinkWanted();
    if (wantUplink && !uplink)
        uplinkBegin(rtx);
    else if (!wantUplink && uplink)
        uplinkEnd(rtx);

    if (settleBlocks > 0) {
        settleBlocks--;
        return;
    }

    size_t n = tetraDemod_process(&demod, block.data, block.len, soft);

    /* On the uplink: no slot grid to keep, no call to follow */
    if (uplink) {
        tetra_tmo_push_soft(&tmo, soft, n);
        const bool present = tetraDemod_present(&demod);
        tetraStatus_setDemod(tetraDemod_phaseError(&demod),
                             tetraDemod_carrierOffset(&demod), present);
        if (present)
            platform_ledOn(GREEN);
        else
            platform_ledOff(GREEN);
        return;
    }

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

    /*
     * Follow a clear call to another carrier of the cell, and come back.
     * On the followed carrier the demodulator holds the main carrier's
     * timing whatever the error: the carrier may carry only the call's
     * timeslot, with noise in between, and a re-acquisition loses the timing
     * libtetra finds the slots by.
     */
    const uint32_t wantHz = tetraStatus_followHz(rtx->rxFrequency);
    if (wantHz != tunedHz) {
        radio_retuneRx(wantHz);
        tunedHz = wantHz;
        following = (wantHz != rtx->rxFrequency);
        tetraDemod_setHold(&demod, following);
        reacqCount = 0;
    }

    tetraStatus_setDemod(tetraDemod_phaseError(&demod),
                         tetraDemod_carrierOffset(&demod), false);

    if (tetraStatus_sync())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
