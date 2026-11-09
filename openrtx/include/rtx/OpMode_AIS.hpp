/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_AIS_H
#define OPMODE_AIS_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

/**
 * AIS receive mode: the radio's FM discriminator output, 9600 bit/s GMSK,
 * decoded in software (protocols/AIS). Receive only: PTT does nothing.
 *
 * Needs a 48 kHz RX audio input and a radio driver that gives the wide,
 * flat discriminator path (as for TETRA) for OPMODE_AIS.
 */
class OpMode_AIS : public OpMode
{
public:
    OpMode_AIS();
    ~OpMode_AIS();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_AIS;
    }

    /**
     * Open for a few seconds after a report: wakes the screen.
     */
    virtual bool rxSquelchOpen() override;

private:
    void startSampling();
    void stopSampling();

    bool startRx;
    unsigned dataErrors; // Consecutive failed reads of the sample stream
    pathId basebandPath;
    streamId basebandId;
};

#endif /* OPMODE_AIS_H */
