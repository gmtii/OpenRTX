/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/radio.h"
#include "hwconfig.h"
#include <string.h>
#include "rtx/rtx.h"
#include "rtx/OpMode_FM.hpp"
#include "rtx/OpMode_M17.hpp"
#ifdef CONFIG_TETRA
#include "rtx/OpMode_TETRA.hpp"
#endif
#ifdef CONFIG_DMR_RX
#include "rtx/OpMode_DMR_RX.hpp"
#endif
#ifdef CONFIG_AM
#include "rtx/OpMode_AM.hpp"
#endif
#ifdef CONFIG_ACARS
#include "rtx/OpMode_ACARS.hpp"
#endif
#ifdef CONFIG_AIS
#include "rtx/OpMode_AIS.hpp"
#endif
#ifdef CONFIG_FM_BROADCAST
#include "interfaces/delays.h"
#endif

static pthread_mutex_t   *cfgMutex;     // Mutex for incoming config messages
static const rtxStatus_t *newCnf;       // Pointer for incoming config messages
static rtxStatus_t        rtxStatus;    // RTX driver status
static rssi_t             rssi;         // Current RSSI in dBm
static bool               reinitFilter; // Flag for RSSI filter re-initialisation

static OpMode  *currMode;               // Pointer to currently active opMode handler
static OpMode     noMode;               // Empty opMode handler for opmode::NONE
static OpMode_FM  fmMode;               // FM mode handler
#ifdef CONFIG_M17
static OpMode_M17 m17Mode;              // M17 mode handler
#endif
#ifdef CONFIG_TETRA
static OpMode_TETRA tetraMode;          // TETRA mode handler
#endif
#ifdef CONFIG_DMR_RX
static OpMode_DMR_RX dmrRxMode;         // DMR receive mode handler
#endif
#ifdef CONFIG_AM
static OpMode_AM  amMode;               // AM receive mode handler
#endif
#ifdef CONFIG_ACARS
static OpMode_ACARS acarsMode;          // ACARS receive mode handler
#endif
#ifdef CONFIG_AIS
static OpMode_AIS aisMode;              // AIS receive mode handler
#endif

#ifdef CONFIG_SPECTRUM
/*
 * Spectrum sweep. The parameters and the last complete sweep are shared
 * with the UI under cfgMutex; the sweep itself runs in the RTX thread.
 */
static bool     specWanted;             // Requested by the UI
static freq_t   specStart;
static freq_t   specStep;
static size_t   specPoints;
static uint32_t specRange;              // Bumped at every new range
static uint32_t specSweeps;             // Complete sweeps of this range
static rssi_t   specDone[RTX_SPECTRUM_MAX_POINTS];
static rssi_t   specWork[RTX_SPECTRUM_MAX_POINTS];
static bool     specRunning;            // Sweeping, opMode suspended
#endif


void rtx_init(pthread_mutex_t *m)
{
    // Initialise mutex for configuration access
    cfgMutex = m;
    newCnf   = NULL;

    /*
     * Default initialisation for rtx status
     */
    rtxStatus.opMode        = OPMODE_NONE;
    rtxStatus.bandwidth     = BW_25;
    rtxStatus.txDisable     = 0;
    rtxStatus.opStatus      = OFF;
    rtxStatus.rxFrequency   = 430000000;
    rtxStatus.txFrequency   = 430000000;
    rtxStatus.txPower       = 0.0f;
    rtxStatus.sqlLevel      = 1;
    rtxStatus.rxToneEn      = 0;
    rtxStatus.rxTone        = 0;
    rtxStatus.txToneEn      = 0;
    rtxStatus.txTone        = 0;
    rtxStatus.invertRxPhase = false;
    rtxStatus.lsfOk         = false;
    rtxStatus.M17_src[0]    = '\0';
    rtxStatus.M17_dst[0]    = '\0';
    rtxStatus.M17_link[0]   = '\0';
    rtxStatus.M17_refl[0]   = '\0';
    rtxStatus.M17_meta_text[0] = '\0';
    currMode = &noMode;

    /*
     * Initialise low-level platform-specific driver
     */
    radio_init(&rtxStatus);
    radio_updateConfiguration();

    /*
     * Initial value for RSSI filter
     */
    rssi         = radio_getRssi();
    reinitFilter = false;
}

