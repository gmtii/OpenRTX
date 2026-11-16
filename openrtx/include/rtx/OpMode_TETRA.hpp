/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_TETRA_H
#define OPMODE_TETRA_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"
#include "protocols/TETRA/TetraSpeech.h"

struct tetra_event;

/**
 * TETRA TMO downlink receive mode: demodulates the radio's flat FM
 * discriminator output (pi/4-DQPSK by integrate-and-dump) and decodes it
 * with libtetra. Receive only: PTT does nothing in this mode.
 *
 * Needs a 48 kHz RX audio input and a radio driver that sets up a wide,
 * flat receive path for OPMODE_TETRA.
 */
class OpMode_TETRA : public OpMode
{
public:
    OpMode_TETRA();
    ~OpMode_TETRA();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_TETRA;
    }

    /**
     * Open while a clear call is played: wakes the screen as FM does.
     */
    virtual bool rxSquelchOpen() override
    {
        return tetraSpeech_playing();
    }

private:
    void initDemod();
    void reset();
    void resync(bool overrun);
    void initTmo(void (*cb)(void *, const struct tetra_event *));
    void scanBegin(rtxStatus_t *const rtx);
    void scanVisit(int i);
    void scanNext(rtxStatus_t *const rtx);
    void scanEnd(rtxStatus_t *const rtx);
    void scanStep(rtxStatus_t *const rtx, const dataBlock_t &block);
    void roamBegin();
    uint32_t roamPick();
    void uplinkBegin(rtxStatus_t *const rtx);
    void uplinkEnd(rtxStatus_t *const rtx);
    void startSampling();
    void stopSampling();

    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    uint32_t demodJumps; // Demodulator slips and re-acquisitions seen
    unsigned settleBlocks;     // Sample blocks still to drop
    unsigned reacqCount;       // Re-acquisitions within the window
    uint32_t reacqWindowStart; // Start of that window, in ms
    uint32_t tunedHz;          // Receiver frequency: configured or followed
    uint32_t mainHz;           // The cell's: configured, or tuned by roaming
    bool following;            // On another carrier of the cell, for a call
    bool scanning;             // Site discovery in place of reception
    bool scanSync;             // The cell visited reached synchronisation
    int scanIdx;               // Cell visited
    int scanCount;             // Cells to visit
    unsigned scanBlocks;       // Blocks since the visit started, settling
    int32_t scanRssiSum;       // RSSI readings while settling, dBm
    uint32_t scanErrSum;       // Phase error readings while in sync
    unsigned scanErrBlocks;    // Number of them
    long long scanDeadline;    // End of the wait for sync or sysinfo
    bool uplink;               // On the uplink of the cell
    uint16_t ulMcc;            // Its cell, for the uplink scrambling
    uint16_t ulMnc;
    uint8_t ulColour;
    bool roamScan;             // The scan running is the roaming's
    bool roamPending;          // Roaming tuned mainHz, the UI follows it
    bool roamWaiting;          // No cell found: scanning again later
    long long lastSyncAt;      // Tick of the last block in synchronisation
    long long roamCheckAt;     // Tick of the next table refresh
    long long roamRetryAt;     // Tick of the next roaming scan
    pathId basebandPath;
    streamId basebandId;
};

#endif /* OPMODE_TETRA_H */
