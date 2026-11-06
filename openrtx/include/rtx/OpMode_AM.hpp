/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_AM_H
#define OPMODE_AM_H

#include "OpMode_FM.hpp"

/**
 * Analog AM receive mode, for the air band: the radio's own AM detector,
 * with the RF squelch and audio routing of the FM mode. Receive only: PTT
 * does nothing in this mode.
 *
 * Needs a radio driver that selects its AM detector for OPMODE_AM.
 */
class OpMode_AM : public OpMode_FM
{
public:
    virtual void update(rtxStatus_t *const status, const bool newCfg) override
    {
        /* Receive only */
        status->txDisable = 1;
        OpMode_FM::update(status, newCfg);
    }

    virtual opmode getID() override
    {
        return OPMODE_AM;
    }
};

#endif /* OPMODE_AM_H */