void rtx_terminate()
{
    rtxStatus.opStatus = OFF;
    rtxStatus.opMode   = OPMODE_NONE;
    currMode->disable();
    radio_terminate();
}

void rtx_configure(const rtxStatus_t *cfg)
{
    /*
     * NOTE: an incoming configuration may overwrite a preceding one not yet
     * read by the radio task. This mechanism ensures that the radio driver
     * always gets the most recent configuration.
     */

    pthread_mutex_lock(cfgMutex);
    newCnf = cfg;
    pthread_mutex_unlock(cfgMutex);
}

rtxStatus_t rtx_getCurrentStatus()
{
    return rtxStatus;
}

#ifdef CONFIG_SPECTRUM
void rtx_spectrumStart(freq_t start, freq_t step, size_t points)
{
    if(points > RTX_SPECTRUM_MAX_POINTS)
        points = RTX_SPECTRUM_MAX_POINTS;

    pthread_mutex_lock(cfgMutex);
    specStart  = start;
    specStep   = step;
    specPoints = points;
    specRange++;
    specSweeps = 0;
    specWanted = true;
    pthread_mutex_unlock(cfgMutex);
}

void rtx_spectrumStop()
{
    pthread_mutex_lock(cfgMutex);
    specWanted = false;
    pthread_mutex_unlock(cfgMutex);
}

uint32_t rtx_spectrumRead(rssi_t *out)
{
    pthread_mutex_lock(cfgMutex);
    uint32_t sweeps = specSweeps;
    if(sweeps > 0)
        memcpy(out, specDone, specPoints * sizeof(rssi_t));
    pthread_mutex_unlock(cfgMutex);

    return sweeps;
}

/**
 * \internal
 * Run the spectrum sweep, if requested, in place of the operating mode.
 *
 * @return true if the sweep owns the radio for this RTX task step.
 */
static bool spectrumTask()
{
    pthread_mutex_lock(cfgMutex);
    bool     wanted = specWanted;
    freq_t   start  = specStart;
    freq_t   step   = specStep;
    size_t   points = specPoints;
    uint32_t range  = specRange;
    pthread_mutex_unlock(cfgMutex);

    if(wanted && !specRunning)
    {
        // Suspend the operating mode: no audio, no transmission
        currMode->disable();
        rtxStatus.opStatus = OFF;
        radio_spectrumBegin();
        specRunning = true;
    }

    if(!wanted && specRunning)
    {
        // Resume it: the next update turns the receiver on again
        radio_spectrumEnd();
        specRunning = false;
        rtxStatus.opStatus = OFF;
        reinitFilter = true;
        currMode->enable();
        return false;
    }

    if(!specRunning)
        return false;

    for(size_t i = 0; i < points; i++)
    {
        // Give up on a sweep made obsolete by a new range, or a stop
        if((i % 8) == 0)
        {
            pthread_mutex_lock(cfgMutex);
            bool same = specWanted && (specRange == range);
            pthread_mutex_unlock(cfgMutex);
            if(!same)
                return true;
        }

        specWork[i] = radio_spectrumProbe(start + i * step);
    }

    pthread_mutex_lock(cfgMutex);
    if(specRange == range)
    {
        memcpy(specDone, specWork, points * sizeof(rssi_t));
        specSweeps++;
    }
    pthread_mutex_unlock(cfgMutex);

    return true;
}
#endif

#ifdef CONFIG_FM_BROADCAST
/*
 * Broadcast FM. The request and the receiver state are shared with the UI
 * under cfgMutex; the receiver is driven from the RTX thread.
 */
#define BC_LOW  87500000
#define BC_HIGH 108000000
#define BC_STEP 100000

static bool    bcWanted;                // Requested by the UI
static freq_t  bcFreq;                  // Wanted, then tuned while seeking
static uint8_t bcVolume;
static int8_t  bcSeek;                  // Seek direction, 0 none
static bool    bcRunning;               // Playing, opMode suspended
static freq_t  bcTuned;                 // Applied to the receiver
static uint8_t bcVolumeSet;
static unsigned bcSeekSteps;            // Channels tried in this seek
static unsigned bcPoll;                 // Steps since the last status read
static struct rtxBroadcast bcState;     // For the UI

