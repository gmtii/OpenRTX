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
#include "protocols/TETRA/TetraRoam.h"
#include "protocols/TETRA/TetraScan.h"
#include "protocols/TETRA/TetraSpeech.h"
#include "protocols/TETRA/TetraStatus.h"
#include "rtx/OpMode_TETRA.hpp"
#ifdef CONFIG_TETRA_AFC
#include <math.h>
#include <pthread.h>
#include "core/state.h"
#include "rtx/rtx.h"
#endif

/* 30 ms per half buffer: room for libtetra's per-slot decoding */
#define HALF_SAMPLES 1440

/*
 * Blocks dropped after sampling starts or the frequency changes (240 ms): the
 * radio audio is still settling, and the demodulator measures its gain and
 * offset on what comes first.
 */
#define SETTLE_BLOCKS 8
/*
 * Blocks dropped after the receiver starts or a new frequency is set: about
 * 0.5 s, while the radio and its audio path settle. The demodulator took
 * their transient for the carrier offset: a C62 turned on in TETRA mode
 * locked after about 18 s instead of 3 (SP5WWP's c62/tetra, 6193eb54).
 */
#define START_BLOCKS 17

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
/*
 * Roaming: the table is scanned once the burst synchronisation has been
 * lost this long, after the start of reception or a cell change too; if no
 * cell of the network is found, again ROAM_RETRY_MS later. The table
 * follows the cell received once a second.
 */
#define ROAM_LOST_MS 3000
#define ROAM_RETRY_MS 10000
#define ROAM_REFRESH_MS 1000
/* Uplink below the downlink when the cell gave no duplex spacing */
#define UPLINK_SPACING_HZ 10000000

#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define ROAM_PRINT printk
#else
#define ROAM_PRINT printf
#endif

/* Large state: static, there is only one TETRA mode */
static int16_t baseband[2 * HALF_SAMPLES];
static int8_t soft[HALF_SAMPLES * 3 / 4 + 8];
static struct tetraDemod demod;
static struct tetra_tmo_rx tmo;
static uint32_t lastCrc; /* libtetra's blocks passing their CRC, last seen */

#ifdef CONFIG_TETRA_AFC
/*
 * Automatic frequency correction, from SP5WWP's c62/tetra branch (2188b7df,
 * 21219d8f). Every AFC_PERIOD_MS the mean carrier offset measured by the
 * demodulator while blocks decode moves the receiver's fine tuning towards
 * a zero reading. On the C62 the reading is about 0.15 of the real offset
 * (the receive path removes most of the carrier's DC), but it is monotonic
 * and zero where the phase error is lowest, about 1.6 kHz off the nominal
 * frequency; the loop gain allows for it. The correction is mostly the
 * radio's own (filters, crystal): it is kept across retunes (carriers
 * followed, scans, uplink, roaming), stored apart from the settings when it
 * moved far enough, and started from at power on, so that a radio turned
 * on with a signal too weak to steer on starts corrected.
 */
#define AFC_PERIOD_MS 2000
#define AFC_GAIN 3       /* Hz of correction per Hz of mean reading */
#define AFC_STEP_MAX 300 /* Hz per period */
#define AFC_DEADBAND 25  /* Hz of mean reading */
/*
 * Each correction retunes the BK4819, which restarts its receiver: the RSSI
 * drops to its minimum and the demodulator gets a few milliseconds of
 * garbage. Once within AFC_DEADBAND the correction holds until the reading
 * stays beyond this for AFC_HOLD_PERIODS periods in a row (the reading
 * jitters, the radio's error hardly moves), and never moves while speech
 * is played.
 */
#define AFC_HOLD_BAND (3 * AFC_DEADBAND)
#define AFC_HOLD_PERIODS 2
#define AFC_RANGE 4000   /* Hz either way: the C62 decodes about 4 kHz off */
#define AFC_CRC_RECENT_MS 1000
/* Readings trusted below this phase error (% of pi/4): near the edges of
 * the band where the C62 still decodes they can have the wrong sign */
