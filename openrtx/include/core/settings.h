/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "hwconfig.h"
#include <stdbool.h>

typedef enum
{
    TIMER_OFF =  0,
    TIMER_5S  =  1,
    TIMER_10S =  2,
    TIMER_15S =  3,
    TIMER_20S =  4,
    TIMER_25S =  5,
    TIMER_30S =  6,
    TIMER_1M  =  7,
    TIMER_2M  =  8,
    TIMER_3M  =  9,
    TIMER_4M  = 10,
    TIMER_5M  = 11,
    TIMER_15M = 12,
    TIMER_30M = 13,
    TIMER_45M = 14,
    TIMER_1H  = 15
}
display_timer_t;

#ifdef CONFIG_BOOT_PIN
/* No code asked at power on (state_t.boot_pin) */
#define BOOT_PIN_OFF 0xFFFF
#endif

typedef struct
{
    uint8_t brightness;           // Display brightness
    uint8_t contrast;             // Display contrast
    uint8_t sqlLevel;             // Squelch level
    uint8_t voxLevel;             // Vox level
    int8_t  utc_timezone;         // Timezone, in units of half hours
    bool    gps_enabled;          // GPS active
    char    callsign[10];         // Plaintext callsign
    uint8_t display_timer   : 4,  // Standby timer
            m17_can         : 4;  // M17 CAN
    uint8_t vpLevel         : 3,  // Voice prompt level
            vpPhoneticSpell : 1,  // Phonetic spell enabled
            macroMenuLatch  : 1,  // Automatic latch of macro menu
            tetraRoam       : 1,  // TETRA roaming, from the first cell
            noRxWake        : 1,  // A signal received leaves the screen off
            tetraAfcOff     : 1;  // TETRA frequency correction off
    bool    m17_can_rx;           // Check M17 CAN on RX
    char    m17_dest[10];         // M17 destination
    bool    showBatteryIcon;      // Battery display true: icon, false: percentage
    uint8_t gpsSetTime      : 1,  // Use GPS to ajust RTC time
            tetraTonesOff   : 1;  // TETRA call tones off
    char    M17_meta_text[53];    // M17 Meta Text to send
}
__attribute__((packed)) settings_t;


static const settings_t default_settings =
{
    100,                          // Brightness
#ifdef CONFIG_SCREEN_CONTRAST
    CONFIG_DEFAULT_CONTRAST,      // Contrast
#else
    255,                          // Contrast
#endif
    4,                            // Squelch level, 4 = S3
    0,                            // Vox level
    0,                            // UTC Timezone
    false,                        // GPS enabled
    "N0CALL",                     // Default callsign
    TIMER_30S,                    // 30 seconds
    0,                            // M17 CAN
    0,                            // Voice prompts off
    0,                            // Phonetic spell off
    1,                            // Automatic latch of macro menu enabled
    0,                            // TETRA roaming off
    0,                            // A signal received wakes the screen
    0,                            // TETRA frequency correction on
    false,                        // Check M17 CAN on RX
    "",                           // Empty M17 destination
    false,                        // Display battery icon
    0,                            // Update RTC with GPS
    0,                            // TETRA call tones on
    "OpenRTX",                    // Default M17 meta text
};

#ifdef CONFIG_DMR_TX
/*
 * DMR transmission: own ID and what a call goes to. A record of its own,
 * apart from settings_t, so that radios without it keep their settings
 * layout and adding it kept the saved settings.
 */
typedef struct
{
    uint32_t id;            // Own DMR ID, 24 bits, 0 = not set (no TX)
    uint32_t tg;            // Talkgroup, or called ID of a private call
    uint8_t  cc;            // Colour code, 0..15
    uint8_t  slot;          // Timeslot, 1 or 2
    uint8_t  private_call;  // 1: private (unit) call to tg, 0: group call
    uint8_t  tx_invert;     // 1: transmitted polarity flipped from the
                            //    radio's default
    uint8_t  _reserved[8];  // For the repeater mode, 0 until then
}
__attribute__((packed)) dmrSettings_t;

#define DMR_ID_MAX 16777215u  // 24 bits

static const dmrSettings_t default_dmr_settings =
{
    0,                            // No own ID: set it before transmitting
    9,                            // Talkgroup 9, local
    1,                            // Colour code 1
    1,                            // Timeslot 1
    0,                            // Group call
    0,                            // The radio's default polarity
    { 0 },
};
#endif

#endif /* SETTINGS_H */