void rtx_broadcastStart(freq_t freq, uint8_t volume)
{
    if(freq < BC_LOW)  freq = BC_LOW;
    if(freq > BC_HIGH) freq = BC_HIGH;

    pthread_mutex_lock(cfgMutex);
    bcFreq     = freq;
    bcVolume   = (volume > 15) ? 15 : volume;
    bcSeek     = 0;
    bcWanted   = true;
    pthread_mutex_unlock(cfgMutex);
}

void rtx_broadcastSeek(int8_t dir)
{
    pthread_mutex_lock(cfgMutex);
    bcSeek = dir;
    bcSeekSteps = 0;
    pthread_mutex_unlock(cfgMutex);
}

void rtx_broadcastStop()
{
    pthread_mutex_lock(cfgMutex);
    bcWanted = false;
    bcSeek   = 0;
    pthread_mutex_unlock(cfgMutex);
}

void rtx_broadcastRead(struct rtxBroadcast *out)
{
    pthread_mutex_lock(cfgMutex);
    *out = bcState;
    pthread_mutex_unlock(cfgMutex);
}

/**
 * \internal
 * Play broadcast FM, if requested, in place of the operating mode.
 *
 * @return true if broadcast FM owns the radio for this RTX task step.
 */
static bool broadcastTask()
{
    pthread_mutex_lock(cfgMutex);
    bool    wanted = bcWanted;
    freq_t  freq   = bcFreq;
    uint8_t volume = bcVolume;
    int8_t  seek   = bcSeek;
    pthread_mutex_unlock(cfgMutex);

    if(wanted && !bcRunning)
    {
        // Suspend the operating mode: no audio, no transmission
        currMode->disable();
        rtxStatus.opStatus = OFF;
        radio_broadcastOn(freq, volume);
        bcTuned     = freq;
        bcVolumeSet = volume;
        bcRunning   = true;
    }

    if(!wanted && bcRunning)
    {
        // Resume it: the next update turns the receiver on again
        radio_broadcastOff();
        bcRunning = false;
        rtxStatus.opStatus = OFF;
        reinitFilter = true;
        currMode->enable();
        return false;
    }

    if(!bcRunning)
        return false;

    if(volume != bcVolumeSet)
    {
        radio_broadcastVolume(volume);
        bcVolumeSet = volume;
    }

    if(seek != 0)
    {
        // Next channel, around the band; stop on a station or after a turn
        freq = bcTuned + seek * BC_STEP;
        if(freq > BC_HIGH) freq = BC_LOW;
        if(freq < BC_LOW)  freq = BC_HIGH;
        radio_broadcastTune(freq);
        bcTuned = freq;
        sleepFor(0, 60);

        uint8_t rssi, snr;
        bool station = radio_broadcastStatus(&rssi, &snr);
        pthread_mutex_lock(cfgMutex);
        bcFreq  = freq;
        if(station || (++bcSeekSteps > (BC_HIGH - BC_LOW) / BC_STEP))
            bcSeek = 0;
        bcState = { freq, rssi, snr, station, bcSeek };
        pthread_mutex_unlock(cfgMutex);
        return true;
    }

    bool retuned = (freq != bcTuned);
    if(retuned)
    {
        radio_broadcastTune(freq);
        bcTuned = freq;
    }

    // The signal once a second (soon after a retune): bus traffic near the
    // audio path is heard as clicks
    if(retuned || (bcPoll == 3) || (++bcPoll >= 10))
    {
        bcPoll = retuned ? 0 : 4;
        uint8_t rssi, snr;
        bool station = radio_broadcastStatus(&rssi, &snr);
        pthread_mutex_lock(cfgMutex);
        bcState = { bcTuned, rssi, snr, station, 0 };
        pthread_mutex_unlock(cfgMutex);
    }

    sleepFor(0, 100);
    return true;
}
#endif