#define AFC_ERROR_MAX 35
/* Nothing decoded this long, with a signal this strong (dBm): the
 * correction may be wrong for this signal; back to the stored one, or from
 * it to none. A weak signal that does not decode is just weak. */
#define AFC_LOST_MS 8000
#define AFC_LOST_RSSI -90
#define AFC_STORE_STEP 200 /* Hz moved before storing, to spare the flash */

static int32_t afcOffset;
static float afcSum;
static uint32_t afcCount;
static long long afcStart;
static long long lastCrcTick;
static bool afcLoaded; /* Stored correction read, once per power on */
static bool afcSettled; /* Within AFC_DEADBAND: holding */
static unsigned afcOver; /* Periods in a row beyond AFC_HOLD_BAND */

static bool afcEnabled(void)
{
    return state.settings.tetraAfcOff == 0;
}

static int32_t afcStored(void)
{
    return state.tetra_afc;
}

/**
 * The correction in effect, 0 if off: for the diagnostics page.
 */
int32_t tetra_afcOffset(void)
{
    return afcEnabled() ? afcOffset : 0;
}

/**
 * Store the correction once it has moved far enough from the stored one:
 * saved to flash a few seconds later (CONFIG_NVM_SAVE_ON_CHANGE).
 */
static void afcStore(void)
{
    int32_t delta = afcOffset - afcStored();
    if (delta < AFC_STORE_STEP && delta > -AFC_STORE_STEP)
        return;
    pthread_mutex_lock(&state_mutex);
    state.tetra_afc = (int16_t)afcOffset;
    pthread_mutex_unlock(&state_mutex);
}

/**
 * Before receiving on a new frequency: the correction found so far, the
 * stored one at the first time.
 */
static void afcBegin(void)
{
    if (!afcLoaded) {
        int32_t stored = afcStored();
        if (stored > AFC_RANGE || stored < -AFC_RANGE)
            stored = 0;
        afcOffset = (stored / 10) * 10;
        afcLoaded = true;
    }
    radio_setRxOffset(afcEnabled() ? afcOffset : 0);
    afcSum = 0;
    afcCount = 0;
    afcSettled = false;
    afcOver = 0;
    afcStart = getTick();
    lastCrcTick = afcStart; /* Not "lost" before trying */
}

/**
 * After each block of samples received on the downlink.
 */
static void afcUpdate(const struct tetraDemod *dm,
                      const struct tetra_tmo_rx *rx)
{
    if (!afcEnabled()) {
        radio_setRxOffset(0);
        afcSum = 0;
        afcCount = 0;
        return;
    }
    radio_setRxOffset(afcOffset); /* Turned on again */

    /* Measure only while blocks decode: never steer on noise */
    long long now = getTick();
    if (tetra_tmo_locked(rx) && (now - lastCrcTick) < AFC_CRC_RECENT_MS
        && tetraDemod_phaseError(dm) < AFC_ERROR_MAX) {
        afcSum += tetraDemod_carrierOffset(dm);
        afcCount++;
    }

    if ((now - lastCrcTick) > AFC_LOST_MS && rtx_getRssi() > AFC_LOST_RSSI) {
        int32_t stored = afcStored();
        int32_t next = (afcOffset != stored) ? stored : 0;
        if (next == afcOffset)
            return;
        afcOffset = next;
        radio_setRxOffset(afcOffset);
        ROAM_PRINT("TETRA AFC: %+ld Hz, nothing decoded\n", (long)afcOffset);
        afcSum = 0;
        afcCount = 0;
        afcSettled = false;
        afcOver = 0;
        afcStart = now;
        lastCrcTick = now; /* Give it the time to decode */
        return;
    }
    if ((now - afcStart) < AFC_PERIOD_MS)
        return;
    afcStart = now;

    uint32_t count = afcCount;
    float mean = (count > 0) ? afcSum / count : 0;
    afcSum = 0;
    afcCount = 0;
    if (count < 10)
        return;
    if (fabsf(mean) <= AFC_DEADBAND)
        afcSettled = true;
    if (fabsf(mean) <= (afcSettled ? AFC_HOLD_BAND : AFC_DEADBAND)) {
        afcOver = 0;
        return;
    }
    if (afcSettled && ++afcOver < AFC_HOLD_PERIODS)
        return;
    /* No retune under the speech played: the next period will do */
    if (tetraSpeech_playing())
        return;
    afcOver = 0;
    afcSettled = false;

    float step = AFC_GAIN * mean;
    if (step > AFC_STEP_MAX)
        step = AFC_STEP_MAX;
    else if (step < -AFC_STEP_MAX)
        step = -AFC_STEP_MAX;
    int32_t next = afcOffset + (int32_t)lrintf(step);
    if (next > AFC_RANGE)
        next = AFC_RANGE;
    else if (next < -AFC_RANGE)
        next = -AFC_RANGE;
    afcOffset = (next / 10) * 10; /* The BK4819 tunes in 10 Hz steps */
    radio_setRxOffset(afcOffset);
    ROAM_PRINT("TETRA AFC: %+ld Hz (reading %+d Hz)\n", (long)afcOffset,
               (int)lrintf(mean));
    afcStore();
}
#else
int32_t tetra_afcOffset(void)
{
    return 0;
}
#endif

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
    , mainHz(0)
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
    , roamScan(false)
    , roamPending(false)
    , roamWaiting(false)
    , lastSyncAt(0)
    , roamCheckAt(0)
    , roamRetryAt(0)
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
    roamScan = false;
    roamPending = false;
    roamWaiting = false;
    following = false;
    uplink = false;
    initDemod();
