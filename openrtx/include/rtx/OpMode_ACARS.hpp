/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_ACARS_H
#define OPMODE_ACARS_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"

/**
 * ACARS receive mode: the radio's AM detector output, 2400 bit/s MSK,
 * decoded in software (protocols/ACARS). Receive only: PTT does nothing.
 *
 * Needs a 48 kHz RX audio input and a radio driver that selects its AM
 * detector with a flat audio path for OPMODE_ACARS.
 */
class OpMode_ACARS : public OpMode
{
public:
    OpMode_ACARS();
    ~OpMode_ACARS();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_ACARS;
    }

    /**
     * Open for a few seconds after a message: wakes the screen.
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

#endif /* OPMODE_ACARS_H */
