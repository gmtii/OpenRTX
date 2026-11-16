/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_NXDN_H
#define OPMODE_NXDN_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "OpMode.hpp"
#include "protocols/DMR/DmrSpeech.h"

/**
 * NXDN48 receive mode: the flat discriminator audio into a software 4FSK
 * demodulator, speech through the DMR vocoder.
 */
class OpMode_NXDN : public OpMode
{
public:
    OpMode_NXDN();
    ~OpMode_NXDN();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_NXDN;
    }

    /**
     * Open while speech is played: wakes the screen as FM does.
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

#endif /* OPMODE_NXDN_H */