#ifdef CONFIG_TETRA_AFC
    afcBegin();
#endif
    settleBlocks = START_BLOCKS;
    resync(false);
    tetraStatus_reset();
    lastSyncAt = getTick();
    roamRetryAt = 0;
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
    (void)rtx;
    scanCount = tetraScan_begin(mainHz);
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
    (void)rtx;
    tetraScan_end();
    scanning = false;

    /* Roaming: the strongest cell of the network found, if any */
    bool moved = false;
    if (roamScan) {
        roamScan = false;
        const uint32_t best = roamPick();
        const long long now = getTick();
        roamWaiting = (best == 0);
        roamRetryAt = roamWaiting ? now + ROAM_RETRY_MS : 0;
        tetraRoam_setState(roamWaiting ? TETRA_ROAM_WAITING :
                                         TETRA_ROAM_LISTENING,
                           roamRetryAt);
        if (best != 0 && best != mainHz) {
            mainHz = best;
            moved = true;
        }
    }

    radio_retuneRx(mainHz);
    tunedHz = mainHz;
    initDemod();
    settleBlocks = SETTLE_BLOCKS;
    if (moved) {
        /* A new cell: start from scratch there, the UI following it */
        tetraStatus_reset();
        roamPending = true;
        tetraRoam_moved(mainHz);
    }
    resync(false);
    tetraStatus_scanDone();
    /* Time to synchronise before the roaming looks again */
    lastSyncAt = getTick();
}

/*
 * Roaming: scan the table of cells, in place of reception as the site scan
 * does; the scan's end tunes the best cell found (see scanEnd()).
 */
void OpMode_TETRA::roamBegin()
{
    static uint32_t hz[TETRA_ROAM_MAX];
    static uint8_t cellId[TETRA_ROAM_MAX];
    const int n = tetraRoam_cells(hz, cellId);
    scanCount = tetraScan_beginList(hz, cellId, n);
    if (scanCount == 0) {
        roamWaiting = true;
        roamRetryAt = getTick() + ROAM_RETRY_MS;
        tetraRoam_setState(TETRA_ROAM_WAITING, roamRetryAt);
        return;
    }
    ROAM_PRINT("TETRA: roaming, scanning %d cells\n", scanCount);
    tetraRoam_setState(TETRA_ROAM_SCANNING, 0);
    roamScan = true;
    scanning = true;
    following = false;
    platform_ledOff(GREEN);
    scanVisit(0);
}