void rtx_task()
{
    /*
     * A spectrum sweep suspends the operating mode. New configurations wait
     * for its end: they are applied when the mode resumes.
     */
    #ifdef CONFIG_FM_BROADCAST
    if(broadcastTask())
        return;
    #endif

    #ifdef CONFIG_SPECTRUM
    if(spectrumTask())
        return;
    #endif

    // Check if there is a pending new configuration and, in case, read it.
    bool reconfigure = false;
    if(pthread_mutex_trylock(cfgMutex) == 0)
    {
        if(newCnf != NULL)
        {
            // Copy new configuration and override opStatus flags
            uint8_t tmp = rtxStatus.opStatus;
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));
            rtxStatus.opStatus = tmp;

            reconfigure = true;
            newCnf = NULL;
        }

        pthread_mutex_unlock(cfgMutex);
    }

    if(reconfigure)
    {
        // Force TX and RX tone squelch to off for OpModes different from FM.
        if(rtxStatus.opMode != OPMODE_FM)
        {
            rtxStatus.txToneEn = 0;
            rtxStatus.rxToneEn = 0;
        }

        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        if(currMode->getID() != rtxStatus.opMode)
        {
            // Forward opMode change also to radio driver
            radio_setOpmode(static_cast< enum opmode >(rtxStatus.opMode));

            currMode->disable();
            rtxStatus.opStatus = OFF;

            switch(rtxStatus.opMode)
            {
                case OPMODE_NONE: currMode = &noMode;  break;
                case OPMODE_FM:   currMode = &fmMode;  break;
                #ifdef CONFIG_M17
                case OPMODE_M17:  currMode = &m17Mode; break;
                #endif
                #ifdef CONFIG_TETRA
                case OPMODE_TETRA: currMode = &tetraMode; break;
                #endif
                #ifdef CONFIG_DMR_RX
                case OPMODE_DMR:  currMode = &dmrRxMode; break;
                #endif
                #ifdef CONFIG_AM
                case OPMODE_AM:   currMode = &amMode; break;
                #endif
                #ifdef CONFIG_ACARS
                case OPMODE_ACARS: currMode = &acarsMode; break;
                #endif
                #ifdef CONFIG_AIS
                case OPMODE_AIS:  currMode = &aisMode; break;
                #endif
                default:   currMode = &noMode;
            }

            currMode->enable();
        }

        // Tell radio driver that there was a change in its configuration.
        radio_updateConfiguration();
    }

    /*
     * RSSI update block, run only when radio is in RX mode.
     *
     * RSSI value is passed through a filter with a time constant of 60ms
     * (cut-off frequency of 15Hz) at an update rate of 33.3Hz.
     *
     * The low pass filter skips an update step if a new configuration has
     * just been applied. This is a workaround for the AT1846S returning a
     * full-scale RSSI value immediately after one of its parameters changed,
     * thus causing the squelch to open briefly.
     *
     * Also, the RSSI filter is re-initialised every time radio stage is
     * switched back from TX/OFF to RX. This provides a workaround for some
     * radios reporting a full-scale RSSI value when transmitting.
     */
    if(rtxStatus.opStatus == RX)
    {

        if(!reconfigure)
        {
            if(!reinitFilter)
            {
                /*
                 * Filter RSSI value using 15.16 fixed point math. Equivalent
                 * floating point code is: rssi = 0.74*radio_getRssi() + 0.26*rssi
                 */
                int32_t filt_rssi = radio_getRssi() * 0xBD70    // 0.74 * radio_getRssi
                                  + rssi            * 0x428F;   // 0.26 * rssi
                rssi = (filt_rssi + 32768) >> 16;               // Round to nearest
            }
            else
            {
                rssi = radio_getRssi();
                reinitFilter = false;
            }
        }
    }
    else
    {
        // Reinit required if current operating status is TX or OFF
        reinitFilter = true;
    }

    /*
     * Forward the periodic update step to the currently active opMode handler.
     * Call is placed after RSSI update to allow handler's code have a fresh
     * version of the RSSI level.
     */
    currMode->update(&rtxStatus, reconfigure);
}

rssi_t rtx_getRssi()
{
    return rssi;
}

bool rtx_rxSquelchOpen()
{
    return currMode->rxSquelchOpen();
}
