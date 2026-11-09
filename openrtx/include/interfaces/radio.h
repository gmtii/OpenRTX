/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>
#include "rtx/rtx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This file provides a common interface for the platform-dependent low-level
 * rtx driver. Top level application code normally does not have to call directly
 * the API functions provided here, since all the transceiver managment, comprised
 * the handling of digital protocols is done by the 'rtx' module.
 *
 * The radio functionalities are controlled by means of an rtxStatus_t data
 * structure containing all the parameters required to define a given operating
 * configuration for the RF stage such as TX and RX frequencies, ...
 * The data structure is internally accessed by each of the API functions and is
 * guaranteed that the access is performed in read only mode.
 */

/**
 * Initialise low-level radio transceiver.
 *
 * @param rtxState: pointer to an rtxStatus_t structure used to describe the
 * operating configuration of the radio module.
 */
void radio_init(const rtxStatus_t *rtxState);

/**
 * Shut down low-level radio transceiver.
 */
void radio_terminate();

/**
 * Set current operating mode.
 *
 * @param mode: new operating mode.
 */
void radio_setOpmode(const enum opmode mode);

/**
 * Check if digital squelch is opened, that is if a CTC/DCS code is being
 * detected.
 *
 * @return true if RX digital squelch is enabled and if the configured CTC/DCS
 * code is present alongside the carrier.
 */
bool radio_checkRxDigitalSquelch();

/**
 * Enable AF output towards the speakers.
 */
void radio_enableAfOutput();

/**
 * Disable AF output towards the speakers.
 */
void radio_disableAfOutput();

/**
 * Enable the RX stage.
 */
void radio_enableRx();

/**
 * Enable the TX stage.
 */
void radio_enableTx();

/**
 * Disable both the RX and TX stages, as long as transmission of CTC/DCS code
 * and digital squelch.
 */
void radio_disableRtx();

/**
 * Update configuration of the radio module to match the one currently described
 * by the rtxStatus_t configuration data structure.
 * This function has to be called whenever the configuration data structure has
 * been updated, to ensure all the operating parameters of the radio driver are
 * correctly configured.
 */
void radio_updateConfiguration();

/**
 * Get the current RSSI level in dBm.
 *
 * @return RSSI level in dBm.
 */
rssi_t radio_getRssi();

/**
 * Prepare the radio for a spectrum sweep: receiver on, audio muted, fixed
 * gain, so that RSSI readings on different frequencies can be compared.
 * Only radios building the spectrum view (CONFIG_SPECTRUM) implement the
 * radio_spectrum functions; they are called from the RTX thread.
 */
void radio_spectrumBegin();

/**
 * Tune the receiver to a frequency and measure the RSSI there, during a
 * spectrum sweep.
 *
 * @param freq: frequency to measure, in Hz.
 * @return RSSI level in dBm.
 */
rssi_t radio_spectrumProbe(const freq_t freq);

/**
 * End a spectrum sweep, leaving the radio off: the operating mode turns the
 * receiver on again.
 */
void radio_spectrumEnd();

/**
 * Broadcast FM receiver, on radios with one (CONFIG_FM_BROADCAST). The
 * main receiver is off meanwhile; the broadcast audio goes to the speaker.
 * Called from the RTX thread.
 *
 * @param freq: frequency in Hz, 87.5 to 108 MHz.
 * @param volume: 0 to 15.
 */
void radio_broadcastOn(const freq_t freq, const uint8_t volume);

/**
 * Tune the broadcast receiver.
 *
 * @param freq: frequency in Hz, 87.5 to 108 MHz.
 */
void radio_broadcastTune(const freq_t freq);

/**
 * Set the broadcast receiver volume, 0 (silent) to 15.
 */
void radio_broadcastVolume(const uint8_t volume);

/**
 * Signal of the broadcast station tuned.
 *
 * @param rssi: signal strength, 0 to 255 (chip units).
 * @param snr: signal to noise ratio, 0 to 15 (chip units).
 * @return true if a station is tuned (strong enough, centred), as a seek
 * stops on.
 */
bool radio_broadcastStatus(uint8_t *rssi, uint8_t *snr);

/**
 * Turn the broadcast receiver off and free the speaker.
 */
void radio_broadcastOff();

/**
 * Get the current operating status of the radio module.
 *
 * @return current operating status.
 */
enum opstatus radio_getStatus();

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