/*
 * The cell to roam to: synchronised to, of the network, the strongest.
 *
 * @return its frequency, 0 if none.
 */
uint32_t OpMode_TETRA::roamPick()
{
    static struct tetraScanResult sr[TETRA_SCAN_MAX];
    /* Synchronised first, then by RSSI */
    const int n = tetraScan_results(sr, NULL);
    for (int i = 0; i < n; i++) {
        if (sr[i].measured && sr[i].sync && sr[i].cell
            && tetraRoam_network(sr[i].mcc, sr[i].mnc)) {
            ROAM_PRINT("TETRA: roaming to %lu Hz, %d dBm\n",
                   (unsigned long)sr[i].dlHz, sr[i].rssi);
            return sr[i].dlHz;
        }
    }
    ROAM_PRINT("TETRA: roaming, no cell of the network found\n");
    return 0;
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
    const uint32_t ulHz = (uint32_t)((int32_t)mainHz - spacing);

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
    (void)rtx;
    uplink = false;
    radio_retuneRx(mainHz);
    tunedHz = mainHz;
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
    /* Roaming is for the TETRA mode */
    tetraRoam_stop();
#ifdef CONFIG_TETRA_AFC
    /* The other modes tune where they are told */
    radio_setRxOffset(0);
#endif
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
    settleBlocks = START_BLOCKS;
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
        mainHz = rtx->rxFrequency;
        startRx = false;
    }

    /* A new frequency is a new cell: start from scratch. The radio driver
     * tuned the configured frequency again. Not when it is the UI following
     * the cell roaming tuned: that one is already being received. */
    if (newCfg) {
        if (roamPending && rtx->rxFrequency == mainHz && !following) {
            roamPending = false;
        } else {
            reset();
            tunedHz = rtx->rxFrequency;
            mainHz = rtx->rxFrequency;
        }
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

    /*
     * Roaming, on the downlink: the table follows the cell received, and
     * once the synchronisation is lost for a while the table is scanned for
     * the best cell of the network.
     */
    const long long now = getTick();
    const bool sync = tetraStatus_sync();
    if (sync)
        lastSyncAt = now;
    if (!uplink && tetraRoam_on()) {
        if (sync && now >= roamCheckAt) {
            roamCheckAt = now + ROAM_REFRESH_MS;
            tetraRoam_refresh(mainHz);
            if (roamWaiting) {
                roamWaiting = false;
                tetraRoam_setState(TETRA_ROAM_LISTENING, 0);
            }
        }
        if (!sync && !following && (now - lastSyncAt) >= ROAM_LOST_MS
            && now >= roamRetryAt) {
            roamBegin();
            if (scanning)
                return;
        }
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

    /* Blocks decode: the demodulator keeps its lock (a new libtetra
     * context counts from zero again) */
    const uint32_t crc = tetra_tmo_crc_ok(&tmo);
    if (crc > lastCrc) {
        tetraDemod_signalOk(&demod);
#ifdef CONFIG_TETRA_AFC
        lastCrcTick = getTick();
#endif
    }
    lastCrc = crc;

    /*
     * Follow a clear call to another carrier of the cell, and come back.
     * On the followed carrier the demodulator holds the main carrier's
     * timing whatever the error: the carrier may carry only the call's
     * timeslot, with noise in between, and a re-acquisition loses the timing
     * libtetra finds the slots by.
     */
    const uint32_t wantHz = tetraStatus_followHz(mainHz);
    if (wantHz != tunedHz) {
        radio_retuneRx(wantHz);
        tunedHz = wantHz;
        following = (wantHz != mainHz);
        tetraDemod_setHold(&demod, following);
        reacqCount = 0;
    }

    tetraStatus_setDemod(tetraDemod_phaseError(&demod),
                         tetraDemod_carrierOffset(&demod), false);

#ifdef CONFIG_TETRA_AFC
    afcUpdate(&demod, &tmo);
#endif

    if (tetraStatus_sync())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
