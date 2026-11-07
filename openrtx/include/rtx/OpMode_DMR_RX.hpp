/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_RX_H
#define OPMODE_DMR_RX_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"
#include "protocols/DMR/DmrSpeech.h"

/**
 * DMR receive mode, for radios without a DMR baseband chip: demodulates the
 * radio's flat FM discriminator output (4FSK, 4800 baud) in software and
 * decodes repeater (BS), simplex (MS) and direct mode bursts: colour code,
 * talkgroup, source ID, talker alias, and, with mbelib, the voice of one
 * slot. Receive only: PTT does nothing in this mode.
 *
 * Needs a 48 kHz RX audio input and a radio driver that sets up a wide,
 * flat receive path for OPMODE_DMR.
 */
class OpMode_DMR_RX : public OpMode
{
public:
    OpMode_DMR_RX();
    ~OpMode_DMR_RX();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    /**
     * Open while a clear call is played: wakes the screen as FM does.
     */
    virtual bool rxSquelchOpen() override
    {
        return dmrSpeech_playing();
    }

private:
    void startSampling();
    void stopSampling();

    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    pathId basebandPath;
    streamId basebandId;
};

#endif /* OPMODE_DMR_RX_H */
