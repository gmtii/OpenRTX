/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/cps_io.h"
#include "interfaces/delays.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ui/ui_default.h"
#include <string.h>
#include "ui/ui_strings.h"
#include "core/utils.h"
#include "ui/utils.h"
/*
 * Copies of large tables for the pages that show them, filled before each
 * use: in PSRAM on the C62.
 */
#ifdef PLATFORM_C62
#define UI_BSS __attribute__((section(".psram_section")))
#else
#define UI_BSS
#endif

#ifdef CONFIG_ACARS
#include "protocols/ACARS/Acars.h"
#endif
#ifdef CONFIG_AIS
#include "protocols/AIS/Ais.h"
#endif

#if defined(CONFIG_TETRA) || defined(CONFIG_SPECTRUM) || defined(CONFIG_ACARS) \
    || defined(CONFIG_AIS) || defined(CONFIG_FM_BROADCAST)
/* Secondary text of the information pages: color_grey is too dark on black */
static const color_t info_grey = { 160, 160, 160, 255 };
#endif

#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraCell.h"
#include "protocols/TETRA/TetraHeard.h"
#include "protocols/TETRA/TetraScan.h"
#include "protocols/TETRA/TetraSds.h"
#include "protocols/TETRA/TetraSpeech.h"
#include "protocols/TETRA/TetraDemod.h"
#include <math.h>

/* An SDS text stays on the main screen this long */
#define TETRA_SDS_SHOW_MS 30000

/**
 * Print an SSI, or a dash if it is not known.
 */
static void _ui_printSsi(point_t pos, fontSize_t font, const char *prefix,
                         uint32_t ssi)
{
    if (ssi != 0)
        gfx_print(pos, font, TEXT_ALIGN_CENTER, color_white, "%s%lu", prefix,
                  (unsigned long)ssi);
    else
        gfx_print(pos, font, TEXT_ALIGN_CENTER, color_white, "%s-", prefix);
}

/**
 * Check whether a TETRA call takes over the main screen: only one whose
 * signalling told something about it. Traffic of a call never signalled in
 * clear (air encrypted, mostly) has nothing to show but its timeslot.
 */
static bool _ui_tetraCallShown(const struct tetraStatus *ts)
{
    bool known = (ts->callDst != 0) || (ts->talker != 0)
              || (ts->callId != 0) || !ts->callUnknown;
    return ts->sync && ts->call && known;
}

/**
 * TETRA cell or call information, from line 1 to line 4 during a call,
 * on line 2 otherwise.
 */
static void _ui_drawTetraInfo(void)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);

    if (_ui_tetraCallShown(&ts)) {
        // Talking party
        gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size,
                       TEXT_ALIGN_LEFT, color_white, SYMBOL_CALL_MADE);
        _ui_printSsi(layout.line1_pos, layout.line2_font, "", ts.talker);

        // Called group or individual
        gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size,
                       TEXT_ALIGN_LEFT, color_white, SYMBOL_CALL_RECEIVED);
        _ui_printSsi(layout.line2_pos, layout.line2_font,
                     ts.callGroup ? "G " : "", ts.callDst);

        // Cell
        gfx_drawSymbol(layout.line3_pos, layout.line3_symbol_size,
                       TEXT_ALIGN_LEFT, color_white,
                       SYMBOL_TRANSMISSION_TOWER);
        // Or the carrier the call is followed to
        if (ts.followHz != 0)
            gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                      yellow_fab413, "%u-%u carrier %u", ts.mcc, ts.mnc,
                      ts.followCarrier);
        else
            gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                      color_white, "%u-%u LA %u", ts.mcc, ts.mnc, ts.la);

        // Call state
        if (ts.callEncrypted) {
            gfx_drawSymbol(layout.line4_pos, layout.line4_symbol_size,
                           TEXT_ALIGN_LEFT, color_white, SYMBOL_LOCK);
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "Encrypted");
        } else if (ts.callUnknown) {
            // No clear setup for this traffic: likely air encrypted
            gfx_drawSymbol(layout.line4_pos, layout.line4_symbol_size,
                           TEXT_ALIGN_LEFT, color_white, SYMBOL_LOCK);
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "Encrypted?");
        } else if (ts.callId != 0) {
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "%s, call %u",
                      ts.voice ? "Voice" : "Idle", ts.callId);
        } else {
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "%s", ts.voice ? "Voice" : "Idle");
        }
        return;
    }

    if (!ts.sync) {
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "T searching");
        return;
    }

    gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size,
                   TEXT_ALIGN_LEFT, color_white, SYMBOL_TRANSMISSION_TOWER);
    if (ts.sysinfo)
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "T %u-%u LA %u%s", ts.mcc, ts.mnc, ts.la,
                  ts.airEncryption ? " enc" : "");
    else
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "T %u-%u CC %u", ts.mcc, ts.mnc,
                  ts.colourCode);
}

/**
 * TETRA line 1 in VFO mode, when no call is shown: the last SDS (scrolling)
 * or the demodulator quality.
 */
static void _ui_drawTetraLine1(ui_state_t *ui_state)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);

    if (_ui_tetraCallShown(&ts))
        return;

    long long now = getTick();
    if (ts.sdsSrc != 0 && now - ts.sdsTime < TETRA_SDS_SHOW_MS) {
        char text[96];
        char msg[14];
        snprintf(text, sizeof(text), "%lu: %s   ", (unsigned long)ts.sdsSrc,
                 ts.sds);
        scrollTextPeek(text, msg, sizeof(msg),
                       ui_state->m17_meta_text_scroll_position);
        if (now - ui_state->m17_meta_text_last_scroll_tick >= 100) {
            scrollTextAdvance(text, sizeof(msg),
                              &ui_state->m17_meta_text_scroll_position);
            ui_state->m17_meta_text_last_scroll_tick = now;
        }
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
                  color_white, "%s", msg);
        return;
    }

    // Phase error, carrier offset, lost sample blocks, resyncs
    ui_state->m17_meta_text_scroll_position = 0;
    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, "e%u%% %+dHz o%u r%u", ts.phaseErr, ts.carrierHz,
              ts.overruns, ts.resyncs);
}
#endif

#ifdef CONFIG_DMR_RX
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_voice.h"
#include "protocols/DMR/dmr_track.h"

/* A DMR call stays on the main screen this long after its last burst */
#define DMR_CALL_SHOW_MS 1500

static bool _ui_dmrLastFix(char *buf, size_t len);

/**
 * Pick the DMR slot whose call takes over the main screen: the one being
 * heard, else the most recent voice call.
 *
 * @param slot: the slot information, filled in.
 * @param ds: the demodulator status, filled in.
 * @return the slot index, or -1 if no call is shown.
 */
static int _ui_dmrCallSlot(dmr_slot_info_t *slot, dmr_demod_status_t *ds)
{
    dmr_demod_get_status(ds);
    if (!ds->locked)
        return -1;

    int playing = dmr_voice_get_playing();
    int best = -1;
    uint32_t bestAge = DMR_CALL_SHOW_MS;
    for (int i = 0; i < 2; i++) {
        dmr_slot_info_t si;
        dmr_proto_get_slot(i, &si);
        uint32_t age = ds->now_ms - si.last_ms;
        if (si.activity != DMR_ACT_VOICE || !si.ids_valid
            || age >= DMR_CALL_SHOW_MS)
            continue;
        if (i == playing || age < bestAge) {
            best = i;
            bestAge = age;
            *slot = si;
            if (i == playing)
                break;
        }
    }
    return best;
}

/**
 * DMR channel or call information, from line 1 to line 4 during a call,
 * on line 2 otherwise.
 */
static void _ui_drawDmrInfo(void)
{
    dmr_slot_info_t si;
    dmr_demod_status_t ds;
    int slot = _ui_dmrCallSlot(&si, &ds);
    bool ms = dmr_proto_is_ms_mode();

    if (slot >= 0) {
        // Talking party: alias, else radio ID
        gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size,
                       TEXT_ALIGN_LEFT, color_white, SYMBOL_CALL_MADE);
        if (si.alias[0] != '\0')
            gfx_print(layout.line1_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                      color_white, "%.12s", si.alias);
        else
            gfx_print(layout.line1_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                      color_white, "%lu", (unsigned long)si.src);

        // Talkgroup or called radio
        gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size,
                       TEXT_ALIGN_LEFT, color_white, SYMBOL_CALL_RECEIVED);
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "%s%lu", si.group ? "TG " : "",
                  (unsigned long)si.dst);

        // Source ID when the alias took line 1, then slot and colour code
        if (si.alias[0] != '\0')
            gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                      color_white, "%lu %s%d", (unsigned long)si.src,
                      ms ? "MS" : "TS", ms ? si.cc : slot + 1);
        else if (ms)
            gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                      color_white, "MS CC%d", si.cc);
        else
            gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                      color_white, "TS%d CC%d", slot + 1, si.cc);

        // Call state
        if (si.encrypted) {
            gfx_drawSymbol(layout.line4_pos, layout.line4_symbol_size,
                           TEXT_ALIGN_LEFT, color_white, SYMBOL_LOCK);
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "Encrypted");
        } else if (!dmr_voice_available()) {
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "Voice (no vocoder)");
        } else if (dmr_voice_get_playing() == slot) {
            uint32_t frames, avgUs;
            dmr_voice_get_timing(&frames, &avgUs);
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "Voice %lu.%lums", (unsigned long)(avgUs / 1000),
                      (unsigned long)((avgUs % 1000) / 100));
        } else {
            gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                      color_white, "Voice");
        }
        return;
    }

    if (!ds.locked) {
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "DMR searching");
        return;
    }

    // Colour code of the slot heard last
    int cc = -1;
    for (int i = 0; i < 2 && cc < 0; i++) {
        dmr_proto_get_slot(i, &si);
        cc = si.cc;
    }
    gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size,
                   TEXT_ALIGN_LEFT, color_white, SYMBOL_TRANSMISSION_TOWER);
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
              color_white, "DMR %s CC%d", ds.bs ? "RPT" : (ms ? "MS" : "DM"),
              cc);

    // The latest LRRP position, under the frequency
    char fix[40];
    if(_ui_dmrLastFix(fix, sizeof(fix)))
        gfx_print(layout.line4_pos, layout.line4_font, TEXT_ALIGN_CENTER,
                  color_white, "%s", fix);
}

/**
 * DMR line 1 in VFO mode, when no call is shown: the demodulator figures.
 */
static void _ui_drawDmrLine1(void)
{
    dmr_slot_info_t si;
    dmr_demod_status_t ds;
    if (_ui_dmrCallSlot(&si, &ds) >= 0)
        return;

    // Outer symbol level, DC offset (input units), lost sample blocks,
    // polarity
    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, "L%d %+d o%lu%s", (int)ds.level, (int)ds.dc,
              (unsigned long)ds.overruns, ds.inverted ? " inv" : "");
}
#endif

/**
 * Check whether the current mode shows a call instead of the frequency.
 */
static bool _ui_callShown(void)
{
#ifdef CONFIG_M17
    rtxStatus_t status = rtx_getCurrentStatus();
    if ((status.opMode == OPMODE_M17) && status.lsfOk)
        return true;
#endif
#ifdef CONFIG_TETRA
    if (last_state.channel.mode == OPMODE_TETRA) {
        struct tetraStatus ts;
        tetra_getStatus(&ts);
        return _ui_tetraCallShown(&ts);
    }
#endif
#ifdef CONFIG_DMR_RX
    if (last_state.channel.mode == OPMODE_DMR) {
        dmr_slot_info_t si;
        dmr_demod_status_t ds;
        return _ui_dmrCallSlot(&si, &ds) >= 0;
    }
#endif
    return false;
}

void _ui_drawMainBackground()
{
    // Print top bar line of hline_h pixel height
    gfx_drawHLine(layout.top_h, layout.hline_h, color_grey);
    // Print bottom bar line of 1 pixel height
    gfx_drawHLine(CONFIG_SCREEN_HEIGHT - layout.bottom_h - 1, layout.hline_h, color_grey);
}

void _ui_drawMainTop(ui_state_t * ui_state)
{
#ifdef CONFIG_RTC
    // Print clock on top bar
    datetime_t local_time = utcToLocalTime(last_state.time,
                                           last_state.settings.utc_timezone);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER,
              color_white, "%02d:%02d:%02d", local_time.hour,
              local_time.minute, local_time.second);
#endif
    // If the radio has no built-in battery, print input voltage
#ifdef CONFIG_BAT_NONE
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_RIGHT,
              color_white,"%u.%uV", last_state.v_bat/1000, (last_state.v_bat % 1000) /100);
#else
    if(last_state.settings.showBatteryIcon) {
        // print battery icon on top bar, use 4 px padding
        uint16_t bat_width = CONFIG_SCREEN_WIDTH / 9;
        uint16_t bat_height = layout.top_h - (layout.status_v_pad * 2);
        point_t bat_pos = {CONFIG_SCREEN_WIDTH - bat_width - layout.horizontal_pad,
                        layout.status_v_pad};
        gfx_drawBattery(bat_pos, bat_width, bat_height, last_state.charge);
    } else {
        // print the battery percentage
        point_t bat_pos = {layout.top_pos.x, layout.top_pos.y - 2};
        gfx_print(bat_pos , FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
        color_white,"%d%%", last_state.charge);
    }
#endif
    if (ui_state->input_locked == true)
      gfx_drawSymbol(layout.top_pos, layout.top_symbol_size, TEXT_ALIGN_LEFT,
                     color_white, SYMBOL_LOCK);
}

void _ui_drawBankChannel()
{
    // Print Bank number, channel number and Channel name
    uint16_t b = (last_state.bank_enabled) ? last_state.bank : 0;
    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, "%01d-%03d: %.12s",
              b, last_state.channel_index + 1, last_state.channel.name);
}

const char* _ui_getToneEnabledString(bool tone_tx_enable, bool tone_rx_enable,
        bool use_abbreviation)
{
    const char *strings[2][4] = {
        {
            currentLanguage->None,
            currentLanguage->Encode,
            currentLanguage->Decode,
            currentLanguage->Both,
        }, {
            "N",
            "T",
            "R",
            "B"
        }
    };


    uint8_t index = (tone_rx_enable << 1) | (tone_tx_enable);
    return strings[use_abbreviation][index];
}

void _ui_drawModeInfo(ui_state_t* ui_state)
{
    char bw_str[8] = { 0 };

    switch(last_state.channel.mode)
    {
        case OPMODE_FM:

            // Get Bandwidth string
            if(last_state.channel.bandwidth == BW_12_5)
                sniprintf(bw_str, 8, "NFM");
            else if(last_state.channel.bandwidth == BW_25)
                sniprintf(bw_str, 8, "FM");

            // Get encdec string
            bool tone_tx_enable = last_state.channel.fm.txToneEn;
            bool tone_rx_enable = last_state.channel.fm.rxToneEn;
            // Print Bandwidth, Tone and encdec info
            if (tone_tx_enable || tone_rx_enable)
            {
                uint16_t tone = ctcss_tone[last_state.channel.fm.txTone];
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s %d.%d %s", bw_str, (tone / 10),
                          (tone % 10), _ui_getToneEnabledString(tone_tx_enable, tone_rx_enable, true));
            }
            else
            {
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", bw_str );
            }
            break;

        case OPMODE_DMR:
            #ifdef CONFIG_DMR_RX
            _ui_drawDmrInfo();
            #else
            // Print talkgroup
            gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                    color_white, "DMR TG%s", "");
            #endif
            break;

        #ifdef CONFIG_M17
        case OPMODE_M17:
        {
            // Print M17 Destination ID on line 3 of 3
            rtxStatus_t rtxStatus = rtx_getCurrentStatus();

            if(rtxStatus.lsfOk)
            {
                // Destination address
                gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_RECEIVED);

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_dst);

                // Source address
                gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_MADE);

                gfx_print(layout.line1_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_src);

                // RF link (if present)
                if(rtxStatus.M17_link[0] != '\0')
                {
                    gfx_drawSymbol(layout.line4_pos, layout.line3_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_ACCESS_POINT);

                    gfx_print(layout.line4_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_link);
                }

                // Meta text (if present)
                if(rtxStatus.M17_meta_text[0] != '\0')
                {
                    char msg[14];
                    
                    // Always display current position
                    scrollTextPeek(rtxStatus.M17_meta_text, msg, sizeof(msg),
                                   ui_state->m17_meta_text_scroll_position);
                    // Only advance scroll every 100ms
                    long long now = getTick();
                    if(now - ui_state->m17_meta_text_last_scroll_tick >= 100)
                    {
                        scrollTextAdvance(rtxStatus.M17_meta_text, sizeof(msg),
                                          &ui_state->m17_meta_text_scroll_position);
                        ui_state->m17_meta_text_last_scroll_tick = now;
                    }
                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", msg);
                }
                // Reflector (if present)
                else if(rtxStatus.M17_refl[0] != '\0')
                {                    
                    gfx_drawSymbol(layout.line3_pos, layout.line4_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_NETWORK);

                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_refl);
                }
                
                // Reset scroll position when meta text becomes empty
                if(rtxStatus.M17_meta_text[0] == '\0')
                {
                    ui_state->m17_meta_text_scroll_position = 0;
                }
            }
            else
            {
                const char *dst = NULL;
                if(ui_state->edit_mode)
                {
                    dst = ui_state->new_callsign;
                }
                else
                {
                    if(strnlen(rtxStatus.destination_address, 10) == 0)
                        dst = currentLanguage->broadcast;
                    else
                        dst = rtxStatus.destination_address;
                }

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "M17 #%s", dst);
            }
            break;
        }
        #endif

        #ifdef CONFIG_TETRA
        case OPMODE_TETRA:
            (void) ui_state;
            _ui_drawTetraInfo();
            break;
        #endif

        #ifdef CONFIG_AM
        case OPMODE_AM:
            gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                      color_white, "AM");
            break;
        #endif

        #ifdef CONFIG_ACARS
        case OPMODE_ACARS:
            gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                      color_white, "ACARS");
            break;
        #endif

        #ifdef CONFIG_AIS
        case OPMODE_AIS:
            gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                      color_white, "AIS");
            break;
        #endif
    }
}

void _ui_drawFrequency()
{
    freq_t freq = platform_getPttStatus() ? last_state.channel.tx_frequency
                                          : last_state.channel.rx_frequency;

    char freq_str[16] = {0};
    sniprintf(freq_str, sizeof(freq_str), "%03lu.%05lu",
              (freq / 1000000lu), (freq % 1000000lu) / 10);

    size_t len = strlen(freq_str);
    char main_str[16] = {0};
    char small_str[3] = {0};
    strncpy(main_str, freq_str, len - 2);
    strncpy(small_str, freq_str + len - 2, 2);

    fontSize_t small_font = FONT_SIZE_5PT;
    if (layout.line3_large_font > FONT_SIZE_6PT)
    {
        small_font = (fontSize_t)(layout.line3_large_font - 2);
    }

    uint16_t main_width = gfx_getTextWidth(layout.line3_large_font, main_str);
    uint16_t small_width = gfx_getTextWidth(small_font, small_str);
    uint16_t total_width = main_width + small_width;
    int16_t start_x = (CONFIG_SCREEN_WIDTH - total_width) / 2;

    point_t main_pos = { (uint16_t)start_x, layout.line3_large_pos.y };
    gfx_print(main_pos, layout.line3_large_font, TEXT_ALIGN_LEFT,
              color_white, "%s", main_str);

    point_t small_pos = { (uint16_t)(start_x + main_width), layout.line3_large_pos.y };
    gfx_print(small_pos, small_font, TEXT_ALIGN_LEFT,
              color_white, "%s", small_str);
}

void _ui_drawVFOMiddleInput(ui_state_t* ui_state)
{
    // Add inserted number to string, skipping "Rx: "/"Tx: " and "."
    uint8_t insert_pos = ui_state->input_position + 3;
    if(ui_state->input_position > 3) insert_pos += 1;
    char input_char = ui_state->input_number + '0';

    if(ui_state->input_set == SET_RX)
    {
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Rx:%03lu.%04lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        }
        else
        {
            // Replace Rx frequency with underscorses
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_rx_freq_buf, ">Rx:___.____");
            ui_state->new_rx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ui_state->new_rx_freq_buf);
        }
        gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Tx:%03lu.%04lu",
                  (unsigned long)last_state.channel.tx_frequency/1000000,
                  (unsigned long)(last_state.channel.tx_frequency%1000000)/100);
    }
    else if(ui_state->input_set == SET_TX)
    {
        gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Rx:%03lu.%04lu",
                  (unsigned long)ui_state->new_rx_frequency/1000000,
                  (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        // Replace Rx frequency with underscorses
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Tx:%03lu.%04lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        }
        else
        {
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_tx_freq_buf, ">Tx:___.____");
            ui_state->new_tx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ui_state->new_tx_freq_buf);
        }
    }
}

void _ui_drawMainBottom()
{
    // Squelch bar
    rssi_t   rssi = last_state.rssi;
    uint8_t  squelch = last_state.settings.sqlLevel;
    uint8_t  volume = last_state.volume;
    uint16_t meter_width = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    uint16_t meter_height = layout.bottom_h;
    point_t meter_pos = { layout.horizontal_pad,
                          CONFIG_SCREEN_HEIGHT - meter_height - layout.bottom_pad};
    uint8_t mic_level = platform_getMicLevel();
    switch(last_state.channel.mode)
    {
        case OPMODE_FM:
        #ifdef CONFIG_AM
        case OPMODE_AM:
        #endif
        #ifdef CONFIG_ACARS
        case OPMODE_ACARS:
        #endif
        #ifdef CONFIG_AIS
        case OPMODE_AIS:
        #endif
            gfx_drawSmeter(meter_pos,
                           meter_width,
                           meter_height,
                           rssi,
                           squelch,
                           volume,
                           true,
                           yellow_fab413);
            break;
        case OPMODE_DMR:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #ifdef CONFIG_M17
        case OPMODE_M17:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #endif
        #ifdef CONFIG_TETRA
        case OPMODE_TETRA:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #endif
    }
}

#ifdef CONFIG_TETRA
/**
 * TETRA diagnostics page, in place of the VFO screen's middle: demodulator,
 * synchronisation, calls and speech figures.
 */
static void _ui_drawTetraDebug(void)
{
    struct tetraStatus ts;
    struct tetraSpeechStats ss;
    tetra_getStatus(&ts);
    tetraSpeech_getStats(&ss);

    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = gfx_getFontHeight(font) + 2;
    point_t pos = { layout.horizontal_pad, layout.top_h + step };

    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413,
              "Sync %s  e%u%%  %+dHz", ts.sync ? "yes" : "no", ts.phaseErr,
              ts.carrierHz);
    pos.y += step;
    if (ts.followHz != 0)
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413,
                  "Lost o%u r%u  carrier %u", ts.overruns, ts.resyncs,
                  ts.followCarrier);
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
                  "Lost o%u  resync r%u", ts.overruns, ts.resyncs);
    pos.y += step;
    if (ts.cell)
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
                  "Cell %u-%u LA %u%s", ts.mcc, ts.mnc, ts.la,
                  ts.airEncryption ? " enc" : "");
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "No cell");
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
              "Calls %u  last %u um%u ts%x", ts.knownCalls, ts.lastCallId,
              ts.lastCallMarker, ts.lastCallSlots);
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
              "Traffic um%u tn%u  slots %lu", ts.lastTrafficMarker,
              ts.lastTrafficTn, (unsigned long)ss.slots);
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
              "Skip u%lu e%lu d%lu f%lu 2nd %lu",
              (unsigned long)ss.unknown, (unsigned long)ss.encrypted,
              (unsigned long)ss.data, (unsigned long)ss.filtered,
              (unsigned long)ss.other);
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
              "Frames %lu bad %lu", (unsigned long)ss.queued,
              (unsigned long)ss.bad);
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
              "Played %lu  spk busy %lu", (unsigned long)ss.played,
              (unsigned long)ss.noPath);
}
#endif

#ifdef CONFIG_TETRA
/**
 * TETRA short data page: one message of the history, UP/DOWN to browse.
 */
static void _ui_drawTetraSds(ui_state_t *ui_state)
{
    static const char *const kinds[] = { "Status", "Short data", "Text",
                                         "Location", "Report", "Encrypted",
                                         "Data" };
    static UI_BSS struct tetraSds list[TETRA_SDS_MAX];
    int n = tetraSds_get(list, TETRA_SDS_MAX);

    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = gfx_getFontHeight(font) + 2;
    point_t pos = { layout.horizontal_pad, layout.top_h + step };

    if (n == 0) {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "SDS: none yet");
        return;
    }
    if (ui_state->tetra_sds_sel >= n)
        ui_state->tetra_sds_sel = n - 1;
    const struct tetraSds *m = &list[ui_state->tetra_sds_sel];

    // Position in the list and age
    long age = (long)((getTick() - m->tick) / 1000);
    char ago[24];
    if (age < 60)
        snprintf(ago, sizeof(ago), "%lds", age);
    else if (age < 3600)
        snprintf(ago, sizeof(ago), "%ldm", age / 60);
    else
        snprintf(ago, sizeof(ago), "%ldh", age / 3600);
    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "SDS %d/%d",
              ui_state->tetra_sds_sel + 1, n);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, yellow_fab413, "%s ago", ago);

    // Sender, destination, kind
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%lu > %lu",
              (unsigned long)m->from, (unsigned long)m->to);
    pos.y += step;
    const char *kind = (m->kind < sizeof(kinds) / sizeof(kinds[0]))
                     ? kinds[m->kind] : "?";
    if (m->parts > 0)
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%s, %u/%u parts",
                  kind, m->partsGot, m->parts);
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%s", kind);

    // Message, wrapped over the rest of the screen
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", m->text);
}
#endif

#ifdef CONFIG_UI_RICH_MAIN
static void _ui_drawRichMain(ui_state_t *ui_state, bool mem);
#ifdef CONFIG_ACARS
static bool _ui_drawAcarsPage(ui_state_t *ui_state);
#endif
#ifdef CONFIG_AIS
static bool _ui_drawAisPage(ui_state_t *ui_state);
#endif
#endif

#if defined(CONFIG_TETRA) || defined(CONFIG_ACARS) || defined(CONFIG_AIS) \
    || defined(CONFIG_DMR_RX)
/**
 * Time since a tick: "12s", "5m", "2h".
 */
static void _ui_ago(char *buf, size_t len, long long tick)
{
    long age = (long)((getTick() - tick) / 1000);
    if(age < 60)
        sniprintf(buf, len, "%lds", age);
    else if(age < 3600)
        sniprintf(buf, len, "%ldm", age / 60);
    else
        sniprintf(buf, len, "%ldh", age / 3600);
}
#endif

#ifdef CONFIG_DMR_RX
/**
 * A latitude or longitude in degrees, four decimals and a hemisphere
 * letter: "28.1234N". No float formatting in the printf used.
 */
static void _ui_dmrCoord(char *buf, size_t len, float v, char pos, char neg)
{
    long a = (long)(v * 10000.0f + ((v < 0) ? -0.5f : 0.5f));
    char h = (a < 0) ? neg : pos;
    if(a < 0)
        a = -a;
    sniprintf(buf, len, "%ld.%04ld%c", a / 10000, a % 10000, h);
}

/**
 * DMR station page, in place of the VFO or memory screen: one station heard
 * with an LRRP position, whole, UP and DOWN to go through them, the latest
 * fix first.
 *
 * @return true if it was drawn.
 */
static bool _ui_drawDmrPage(ui_state_t *ui_state)
{
    if((last_state.channel.mode != OPMODE_DMR) || !ui_state->dmr_page)
        return false;

    static UI_BSS dmr_station_t list[DMR_TRACK_MAX];
    int n = dmr_track_get(list, DMR_TRACK_MAX);
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    point_t pos = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };

    _ui_drawMainTop(ui_state);
    if(n == 0)
    {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "LRRP");
        pos.y += 2 * step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "No position yet");
        return true;
    }
    if(ui_state->dmr_sel >= n)
        ui_state->dmr_sel = n - 1;
    const dmr_station_t *s = &list[ui_state->dmr_sel];

    char ago[8], grid[7], lat[16], lon[16];
    _ui_ago(ago, sizeof(ago), s->tick);
    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "LRRP %d/%d",
              ui_state->dmr_sel + 1, n);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, yellow_fab413, "%s ago", ago);

    // Name, else the radio ID alone
    pos.y += step;
    if(s->alias[0] != '\0')
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%.20s", s->alias);
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%lu",
                  (unsigned long)s->src);

    pos.y += step;
    dmr_latlon_to_grid(s->lat, s->lon, grid);
    if(s->alias[0] != '\0')
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%lu",
                  (unsigned long)s->src);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, yellow_fab413, "%s", grid);

    pos.y += step;
    _ui_dmrCoord(lat, sizeof(lat), s->lat, 'N', 'S');
    _ui_dmrCoord(lon, sizeof(lon), s->lon, 'E', 'W');
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s %s", lat, lon);

    if(s->speed_kmh >= 0 || s->dir >= 0)
    {
        char buf[24];
        size_t o = 0;
        buf[0] = '\0';
        if(s->speed_kmh >= 0)
            o += sniprintf(buf, sizeof(buf), "%d km/h ", s->speed_kmh);
        if(s->dir >= 0 && o < sizeof(buf))
            sniprintf(buf + o, sizeof(buf) - o, "dir %03d", s->dir);
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", buf);
    }

    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%u report%s",
              s->reports, (s->reports == 1) ? "" : "s");
    return true;
}

/**
 * The latest LRRP fix in one line: "2145678 IL18qd 3m", the alias in place
 * of the ID when there is one.
 *
 * @return false if no position was heard.
 */
static bool _ui_dmrLastFix(char *buf, size_t len)
{
    dmr_station_t s;
    if(dmr_track_get(&s, 1) == 0)
        return false;

    char grid[7], ago[8];
    dmr_latlon_to_grid(s.lat, s.lon, grid);
    _ui_ago(ago, sizeof(ago), s.tick);
    if(s.alias[0] != '\0')
        sniprintf(buf, len, "%.10s %s %s", s.alias, grid, ago);
    else
        sniprintf(buf, len, "%lu %s %s", (unsigned long)s.src, grid, ago);
    return true;
}
#endif

#ifdef CONFIG_TETRA
/* Rows of the groups and radios pages */
#define HEARD_ROWS 6

/**
 * Header and rows of a list page; the picked row highlighted, the list
 * scrolled to keep it in sight.
 *
 * @return the y of the detail lines under the list.
 */
static int16_t _ui_heardList(ui_state_t *ui_state, const char *title, int n,
                             bool (*row)(int i, char *l, char *m, char *r,
                                         size_t len))
{
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    int16_t y = layout.top_h + 3;
    point_t pos = { layout.horizontal_pad, (int16_t)(y + 8) };

    if(ui_state->tetra_sds_sel >= n && n > 0)
        ui_state->tetra_sds_sel = n - 1;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "%s %d", title, n);
    if(n == 0)
    {
        pos.y += 2 * step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "None heard yet");
        return CONFIG_SCREEN_HEIGHT;
    }

    int first = 0;
    if(ui_state->tetra_sds_sel >= HEARD_ROWS)
        first = ui_state->tetra_sds_sel - HEARD_ROWS + 1;
    for(int i = first; i < n && i < first + HEARD_ROWS; i++)
    {
        // A marked row (the filtered group) in green
        static const color_t marked = { 63, 191, 95, 255 };
        char l[16], m[16], r[8];
        bool mark = row(i, l, m, r, sizeof(l));
        int16_t ry = y + 3 + step * (i - first + 1);
        color_t c = mark ? marked : color_white;
        if(i == ui_state->tetra_sds_sel)
        {
            gfx_drawRect((point_t){ 0, (int16_t)(ry - 1) },
                         CONFIG_SCREEN_WIDTH, step, c, true);
            c = color_black;
        }
        point_t p = { layout.horizontal_pad, (int16_t)(ry + 8) };
        gfx_print(p, font, TEXT_ALIGN_LEFT, c, "%s", l);
        p.x = 66;
        gfx_print(p, font, TEXT_ALIGN_LEFT, c, "%s", m);
        p.x = layout.horizontal_pad;
        gfx_print(p, font, TEXT_ALIGN_RIGHT, c, "%s", r);
    }

    int16_t dy = y + 3 + step * (HEARD_ROWS + 1) + 2;
    gfx_drawHLine(dy - 2, 1, info_grey);
    return dy;
}

static UI_BSS struct tetraGroup heardGroups[TETRA_HEARD_GROUPS];
static UI_BSS struct tetraCallInfo heardCalls[TETRA_HEARD_CALLS];
static UI_BSS struct tetraRadio heardRadios[TETRA_HEARD_RADIOS];

static const char *const radioActivity[] = { "call", "talk", "SDS", "reg" };

static bool _ui_groupRow(int i, char *l, char *m, char *r, size_t len)
{
    const struct tetraGroup *g = &heardGroups[i];
    sniprintf(l, len, "G %lu", (unsigned long)g->gssi);
    sniprintf(m, len, "%uc %ut%s", g->calls, g->tx, g->encrypted ? " enc" : "");
    _ui_ago(r, 8, g->last);
    return g->gssi == tetraStatus_filter();
}

/**
 * The group of a row of the groups page, as last drawn.
 *
 * @return its GSSI, 0 if there is no such row.
 */
uint32_t _ui_tetraGroupAt(int i)
{
    if(i < 0 || i >= TETRA_HEARD_GROUPS)
        return 0;
    return heardGroups[i].gssi;
}

static bool _ui_radioRow(int i, char *l, char *m, char *r, size_t len)
{
    const struct tetraRadio *rd = &heardRadios[i];
    sniprintf(l, len, "%lu", (unsigned long)rd->issi);
    if(rd->lastGroup != 0)
        sniprintf(m, len, "G%lu", (unsigned long)rd->lastGroup);
    else
        sniprintf(m, len, "%s", radioActivity[rd->activity & 3]);
    _ui_ago(r, 8, rd->last);
    return false;
}

static bool _ui_callRow(int i, char *l, char *m, char *r, size_t len)
{
    const struct tetraCallInfo *c = &heardCalls[i];
    if(c->dst != 0)
        sniprintf(l, len, "%s %lu", c->group ? "G" : "I",
                  (unsigned long)c->dst);
    else
        sniprintf(l, len, "Call %u", c->id);
    if(c->talker != 0)
        sniprintf(m, len, "%lu", (unsigned long)c->talker);
    else
        m[0] = '\0';
    if(c->active)
        sniprintf(r, 8, "now");
    else
        _ui_ago(r, 8, c->last);
    return (c->dst != 0) && (c->dst == tetraStatus_filter());
}

/**
 * TETRA calls page: the calls heard, then the picked one's state, length
 * and talkers.
 */
static void _ui_drawTetraCalls(ui_state_t *ui_state)
{
    int n = tetraHeard_calls(heardCalls, TETRA_HEARD_CALLS);
    int16_t y = _ui_heardList(ui_state, "Calls", n, _ui_callRow);
    if(n == 0)
        return;

    const struct tetraCallInfo *c = &heardCalls[ui_state->tetra_sds_sel];
    const fontSize_t font = FONT_SIZE_6PT;
    point_t pos = { layout.horizontal_pad, (int16_t)(y + 8) };
    long secs = (long)((c->last - c->start) / 1000);
    gfx_print(pos, font, TEXT_ALIGN_LEFT,
              c->active ? (color_t){ 63, 191, 95, 255 } : color_white,
              "Call %u, %s %ld:%02ld%s", c->id, c->active ? "active" : "ended",
              secs / 60, secs % 60, c->encrypted ? ", enc" : "");

    // Its talkers, as many as fit on a line
    char line[48];
    size_t o = 0;
    line[0] = '\0';
    for(int k = 0; k < c->ntalkers; k++)
    {
        char id[12];
        sniprintf(id, sizeof(id), "%s%lu", k ? " " : "",
                  (unsigned long)c->talkers[k]);
        if(o + strlen(id) + 1 > sizeof(line))
            break;
        strcpy(line + o, id);
        if(gfx_getTextWidth(font, line)
           > CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad)
        {
            line[o] = '\0';
            break;
        }
        o += strlen(id);
    }
    pos.y += 11;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%s",
              line[0] ? line : "No talker known");
}

/**
 * TETRA groups page: the groups heard, then the picked one's last talker
 * and radios.
 */
static void _ui_drawTetraGroups(ui_state_t *ui_state)
{
    int n = tetraHeard_groups(heardGroups, TETRA_HEARD_GROUPS);
    int16_t y = _ui_heardList(ui_state, "Groups", n, _ui_groupRow);

    // The group filter, ENTER sets or clears it
    uint32_t filter = tetraStatus_filter();
    point_t hp = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };
    if(filter != 0)
        gfx_print(hp, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
                  (color_t){ 63, 191, 95, 255 }, "F %lu",
                  (unsigned long)filter);
    else
        gfx_print(hp, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, info_grey,
                  "ENT: filter");
    if(n == 0)
        return;

    const struct tetraGroup *g = &heardGroups[ui_state->tetra_sds_sel];
    const fontSize_t font = FONT_SIZE_6PT;
    point_t pos = { layout.horizontal_pad, (int16_t)(y + 8) };
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "Last %lu, %u radios",
              (unsigned long)g->lastRadio, g->nradios);

    // The radios, as many as fit on a line
    char line[48];
    size_t o = 0;
    line[0] = '\0';
    for(int k = 0; k < g->nradios; k++)
    {
        char id[12];
        sniprintf(id, sizeof(id), "%s%lu", k ? " " : "",
                  (unsigned long)g->radios[k]);
        if(o + strlen(id) + 1 > sizeof(line))
            break;
        strcpy(line + o, id);
        if(gfx_getTextWidth(font, line)
           > CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad)
        {
            line[o] = '\0';
            break;
        }
        o += strlen(id);
    }
    pos.y += 11;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%s", line);
}

/**
 * A frequency in MHz with four decimals, or a dash.
 */
static void _ui_mhz(char *buf, size_t len, uint32_t hz)
{
    if(hz == 0)
        sniprintf(buf, len, "-");
    else
        sniprintf(buf, len, "%lu.%04lu", (unsigned long)(hz / 1000000),
                  (unsigned long)((hz % 1000000) / 100));
}

static const char *const cellLoad[] = { "?", "low", "medium", "high" };

/**
 * TETRA cell page: identity, frequencies, services, encryption, load,
 * network time.
 */
static void _ui_drawTetraCell(void)
{
    struct tetraCellInfo c;
    tetraCell_get(&c);
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    point_t pos = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };

    if(!c.valid)
    {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "Cell");
        pos.y += 2 * step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "No cell yet");
        return;
    }

    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "Cell %u-%u CC%u",
              c.mcc, c.mnc, c.colourCode);
    if(c.sysinfo)
        gfx_print(pos, font, TEXT_ALIGN_RIGHT, yellow_fab413, "LA %u", c.la);

    if(c.sysinfo)
    {
        static const char *const offsets[] = { "", " +6.25k", " -6.25k",
                                               " +12.5k" };
        char dl[16], ul[16];
        _ui_mhz(dl, sizeof(dl), c.dlHz);
        _ui_mhz(ul, sizeof(ul), c.ulHz);
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "DL %s UL %s", dl,
                  ul);
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey,
                  "Ch %u B%u%s D%u%s", c.carrier, c.band,
                  offsets[c.offset & 3], c.duplex, c.reverse ? " rev" : "");

        // Services offered, the ones present
        static const struct { uint16_t bit; const char *name; } serv[] = {
            { TETRA_SERV_VOICE, "Voice" }, { TETRA_SERV_CSD, "CSD" },
            { TETRA_SERV_PACKET, "Pkt" },  { TETRA_SERV_REG, "Reg" },
            { TETRA_SERV_PRIORITY, "Prio" },
            { TETRA_SERV_SYS_WIDE, "SysW" },
            { TETRA_SERV_MIGRATION, "Migr" },
            { TETRA_SERV_ADV_LINK, "AdvL" },
        };
        // As many as fit on the line: gfx_print() would wrap the rest
        char sv[64] = "";
        size_t o = 0;
        const int16_t room = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
        for(size_t i = 0; i < sizeof(serv) / sizeof(serv[0]); i++)
        {
            if(!(c.services & serv[i].bit) || o + 8 >= sizeof(sv))
                continue;
            size_t prev = o;
            o += sniprintf(sv + o, sizeof(sv) - o, "%s%s", o ? " " : "",
                           serv[i].name);
            if(gfx_getTextWidth(font, sv) > room)
            {
                sv[prev] = '\0';
                break;
            }
        }
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", sv);
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white,
                  "Air enc %s, load %s",
                  (c.services & TETRA_SERV_AIE) ? "yes" : "no",
                  cellLoad[c.load & 3]);
    }
    else
    {
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "Waiting for SYSINFO");
    }

    static const char *const sharing[] = { "continuous", "carrier sharing",
                                           "MCCH sharing", "traffic sharing" };
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "Sys %u%s, %s",
              c.systemCode, (c.systemCode == 3) ? " (V3)" : "",
              sharing[c.sharing & 3]);
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "HF %u CCK %u",
              c.hyperframe, c.cck);

    struct tetraTime t;
    pos.y += step;
    if(tetraCell_time(&t))
    {
        // Date and time, and the offset from UTC if it fits
        char tm[40];
        int o = sniprintf(tm, sizeof(tm), "%04u-%02u-%02u %02u:%02u:%02u",
                          t.year, t.month, t.day, t.hour, t.minute, t.second);
        sniprintf(tm + o, sizeof(tm) - o, " %c%02d:%02d",
                  (t.offsetMin < 0) ? '-' : '+', abs(t.offsetMin) / 60,
                  abs(t.offsetMin) % 60);
        if(gfx_getTextWidth(font, tm)
           > CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad)
            tm[o] = '\0';
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", tm);
    }
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "No network time yet");
}

/**
 * TETRA neighbours page, after a site scan: the cells visited, reached
 * first, then by RSSI. UP / DOWN pick one, ENTER tunes the VFO to it.
 */
static void _ui_drawTetraScan(ui_state_t *ui_state)
{
    static UI_BSS struct tetraScanResult sr[TETRA_SCAN_MAX];
    int done;
    int n = tetraScan_results(sr, &done);
    const bool running = tetraScan_running();
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    point_t pos = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };
    static const color_t reached = { 63, 191, 95, 255 };

    if(running)
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413,
                  "Scanning %d/%d", done, n);
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "Scan %d", n);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, info_grey,
              running ? "ESC stop" : "OK=tune #=redo");

    // The picked row stays within the list and on screen
    if(ui_state->tetra_sds_sel >= n && n > 0)
        ui_state->tetra_sds_sel = n - 1;
    const int rows = (CONFIG_SCREEN_HEIGHT - pos.y - 2) / step;
    int first = 0;
    if(rows > 0 && ui_state->tetra_sds_sel >= rows)
        first = ui_state->tetra_sds_sel - rows + 1;

    const int16_t room = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    for(int i = first; i < n && i < first + rows; i++)
    {
        const struct tetraScanResult *r = &sr[i];
        char left[32], right[16], mhz[12];
        _ui_mhz(mhz, sizeof(mhz), r->dlHz);
        int o;
        if(r->cellId == TETRA_SCAN_SERVING)
            o = sniprintf(left, sizeof(left), "* %s", mhz);
        else
            o = sniprintf(left, sizeof(left), "#%u %s", r->cellId, mhz);

        color_t rc = info_grey;
        if(!r->measured)
            sniprintf(right, sizeof(right), "...");
        else if(r->sync)
        {
            sniprintf(right, sizeof(right), "e%u%% %d", r->phaseErr, r->rssi);
            rc = reached;
        }
        else
            sniprintf(right, sizeof(right), "%ddBm", r->rssi);

        // The cell found there, if it fits: its network, else its LA
        if(r->cell)
            sniprintf(left + o, sizeof(left) - o, " %u-%u", r->mcc, r->mnc);
        if(r->sysinfo
           && gfx_getTextWidth(font, left) + 6 + gfx_getTextWidth(font, right)
              > room)
            sniprintf(left + o, sizeof(left) - o, " LA%u", r->la);
        if(gfx_getTextWidth(font, left) + 6 + gfx_getTextWidth(font, right)
           > room)
            left[o] = '\0';

        pos.y += step;
        color_t lc = r->measured ? color_white : info_grey;
        if(i == ui_state->tetra_sds_sel)
        {
            gfx_drawRect((point_t){ 0, (int16_t)(pos.y - 9) },
                         CONFIG_SCREEN_WIDTH, step, lc, true);
            lc = color_black;
            rc = color_black;
        }
        gfx_print(pos, font, TEXT_ALIGN_LEFT, lc, "%s", left);
        gfx_print(pos, font, TEXT_ALIGN_RIGHT, rc, "%s", right);
    }
}

/**
 * TETRA neighbours page: the cells announced by the serving one, or the
 * results of a site scan (# starts one).
 */
static void _ui_drawTetraNeighbours(ui_state_t *ui_state)
{
    static UI_BSS struct tetraNeighbourInfo nb[TETRA_NEIGHBOURS_KEPT];
    if(tetraScan_running() || tetraScan_count() > 0)
    {
        _ui_drawTetraScan(ui_state);
        return;
    }

    int n = tetraCell_neighbours(nb, TETRA_NEIGHBOURS_KEPT);
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    point_t pos = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };

    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "Neighbours %d", n);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, info_grey, "# scan");
    if(n == 0)
    {
        pos.y += 2 * step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "None announced yet");
        return;
    }

    static const char *const load[] = { "?", "lo", "med", "hi" };
    const int16_t room = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    for(int i = 0; i < n && pos.y + step < CONFIG_SCREEN_HEIGHT; i++)
    {
        char left[32], right[12];
        _ui_mhz(right, sizeof(right), nb[i].dlHz);
        int o = sniprintf(left, sizeof(left), "#%u %s", nb[i].cellId, right);
        sniprintf(right, sizeof(right), "%s%s", load[nb[i].load & 3],
                  nb[i].synchronised ? " S" : "");

        // Its network or location area when other than the serving cell's,
        // if it fits
        if(nb[i].mcc != 0xffff || nb[i].mnc != 0xffff)
            sniprintf(left + o, sizeof(left) - o, " %u-%u", nb[i].mcc,
                      nb[i].mnc);
        else if(nb[i].la != 0xffff)
            sniprintf(left + o, sizeof(left) - o, " LA%u", nb[i].la);
        if(gfx_getTextWidth(font, left) + 6 + gfx_getTextWidth(font, right)
           > room)
            left[o] = '\0';

        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", left);
        gfx_print(pos, font, TEXT_ALIGN_RIGHT, info_grey, "%s", right);
    }
}

/**
 * TETRA radios page: the radios heard, then the picked one's activity and
 * last position.
 */
static void _ui_drawTetraRadios(ui_state_t *ui_state)
{
    int n = tetraHeard_radios(heardRadios, TETRA_HEARD_RADIOS);
    int16_t y = _ui_heardList(ui_state, "Radios", n, _ui_radioRow);
    if(n == 0)
        return;

    const struct tetraRadio *r = &heardRadios[ui_state->tetra_sds_sel];
    const fontSize_t font = FONT_SIZE_6PT;
    point_t pos = { layout.horizontal_pad, (int16_t)(y + 8) };
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "Last %s, %u calls %u SDS",
              radioActivity[r->activity & 3], r->calls, r->sds);
    pos.y += 11;
    if(r->pos)
    {
        long la = r->lat, lo = r->lon;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey,
                  "%s%ld.%05ld %s%ld.%05ld", la < 0 ? "-" : "",
                  labs(la) / 100000, labs(la) % 100000, lo < 0 ? "-" : "",
                  labs(lo) / 100000, labs(lo) % 100000);
    }
    else
    {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "No position");
    }
}
#endif

#ifdef CONFIG_TETRA
/**
 * TETRA symbol page: the phase of the last symbols on a circle, where a
 * clean signal makes four dots at +-45 and +-135 degrees, and the histogram
 * of the raw discriminator values with the four levels the demodulator's
 * fit expects: marks off the peaks show a wrong gain or offset.
 */
static void _ui_drawTetraScope(void)
{
    static UI_BSS float d[TETRA_DEMOD_SCOPE];
    static UI_BSS float y[TETRA_DEMOD_SCOPE];
    float levels[4];
    tetraDemod_scope(tetra_getDemod(), d, y, levels);

    struct tetraStatus ts;
    tetra_getStatus(&ts);

    static const color_t grey = { 120, 120, 120, 255 };
    static const color_t dot = { 47, 179, 196, 255 };
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t top = layout.top_h + 3;

    gfx_print((point_t){ layout.horizontal_pad, (int16_t)(top + 8) }, font,
              TEXT_ALIGN_LEFT, yellow_fab413, "Symbols e%u%% %+dHz %s",
              ts.phaseErr, ts.carrierHz, ts.sync ? "SYNC" : "");

    // Constellation of the phase changes
    const int16_t cx = 36, cy = 74, r = 28;
    gfx_drawCircle((point_t){ cx, cy }, r, grey);
    gfx_drawLine((point_t){ cx - r - 2, cy }, (point_t){ cx + r + 2, cy },
                 grey);
    gfx_drawLine((point_t){ cx, cy - r - 2 }, (point_t){ cx, cy + r + 2 },
                 grey);
    for (int i = 0; i < TETRA_DEMOD_SCOPE; i++) {
        if (isnan(y[i]))
            continue;
        int16_t px = cx + (int16_t)lrintf(r * cosf(y[i]));
        int16_t py = cy - (int16_t)lrintf(r * sinf(y[i]));
        gfx_drawRect((point_t){ (int16_t)(px - 1), (int16_t)(py - 1) }, 2, 2,
                     dot, true);
    }
    for (int k = 0; k < 4; k++) {
        float a = (2 * k - 3) * 0.78539816f;
        int16_t px = cx + (int16_t)lrintf(r * cosf(a));
        int16_t py = cy - (int16_t)lrintf(r * sinf(a));
        gfx_drawRect((point_t){ (int16_t)(px - 1), (int16_t)(py - 1) }, 3, 3,
                     yellow_fab413, false);
    }

    // Histogram of the raw values, over the data and the expected levels
    const int16_t hx = 74, hw = 82, hy = 36, hh = 64;
    const int bins = hw / 2;
    float lo = levels[0], hi = levels[3];
    for (int i = 0; i < TETRA_DEMOD_SCOPE; i++) {
        if (d[i] < lo)
            lo = d[i];
        if (d[i] > hi)
            hi = d[i];
    }
    float span = hi - lo;
    if (span <= 0)
        span = 1;
    lo -= span * 0.05f;
    span *= 1.1f;

    uint16_t count[41] = { 0 };
    uint16_t peak = 1;
    for (int i = 0; i < TETRA_DEMOD_SCOPE; i++) {
        int b = (int)((d[i] - lo) / span * bins);
        if (b < 0 || b >= bins)
            continue;
        if (++count[b] > peak)
            peak = count[b];
    }
    gfx_drawLine((point_t){ hx, (int16_t)(hy + hh) },
                 (point_t){ (int16_t)(hx + hw), (int16_t)(hy + hh) }, grey);
    for (int b = 0; b < bins; b++) {
        int16_t h = (int16_t)((count[b] * (hh - 2)) / peak);
        if (h > 0)
            gfx_drawRect((point_t){ (int16_t)(hx + 2 * b),
                                    (int16_t)(hy + hh - h) },
                         2, h, color_white, true);
    }
    for (int k = 0; k < 4; k++) {
        int16_t mx = hx + (int16_t)((levels[k] - lo) / span * hw);
        if (mx < hx || mx > hx + hw)
            continue;
        gfx_drawLine((point_t){ mx, (int16_t)(hy + hh + 1) },
                     (point_t){ mx, (int16_t)(hy + hh + 5) }, yellow_fab413);
        gfx_drawLine((point_t){ mx, hy }, (point_t){ mx, (int16_t)(hy + 3) },
                     yellow_fab413);
    }
    gfx_print((point_t){ hx, (int16_t)(hy + hh + 15) }, font, TEXT_ALIGN_LEFT,
              info_grey, "Raw, marks: fit");
}
#endif

#ifdef CONFIG_TETRA
/**
 * The TETRA page shown in place of the VFO or memory screen, if any.
 *
 * @return true if one was drawn.
 */
static bool _ui_drawTetraPage(ui_state_t *ui_state)
{
    if((last_state.channel.mode != OPMODE_TETRA)
       || (ui_state->tetra_page == TETRA_PAGE_MAIN))
        return false;

    _ui_drawMainTop(ui_state);

    // Page number, the key that goes straight to it
    gfx_print(layout.top_pos, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, info_grey,
              "%u/%u", ui_state->tetra_page, TETRA_PAGE_NUM - 1);

    switch(ui_state->tetra_page)
    {
        case TETRA_PAGE_DEBUG:
            _ui_drawTetraDebug();
            break;
        case TETRA_PAGE_SDS:
            _ui_drawTetraSds(ui_state);
            break;
        case TETRA_PAGE_GROUPS:
            _ui_drawTetraGroups(ui_state);
            break;
        case TETRA_PAGE_RADIOS:
            _ui_drawTetraRadios(ui_state);
            break;
        case TETRA_PAGE_CALLS:
            _ui_drawTetraCalls(ui_state);
            break;
        case TETRA_PAGE_CELL:
            _ui_drawTetraCell();
            break;
        case TETRA_PAGE_NEIGHBOURS:
            _ui_drawTetraNeighbours(ui_state);
            break;
        case TETRA_PAGE_SCOPE:
            _ui_drawTetraScope();
            break;
        default:
            break;
    }
    return true;
}
#endif

void _ui_drawMainVFO(ui_state_t* ui_state)
{
    gfx_clearScreen();

    #if defined(CONFIG_UI_RICH_MAIN) && defined(CONFIG_ACARS)
    if(_ui_drawAcarsPage(ui_state))
        return;
    #endif
    #if defined(CONFIG_UI_RICH_MAIN) && defined(CONFIG_AIS)
    if(_ui_drawAisPage(ui_state))
        return;
    #endif

    #ifdef CONFIG_DMR_RX
    if(_ui_drawDmrPage(ui_state))
        return;
    #endif

    #ifdef CONFIG_TETRA
    if(_ui_drawTetraPage(ui_state))
        return;
    #endif

    #ifdef CONFIG_UI_RICH_MAIN
    _ui_drawRichMain(ui_state, false);
    return;
    #endif

    _ui_drawMainTop(ui_state);
    _ui_drawModeInfo(ui_state);

    // Show VFO frequency unless the mode shows a call (M17 LSF, TETRA call)
    if(_ui_callShown() == false)
        _ui_drawFrequency();

    #ifdef CONFIG_TETRA
    if(last_state.channel.mode == OPMODE_TETRA)
        _ui_drawTetraLine1(ui_state);
    #endif
    #ifdef CONFIG_DMR_RX
    if(last_state.channel.mode == OPMODE_DMR)
        _ui_drawDmrLine1();
    #endif

    _ui_drawMainBottom();
}

void _ui_drawMainVFOInput(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawVFOMiddleInput(ui_state);
    _ui_drawMainBottom();
}

void _ui_drawMainMEM(ui_state_t* ui_state)
{
    gfx_clearScreen();

    #if defined(CONFIG_UI_RICH_MAIN) && defined(CONFIG_ACARS)
    if(_ui_drawAcarsPage(ui_state))
        return;
    #endif
    #if defined(CONFIG_UI_RICH_MAIN) && defined(CONFIG_AIS)
    if(_ui_drawAisPage(ui_state))
        return;
    #endif

    #ifdef CONFIG_DMR_RX
    if(_ui_drawDmrPage(ui_state))
        return;
    #endif

    #ifdef CONFIG_TETRA
    if(_ui_drawTetraPage(ui_state))
        return;
    #endif

    #ifdef CONFIG_UI_RICH_MAIN
    _ui_drawRichMain(ui_state, true);
    return;
    #endif
    _ui_drawMainTop(ui_state);
    _ui_drawModeInfo(ui_state);

    // Show channel data unless the mode shows a call (M17 LSF, TETRA call)
    if(_ui_callShown() == false)
    {
        _ui_drawBankChannel();
        _ui_drawFrequency();
    }

    _ui_drawMainBottom();
}

#ifdef CONFIG_SPECTRUM
/* Last sweep received from the RTX thread */
static rssi_t spec_raw[RTX_SPECTRUM_MAX_POINTS];
/*
 * Shown level of each point, in dBm: follows a rise at once, so that
 * bursts show, and falls back slowly, so that the noise does not flicker.
 */
static rssi_t spec_rssi[RTX_SPECTRUM_MAX_POINTS];
static int32_t spec_level[RTX_SPECTRUM_MAX_POINTS]; /* 1/16 dB */
static rssi_t spec_floor; /* Bottom of the scale, moved only by 6 dB or more */

/*
 * Waterfall: the raw sweeps, newest first, in dBm. Rows of the screen are
 * at most WF_ROWS; in PSRAM on the C62.
 */
#define WF_ROWS 64
#define WF_RANGE 40 /* dB from the noise floor to the top colour */
static UI_BSS int8_t wf[WF_ROWS][SPECTRUM_POINTS];
static uint8_t wf_head;  /* Row of the newest sweep */
static uint8_t wf_count; /* Rows filled */

/**
 * Colour of a waterfall level, 0 (noise floor) to 255 (WF_RANGE above):
 * black, blue, cyan, yellow, red, white.
 */
static color_t _ui_wfColor(int v)
{
    static const uint8_t stops[][3] = {
        { 0, 0, 0 },     { 0, 0, 160 },  { 0, 160, 255 },
        { 255, 230, 0 }, { 255, 32, 0 }, { 255, 255, 255 },
    };
    const int n = sizeof(stops) / sizeof(stops[0]) - 1;
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    int seg = (v * n) / 256;
    int f = (v * n) % 256; /* 0..255 within the segment */
    color_t c;
    c.r = (uint8_t)(stops[seg][0] + ((stops[seg + 1][0] - stops[seg][0]) * f) / 256);
    c.g = (uint8_t)(stops[seg][1] + ((stops[seg + 1][1] - stops[seg][1]) * f) / 256);
    c.b = (uint8_t)(stops[seg][2] + ((stops[seg + 1][2] - stops[seg][2]) * f) / 256);
    c.alpha = 255;
    return c;
}

/**
 * Fetch the last spectrum sweep.
 *
 * @return true if it is a new one, to be drawn.
 */
bool _ui_spectrumUpdate(ui_state_t *ui_state)
{
    uint32_t sweeps = rtx_spectrumRead(spec_raw);
    if (sweeps == ui_state->spec_sweeps)
        return false;

    // First sweep of a range: start from it
    bool first = (ui_state->spec_sweeps == 0);
    ui_state->spec_sweeps = sweeps;
    if (sweeps == 0)
        return true;

    // The sweep as a new waterfall row; a new range starts it again
    if (first)
        wf_count = 0;
    wf_head = (wf_head + WF_ROWS - 1) % WF_ROWS;
    for (int i = 0; i < SPECTRUM_POINTS; i++) {
        rssi_t v = spec_raw[i];
        wf[wf_head][i] = (int8_t)((v < -127) ? -127 : (v > 0) ? 0 : v);
    }
    if (wf_count < WF_ROWS)
        wf_count++;

    rssi_t lo = 0;
    for (int i = 0; i < SPECTRUM_POINTS; i++) {
        int32_t level = spec_raw[i] * 16;
        if (first || (level > spec_level[i]))
            spec_level[i] = level;
        else
            spec_level[i] += (level - spec_level[i]) / 4;
        spec_rssi[i] = spec_level[i] / 16;
        if ((i == 0) || (spec_rssi[i] < lo))
            lo = spec_rssi[i];
    }

    lo -= 3;
    if (first || (lo < spec_floor) || (lo > spec_floor + 6))
        spec_floor = lo;

    return true;
}

/**
 * Strongest point of the last sweep, or the marker if there is none yet.
 */
uint8_t _ui_spectrumPeak(ui_state_t *ui_state)
{
    if (ui_state->spec_sweeps == 0)
        return ui_state->spec_marker;

    uint8_t peak = 0;
    for (uint8_t i = 1; i < SPECTRUM_POINTS; i++) {
        if (spec_rssi[i] > spec_rssi[peak])
            peak = i;
    }
    return peak;
}

/**
 * Print a frequency in MHz, with as many decimals as the step needs.
 */
static void _ui_spectrumFreq(char *buf, size_t len, freq_t freq, freq_t step)
{
    if (step >= 10000)
        snprintf(buf, len, "%lu.%03lu", (unsigned long)(freq / 1000000),
                 (unsigned long)((freq % 1000000) / 1000));
    else
        snprintf(buf, len, "%lu.%05lu", (unsigned long)(freq / 1000000),
                 (unsigned long)((freq % 1000000) / 10));
}

void _ui_drawSpectrum(ui_state_t *ui_state)
{
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t fh = gfx_getFontHeight(font);
    const int16_t pad = layout.horizontal_pad;
    const freq_t start = ui_state->spec_start;
    const freq_t step = ui_state->spec_step;
    const freq_t end = start + step * (SPECTRUM_POINTS - 1);
    const freq_t marker = start + step * ui_state->spec_marker;
    char buf[24];

    gfx_clearScreen();

    // Range: first and last frequency, span in the middle; or the centre
    // frequency being typed
    int16_t y = fh + 1;
    if (ui_state->spec_input) {
        char typed[9] = "___.____";
        for (int i = 0, k = 0; i < ui_state->spec_digits; i++, k++) {
            if (k == 3)
                k++; /* Skip the point */
            typed[k] = ui_state->spec_input_buf[i];
        }
        gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_LEFT, yellow_fab413,
                  "Centre %s MHz", typed);
    } else {
        _ui_spectrumFreq(buf, sizeof(buf), start, step);
        gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_LEFT, color_white,
                  buf);
        _ui_spectrumFreq(buf, sizeof(buf), end, step);
        gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_RIGHT, color_white,
                  buf);
        freq_t span = step * SPECTRUM_POINTS;
        if (span >= 1000000)
            snprintf(buf, sizeof(buf), "%luM",
                     (unsigned long)(span / 1000000));
        else
            snprintf(buf, sizeof(buf), "%luk", (unsigned long)(span / 1000));
        gfx_print((point_t){ 0, y }, font, TEXT_ALIGN_CENTER, yellow_fab413,
                  buf);
    }

    // Graph area: the spectrum, then the waterfall under it
    const int16_t top = y + 3;
    const int16_t wfBottom = CONFIG_SCREEN_HEIGHT - 2 * (fh + 2) - 2;
    const int16_t bottom = top + (wfBottom - top) * 9 / 20;
    const int16_t height = bottom - top;
    const int16_t barW = CONFIG_SCREEN_WIDTH / SPECTRUM_POINTS;
    const int16_t x0 = (CONFIG_SCREEN_WIDTH - barW * SPECTRUM_POINTS) / 2;

    gfx_drawLine((point_t){ 0, bottom }, (point_t){ CONFIG_SCREEN_WIDTH - 1,
                 bottom }, color_grey);

    // Marker
    const int16_t mx = x0 + barW * ui_state->spec_marker;
    gfx_drawRect((point_t){ mx, top }, barW, height, color_grey, true);

    rssi_t markerRssi = 0;
    if (ui_state->spec_sweeps > 0) {
        // Scale from just below the noise floor, 60 dB high
        const rssi_t lo = spec_floor;
        const rssi_t hi = lo + 60;

        for (int i = 0; i < SPECTRUM_POINTS; i++) {
            int32_t h = ((int32_t)(spec_rssi[i] - lo) * height) / (hi - lo);
            if (h < 1)
                h = 1;
            if (h > height)
                h = height;
            color_t c = (i == ui_state->spec_marker) ? yellow_fab413 :
                                                       color_white;
            gfx_drawRect((point_t){ x0 + barW * i, bottom - h }, barW, h, c,
                         true);
        }
        markerRssi = spec_rssi[ui_state->spec_marker];
    }

    // Waterfall: one row per sweep, the newest at the top
    const int16_t wfTop = bottom + 2;
    int rows = wfBottom - wfTop;
    if (rows > WF_ROWS)
        rows = WF_ROWS;
    if (rows > wf_count)
        rows = wf_count;
    const rssi_t wfLo = spec_floor + 3;
    for (int row = 0; row < rows; row++) {
        const int8_t *line = wf[(wf_head + row) % WF_ROWS];
        const int16_t py = wfTop + row;
        for (int i = 0; i < SPECTRUM_POINTS; i++) {
            color_t c = _ui_wfColor(((line[i] - wfLo) * 256) / WF_RANGE);
            for (int k = 0; k < barW; k++)
                gfx_setPixel((point_t){ (int16_t)(x0 + barW * i + k), py }, c);
        }
    }
    // Marker ticks on both sides of the waterfall
    gfx_drawRect((point_t){ mx, wfTop - 2 }, barW, 2, yellow_fab413, true);
    gfx_drawRect((point_t){ mx, wfBottom }, barW, 2, yellow_fab413, true);

    // Marker frequency and level
    y = wfBottom + fh + 2;
    _ui_spectrumFreq(buf, sizeof(buf), marker, step);
    gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_LEFT, yellow_fab413, buf);
    if (ui_state->spec_sweeps > 0)
        gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_RIGHT, color_white,
                  "%d dBm", (int)markerRssi);

    // Keys
    y += fh + 2;
    gfx_print((point_t){ 0, y }, font, TEXT_ALIGN_CENTER, info_grey,
              ui_state->spec_input ? "ENTER:go ESC:cancel"
                                   : "PTT:VFO 0:pk *+ #- 5:f");
}
#endif

#ifdef CONFIG_FM_BROADCAST
/**
 * Broadcast FM screen: the station, its signal, the volume and the keys.
 */
void _ui_drawFmRadio(ui_state_t *ui_state)
{
    static const color_t green = { 63, 191, 95, 255 };
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t pad = layout.horizontal_pad;
    struct rtxBroadcast bc;
    rtx_broadcastRead(&bc);
    ui_state->fm_shown = bc;
    freq_t f = (bc.freq != 0) ? bc.freq : ui_state->fm_freq;

    gfx_clearScreen();
    gfx_print((point_t){ pad, 11 }, font, TEXT_ALIGN_LEFT, yellow_fab413,
              "FM radio");
    gfx_print((point_t){ pad, 11 }, font, TEXT_ALIGN_RIGHT, color_white,
              "Vol %u", ui_state->fm_volume);
    gfx_drawLine((point_t){ 0, 15 }, (point_t){ CONFIG_SCREEN_WIDTH - 1, 15 },
                 info_grey);

    // The frequency, or the one being typed
    char buf[16];
    color_t fc = color_white;
    if(ui_state->fm_input)
    {
        sniprintf(buf, sizeof(buf), "%s_", ui_state->fm_input_buf);
        fc = yellow_fab413;
    }
    else
    {
        sniprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(f / 1000000),
                  (unsigned long)((f % 1000000) / 100000));
    }
    uint16_t fw = gfx_getTextWidth(FONT_SIZE_16PT, buf);
    uint16_t uw = gfx_getTextWidth(font, "MHz");
    int16_t x = (CONFIG_SCREEN_WIDTH - fw - uw - 4) / 2;
    gfx_print((point_t){ x, 48 }, FONT_SIZE_16PT, TEXT_ALIGN_LEFT, fc, "%s",
              buf);
    gfx_print((point_t){ (int16_t)(x + fw + 4), 48 }, font, TEXT_ALIGN_LEFT,
              info_grey, ui_state->fm_input ? "x0.1" : "MHz");

    // Station, or the seek going on
    const char *st;
    color_t sc;
    if(bc.seek > 0)
    {
        st = "Seeking up";
        sc = yellow_fab413;
    }
    else if(bc.seek < 0)
    {
        st = "Seeking down";
        sc = yellow_fab413;
    }
    else if(bc.station)
    {
        st = "Station";
        sc = green;
    }
    else
    {
        st = "No station";
        sc = info_grey;
    }
    gfx_print((point_t){ 0, 66 }, font, TEXT_ALIGN_CENTER, sc, "%s", st);

    // Signal bar, RSSI 0 to 80
    const int16_t bx = pad, bw = CONFIG_SCREEN_WIDTH - 2 * pad, by = 74;
    int16_t w = (int16_t)((bc.rssi > 80 ? 80 : bc.rssi) * bw / 80);
    gfx_drawRect((point_t){ bx, by }, bw, 8, info_grey, false);
    if(w > 2)
        gfx_drawRect((point_t){ (int16_t)(bx + 1), (int16_t)(by + 1) },
                     w - 2, 6, bc.station ? green : yellow_fab413, true);
    gfx_print((point_t){ 0, 94 }, font, TEXT_ALIGN_CENTER, info_grey,
              "RSSI %u  SNR %u", bc.rssi, bc.snr);

    // Keys
    gfx_print((point_t){ 0, 112 }, font, TEXT_ALIGN_CENTER, info_grey,
              ui_state->fm_input ? "1001 = 100.1 MHz"
                                 : "UP/DN tune  P1/P2 seek");
    gfx_print((point_t){ 0, 124 }, font, TEXT_ALIGN_CENTER, info_grey,
              ui_state->fm_input ? "ENTER:go ESC:cancel"
                                 : "*/# vol  0-9 freq  ESC");
}
#endif

#ifdef CONFIG_UI_RICH_MAIN
#include <stdarg.h>

/*
 * Main screen for colour displays of 160x128 or more: a badge with the mode
 * and the receiver state on the top bar, the frequency or the call in a
 * card, the mode's own figures, and a segmented S-meter. Text, rectangles
 * and lines only.
 */

#define RICH_PAD    4
#define RICH_TOP_H  16
#define RICH_INFO_Y 19
#define RICH_SEGS   26  // Half S-units from S0 to S9+40

static const color_t rich_grey   = { 160, 160, 160, 255 };
static const color_t rich_dark   = {  48,  48,  48, 255 };
static const color_t rich_off    = {  38,  38,  38, 255 };
static const color_t rich_black  = {   0,   0,   0, 255 };
static const color_t rich_green  = {  63, 191,  95, 255 };
static const color_t rich_red    = { 224,  72,  72, 255 };
static const color_t rich_orange = { 255, 140,  64, 255 };
static const color_t rich_teal   = {  47, 179, 196, 255 };
static const color_t rich_violet = { 185, 163, 255, 255 };
#ifdef CONFIG_AIS
static const color_t ais_teal    = {  72, 208, 150, 255 };
#endif

/**
 * Height of the digits and capitals of a font: gfx_getFontHeight() also
 * counts the descenders.
 */
static int16_t _ui_richCap(fontSize_t font)
{
    static const uint8_t cap[FONT_SIZE_NUM] = { 5, 8, 11, 12, 14, 17, 21 };
    return cap[font];
}

/**
 * Print text whose top is at y; right alignment takes x as the right margin.
 */
static uint16_t _ui_richPrint(int16_t x, int16_t y, fontSize_t font,
                              textAlign_t align, color_t color,
                              const char *fmt, ...)
{
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // The glyphs' last row sits on the baseline
    point_t pos = { x, (int16_t)(y + _ui_richCap(font) - 1) };
    gfx_print(pos, font, align, color, "%s", buf);
    return gfx_getTextWidth(font, buf);
}

/**
 * A label in a box, filled or outlined.
 *
 * @return the x right after it.
 */
static int16_t _ui_richBadge(int16_t x, int16_t y, int16_t h, fontSize_t font,
                             const char *text, color_t color, bool filled)
{
    uint16_t w = gfx_getTextWidth(font, text) + 8;
    gfx_drawRect((point_t){ x, y }, w, h, color, filled);
    // Centred; the larger glyphs a row higher, as they look low otherwise
    int16_t ty = y + (h - _ui_richCap(font)) / 2;
    if(font >= FONT_SIZE_8PT)
        ty -= 1;
    _ui_richPrint(x + 4, ty, font, TEXT_ALIGN_LEFT,
                  filled ? rich_black : color, "%s", text);
    return x + w;
}

/**
 * Mode badge colour and name.
 */
static color_t _ui_richModeColor(const char **name)
{
    switch(last_state.channel.mode)
    {
        #ifdef CONFIG_AM
        case OPMODE_AM:
            *name = "AM";
            return yellow_fab413;
        #endif
        #ifdef CONFIG_ACARS
        case OPMODE_ACARS:
        {
            static const color_t sky = { 95, 176, 255, 255 };
            *name = "ACARS";
            return sky;
        }
        #endif
        #ifdef CONFIG_AIS
        case OPMODE_AIS:
            *name = "AIS";
            return ais_teal;
        #endif
        #ifdef CONFIG_M17
        case OPMODE_M17:
            *name = "M17";
            return rich_orange;
        #endif
        #ifdef CONFIG_TETRA
        case OPMODE_TETRA:
            *name = "TETRA";
            return rich_teal;
        #endif
        case OPMODE_DMR:
            *name = "DMR";
            return rich_violet;
        default:
            *name = "FM";
            return yellow_fab413;
    }
}

/**
 * Position of a level on the S-meter, in segments: two per S-unit, 6 dB
 * each up to S9 (-73 dBm), 10 dB each above, up to S9+40.
 */
static int _ui_richMeterPos(rssi_t dbm)
{
    int u;
    if(dbm <= -73)
        u = (dbm + 127) / 3;
    else
        u = 18 + (dbm + 73) / 5;
    if(u < 0)
        u = 0;
    if(u > RICH_SEGS)
        u = RICH_SEGS;
    return u;
}

/**
 * Segmented S-meter from y to the bottom of the screen, with the squelch
 * threshold if the mode has one.
 *
 * @param sqlLabel: show the squelch level on the left instead of the
 * S-level, while it is being set.
 */
static void _ui_richMeter(int16_t y, color_t accent, bool squelch,
                          bool sqlLabel)
{
    const rssi_t rssi = last_state.rssi;
    const int16_t bottom = CONFIG_SCREEN_HEIGHT - 2;
    const int16_t x0 = RICH_PAD + 30;
    const int16_t x1 = CONFIG_SCREEN_WIDTH - RICH_PAD;
    const int16_t w = x1 - x0;

    gfx_drawHLine(y, 1, rich_dark);

    // Segments in the middle, scale below, squelch mark above
    const int16_t labelH = _ui_richCap(FONT_SIZE_5PT);
    const int16_t segH = 9;
    int16_t segY = y + ((bottom - y) - segH - labelH - 3) / 2 + 4;

    // S-level, large, on the left, or the squelch level
    uint8_t s = rssiToSlevel(rssi);
    int16_t ty = segY + (segH - _ui_richCap(FONT_SIZE_10PT)) / 2;
    // Level text, smaller when 10 pt does not fit before the segments
    char lvl[8];
    if(sqlLabel)
        sniprintf(lvl, sizeof(lvl), "SQ%u", last_state.settings.sqlLevel);
    else if(s <= 9)
        sniprintf(lvl, sizeof(lvl), "S%u", s);
    else
        sniprintf(lvl, sizeof(lvl), "+%u", (s - 9) * 10);
    fontSize_t lf = sqlLabel ? FONT_SIZE_8PT : FONT_SIZE_10PT;
    if(gfx_getTextWidth(lf, lvl) > x0 - RICH_PAD - 3)
        lf = FONT_SIZE_8PT;
    ty = segY + (segH - _ui_richCap(lf)) / 2;
    _ui_richPrint(RICH_PAD, ty, lf, TEXT_ALIGN_LEFT, accent, "%s", lvl);

    int lit = _ui_richMeterPos(rssi);
    for(int i = 0; i < RICH_SEGS; i++)
    {
        int16_t sx = x0 + (i * w) / RICH_SEGS;
        int16_t sw = (((i + 1) * w) / RICH_SEGS) - (i * w) / RICH_SEGS - 1;
        color_t c = rich_off;
        if(i < lit)
            c = (i < 18) ? rich_green : ((i < 22) ? yellow_fab413 : rich_red);
        gfx_drawRect((point_t){ sx, segY }, sw, segH, c, true);
    }

    static const struct { int pos; const char *text; } marks[] = {
        { 2, "1" }, { 6, "3" }, { 10, "5" }, { 14, "7" }, { 18, "9" },
        { 22, "+20" }, { 26, "+40" }
    };
    for(size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
    {
        uint16_t tw = gfx_getTextWidth(FONT_SIZE_5PT, marks[i].text);
        int16_t mx = x0 + (marks[i].pos * w) / RICH_SEGS - tw / 2;
        if(mx + tw > x1)
            mx = x1 - tw;
        _ui_richPrint(mx, segY + segH + 2, FONT_SIZE_5PT, TEXT_ALIGN_LEFT,
                      rich_grey, "%s", marks[i].text);
    }

    if(squelch)
    {
        rssi_t sql = -127 + (last_state.settings.sqlLevel * 66) / 15;
        int16_t mx = x0 + (_ui_richMeterPos(sql) * w) / RICH_SEGS;
        for(int16_t r = 0; r < 4; r++)
            gfx_drawLine((point_t){ mx - 3 + r, segY - 5 + r },
                         (point_t){ mx + 3 - r, segY - 5 + r }, accent);
    }
}

/**
 * The frequency, large, with its top at y.
 */
static void _ui_richFrequency(int16_t y)
{
    freq_t freq = platform_getPttStatus() ? last_state.channel.tx_frequency
                                          : last_state.channel.rx_frequency;
    char main_str[12];
    char small_str[4];
    sniprintf(main_str, sizeof(main_str), "%lu.%03lu",
              (unsigned long)(freq / 1000000),
              (unsigned long)((freq % 1000000) / 1000));
    sniprintf(small_str, sizeof(small_str), "%02lu",
              (unsigned long)((freq % 1000) / 10));

    uint16_t mw = gfx_getTextWidth(FONT_SIZE_16PT, main_str);
    uint16_t sw = gfx_getTextWidth(FONT_SIZE_10PT, small_str);
    uint16_t uw = gfx_getTextWidth(FONT_SIZE_6PT, "MHz") + 3;
    if(mw + sw + uw > CONFIG_SCREEN_WIDTH - 2 * RICH_PAD)
        uw = 0; // No room for the unit
    int16_t x = (CONFIG_SCREEN_WIDTH - (mw + sw + uw)) / 2;
    int16_t base = y + _ui_richCap(FONT_SIZE_16PT) - 1;

    gfx_print((point_t){ x, base }, FONT_SIZE_16PT, TEXT_ALIGN_LEFT,
              color_white, "%s", main_str);
    gfx_print((point_t){ (int16_t)(x + mw), base }, FONT_SIZE_10PT,
              TEXT_ALIGN_LEFT, color_white, "%s", small_str);
    if(uw > 0)
        gfx_print((point_t){ (int16_t)(x + mw + sw + 3), base }, FONT_SIZE_6PT,
                  TEXT_ALIGN_LEFT, rich_grey, "MHz");
}

/**
 * Card for a call: a badge and the called party, the talking party, and a
 * state line with a coloured dot.
 */
static void _ui_richCard(int16_t y, int16_t h, color_t bg, color_t accent,
                         const char *badge, const char *dst,
                         const char *talker, const char *talkerId,
                         color_t stateColor, const char *state,
                         const char *detail)
{
    const int16_t x = RICH_PAD;
    gfx_drawRect((point_t){ x, y }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD, h,
                 bg, true);

    // Called party
    int16_t row = y + 4;
    int16_t bh = _ui_richCap(FONT_SIZE_8PT) + 4;
    int16_t bx = _ui_richBadge(x + 4, row + 3, bh, FONT_SIZE_8PT, badge,
                               accent, true);
    _ui_richPrint(bx + 4, row, FONT_SIZE_16PT, TEXT_ALIGN_LEFT, color_white,
                  "%s", dst);

    // Talking party
    row += _ui_richCap(FONT_SIZE_16PT) + 5;
    uint16_t tw = _ui_richPrint(x + 4, row, FONT_SIZE_10PT, TEXT_ALIGN_LEFT,
                                yellow_fab413, "%s", talker);
    if(talkerId[0] != '\0')
        _ui_richPrint(x + 8 + tw, row + 6, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_grey, "%s", talkerId);

    // State
    row = y + h - _ui_richCap(FONT_SIZE_8PT) - 4;
    gfx_drawRect((point_t){ x + 4, row + 2 }, 5, 5, stateColor, true);
    uint16_t sw = _ui_richPrint(x + 12, row, FONT_SIZE_8PT, TEXT_ALIGN_LEFT,
                                stateColor, "%s", state);

    // Detail, smaller, only if it fits: gfx_print() would wrap it
    int16_t dx = x + 16 + sw;
    int16_t room = CONFIG_SCREEN_WIDTH - RICH_PAD - 3 - dx;
    if(gfx_getTextWidth(FONT_SIZE_6PT, detail) <= room)
        _ui_richPrint(dx, row + _ui_richCap(FONT_SIZE_8PT)
                              - _ui_richCap(FONT_SIZE_6PT),
                      FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey, "%s",
                      detail);
}

/**
 * Top bar: mode badge, receiver state, power or frequency, battery.
 */
static void _ui_richTop(ui_state_t *ui_state, bool callShown)
{
    const char *name;
    color_t accent = _ui_richModeColor(&name);
    int16_t x = _ui_richBadge(RICH_PAD, 0, 15, FONT_SIZE_8PT, name, accent,
                              true) + 3;

    // Receiver state: transmitting, signal heard, sync
    rtxStatus_t rtx = rtx_getCurrentStatus();
    if(rtx.opStatus == TX)
        x = _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "TX", rich_red, true);
    #ifdef CONFIG_TETRA
    else if(last_state.channel.mode == OPMODE_TETRA)
    {
        struct tetraStatus ts;
        tetra_getStatus(&ts);
        x = ts.sync ? _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "SYNC",
                                    rich_green, true)
                    : _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "NO SYNC",
                                    rich_red, false);
        // A group filter set
        if(tetraStatus_filter() != 0)
            x = _ui_richBadge(x + 2, 2, 12, FONT_SIZE_6PT, "F", rich_green,
                              false);
    }
    #endif
    #ifdef CONFIG_DMR_RX
    else if(last_state.channel.mode == OPMODE_DMR)
    {
        dmr_demod_status_t ds;
        dmr_demod_get_status(&ds);
        x = ds.locked ? _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "LOCK",
                                      rich_green, true)
                      : _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "SEARCH",
                                      rich_red, false);
    }
    #endif
    #ifdef CONFIG_ACARS
    else if(last_state.channel.mode == OPMODE_ACARS)
    {
        struct acarsStats st;
        acars_getStats(&st);
        if(st.inFrame)
            x = _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "DATA", rich_green,
                              true);
    }
    #endif
    #ifdef CONFIG_AIS
    else if(last_state.channel.mode == OPMODE_AIS)
    {
        if(rtx_rxSquelchOpen())
            x = _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "DATA", rich_green,
                              true);
    }
    #endif
    else if(rtx_rxSquelchOpen())
        x = _ui_richBadge(x, 2, 12, FONT_SIZE_6PT, "RX", rich_green, true);

    // Battery, right
    const int16_t bw = 14, bh = 8;
    int16_t bx = CONFIG_SCREEN_WIDTH - RICH_PAD - bw - 2;
    gfx_drawRect((point_t){ bx, 4 }, bw, bh, color_white, false);
    gfx_drawRect((point_t){ (int16_t)(bx + bw), 6 }, 2, 4, color_white, true);
    uint16_t fill = ((bw - 4) * last_state.charge) / 100;
    color_t bc = (last_state.charge > 20) ? rich_green : rich_red;
    gfx_drawRect((point_t){ (int16_t)(bx + 2), 6 }, fill, bh - 4, bc, true);
    // The percentage only if it does not run into the receiver state, and
    // not during a call: the frequency goes there
    char pct[8];
    sniprintf(pct, sizeof(pct), "%u%%", last_state.charge);
    uint16_t pw = gfx_getTextWidth(FONT_SIZE_6PT, pct);
    int16_t xe = bx - 3;
    if(!callShown && (bx - 3 - pw >= x + 3))
    {
        _ui_richPrint(CONFIG_SCREEN_WIDTH - bx + 3, 4, FONT_SIZE_6PT,
                      TEXT_ALIGN_RIGHT, color_white, "%s", pct);
        xe = bx - 3 - pw - 3;
    }
    if(ui_state->input_locked && (xe - 10 >= x))
    {
        xe -= 10;
        gfx_drawSymbol((point_t){ xe, 11 }, SYMBOLS_SIZE_6PT, TEXT_ALIGN_LEFT,
                       color_white, SYMBOL_LOCK);
    }

    // Middle: the frequency during a call, else the power or RX only
    char mid[16];
    freq_t f = last_state.channel.rx_frequency;
    uint32_t p = last_state.channel.power;
    if(callShown)
        sniprintf(mid, sizeof(mid), "%lu.%04lu", (unsigned long)(f / 1000000),
                  (unsigned long)((f % 1000000) / 100));
    else if(last_state.channel.rx_only
        #ifdef CONFIG_TETRA
            || last_state.channel.mode == OPMODE_TETRA
        #endif
        #ifdef CONFIG_AM
            || last_state.channel.mode == OPMODE_AM
        #endif
        #ifdef CONFIG_ACARS
            || last_state.channel.mode == OPMODE_ACARS
        #endif
        #ifdef CONFIG_AIS
            || last_state.channel.mode == OPMODE_AIS
        #endif
            || last_state.channel.mode == OPMODE_DMR)
    {
        #ifdef CONFIG_TETRA
        // The TETRA network time, once received, as the radio's clock
        struct tetraTime t;
        if(tetraCell_time(&t))
            sniprintf(mid, sizeof(mid), "%02u:%02u", t.hour, t.minute);
        else
        #endif
            sniprintf(mid, sizeof(mid), "RX only");
    }
    else if(p % 1000)
        sniprintf(mid, sizeof(mid), "%lu.%lu W", (unsigned long)(p / 1000),
                  (unsigned long)((p % 1000) / 100));
    else
        sniprintf(mid, sizeof(mid), "%lu W", (unsigned long)(p / 1000));
    uint16_t mw = gfx_getTextWidth(FONT_SIZE_6PT, mid);
    if(xe - x > mw)
        _ui_richPrint(x + (xe - x - mw) / 2, 4, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_grey, "%s", mid);

    gfx_drawHLine(RICH_TOP_H, 1, rich_dark);
}

/**
 * VFO or memory tag at the start of the information line: an outlined
 * "VFO", or the memory number filled.
 *
 * @return the x right after it.
 */
static int16_t _ui_richMemTag(int16_t y)
{
    char tag[8];
    bool mem = (last_state.ui_screen == MAIN_MEM);
    if(mem)
        sniprintf(tag, sizeof(tag), "M%03u", last_state.channel_index + 1);
    else
        sniprintf(tag, sizeof(tag), "VFO");
    return _ui_richBadge(RICH_PAD, y - 1, 12, FONT_SIZE_6PT, tag,
                         mem ? yellow_fab413 : rich_grey, mem) + 4;
}

/**
 * FM and AM: tone and squelch, the frequency, the TX frequency or channel
 * name and the level.
 */
static void _ui_richAnalog(bool mem)
{
    const int16_t y = RICH_INFO_Y;
    int16_t x = _ui_richMemTag(y);

    char mid[24] = "";
    if(last_state.channel.mode == OPMODE_FM)
    {
        bool tx = last_state.channel.fm.txToneEn;
        bool rx = last_state.channel.fm.rxToneEn;
        uint16_t tone = ctcss_tone[last_state.channel.fm.txTone];
        if(tx || rx)
            sniprintf(mid, sizeof(mid), "CTCSS %u.%u %s", tone / 10,
                      tone % 10, _ui_getToneEnabledString(tx, rx, true));
        else
            sniprintf(mid, sizeof(mid), "%s",
                      (last_state.channel.bandwidth == BW_12_5) ? "12.5k"
                                                                : "25k");
    }
    else
    {
        sniprintf(mid, sizeof(mid), "12.5k");
    }
    uint16_t qw = _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT,
                                rich_grey, "SQ %u",
                                last_state.settings.sqlLevel);

    // Tone or bandwidth, centred between the tag and the squelch level
    int16_t room = CONFIG_SCREEN_WIDTH - RICH_PAD - qw - 4 - x;
    fontSize_t mf = FONT_SIZE_8PT;
    if(gfx_getTextWidth(mf, mid) > room)
        mf = FONT_SIZE_6PT;
    int16_t mw = gfx_getTextWidth(mf, mid);
    _ui_richPrint(x + (room - mw) / 2,
                  y + _ui_richCap(FONT_SIZE_8PT) - _ui_richCap(mf), mf,
                  TEXT_ALIGN_LEFT, color_white, "%s", mid);

    _ui_richFrequency(34);

    const int16_t y2 = 62;
    freq_t rx = last_state.channel.rx_frequency;
    freq_t tx = last_state.channel.tx_frequency;
    if(mem)
        _ui_richPrint(RICH_PAD, y2, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                      "%.14s", last_state.channel.name);
    else if(tx != rx)
        _ui_richPrint(RICH_PAD, y2, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                      "TX %lu.%04lu", (unsigned long)(tx / 1000000),
                      (unsigned long)((tx % 1000000) / 100));
    _ui_richPrint(RICH_PAD, y2, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, color_white,
                  "%d dBm", (int)last_state.rssi);
}

#if defined(CONFIG_TETRA) || defined(CONFIG_ACARS) || defined(CONFIG_AIS)
/**
 * Text on one line of a given width: as it is if it fits, else scrolling
 * left one character every 150 ms, after a second at its start. Never
 * wraps, as gfx_print() would.
 */
static void _ui_richScroll(int16_t x, int16_t y, int16_t w, fontSize_t font,
                           color_t color, const char *text)
{
    static char shown[128]; // Text being scrolled
    static size_t pos;
    static long long next;
    long long now = getTick();

    if(strncmp(shown, text, sizeof(shown) - 1) != 0)
    {
        strncpy(shown, text, sizeof(shown) - 1);
        shown[sizeof(shown) - 1] = '\0';
        pos = 0;
        next = now + 1000;
    }

    size_t len = strlen(shown);
    char line[64];
    if(gfx_getTextWidth(font, shown) <= w)
    {
        _ui_richPrint(x, y, font, TEXT_ALIGN_LEFT, color, "%s", shown);
        return;
    }

    // The text and a gap, looped, from pos on, as much as fits
    const size_t loop = len + 4;
    size_t n = 0;
    while(n + 1 < sizeof(line))
    {
        size_t k = (pos + n) % loop;
        line[n] = (k < len) ? shown[k] : ' ';
        line[n + 1] = '\0';
        if(gfx_getTextWidth(font, line) > w)
        {
            line[n] = '\0';
            break;
        }
        n++;
    }
    _ui_richPrint(x, y, font, TEXT_ALIGN_LEFT, color, "%s", line);

    if(now >= next)
    {
        pos = (pos + 1) % loop;
        next = now + ((pos == 0) ? 1000 : 150);
    }
}

#endif

#ifdef CONFIG_ACARS
/**
 * ACARS: messages and errors, the frequency, and the last message:
 * registration, flight, label, age and its text scrolling.
 */
static void _ui_richAcars(void)
{
    static const color_t sky = { 95, 176, 255, 255 };
    struct acarsStats st;
    acars_getStats(&st);
    const int16_t y = RICH_INFO_Y;

    int16_t x = _ui_richMemTag(y);
    _ui_richPrint(x, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, color_white,
                  "Msgs %lu", (unsigned long)st.frames);
    _ui_richPrint(RICH_PAD, y + 3, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, rich_grey,
                  "bad %lu", (unsigned long)st.crcErrors);

    _ui_richFrequency(33);

    struct acarsMsg m;
    const int16_t ly = 57, by = 68;
    gfx_drawRect((point_t){ RICH_PAD, by }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD,
                 17, rich_dark, false);
    if(acars_get(&m, 1) == 0)
    {
        _ui_richPrint(RICH_PAD, ly, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                      "Listening, %lu syncs", (unsigned long)st.syncs);
        _ui_richPrint(RICH_PAD + 3, by + 4, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_grey, "No message yet");
    }
    else
    {
        char ago[8];
        _ui_ago(ago, sizeof(ago), m.tick);
        uint16_t w = _ui_richPrint(RICH_PAD, ly, FONT_SIZE_6PT,
                                   TEXT_ALIGN_LEFT, color_white, "%s",
                                   m.address[0] ? m.address : "-");
        if(m.flight[0] != '\0')
            _ui_richPrint(RICH_PAD + w + 5, ly, FONT_SIZE_6PT,
                          TEXT_ALIGN_LEFT, yellow_fab413, "%s", m.flight);
        _ui_richPrint(RICH_PAD, ly, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, sky,
                      "%s %s", m.label, ago);

        const char *name = acars_labelName(m.label);
        _ui_richScroll(RICH_PAD + 3, by + 4,
                       CONFIG_SCREEN_WIDTH - 2 * RICH_PAD - 6, FONT_SIZE_6PT,
                       color_white,
                       m.text[0] ? m.text : (name ? name : "(no text)"));
    }

    _ui_richMeter(89, sky, false, false);
}

/**
 * ACARS message page, in place of the VFO or memory screen: one message
 * of the history whole, UP and DOWN to go through them.
 *
 * @return true if it was drawn.
 */
static bool _ui_drawAcarsPage(ui_state_t *ui_state)
{
    if((last_state.channel.mode != OPMODE_ACARS) || !ui_state->acars_page)
        return false;

    static UI_BSS struct acarsMsg list[ACARS_MAX_MSGS];
    int n = acars_get(list, ACARS_MAX_MSGS);
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    point_t pos = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };

    _ui_drawMainTop(ui_state);
    if(n == 0)
    {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "ACARS");
        pos.y += 2 * step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "No message yet");
        return true;
    }
    if(ui_state->acars_sel >= n)
        ui_state->acars_sel = n - 1;
    const struct acarsMsg *m = &list[ui_state->acars_sel];

    char ago[8];
    _ui_ago(ago, sizeof(ago), m->tick);
    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "ACARS %d/%d",
              ui_state->acars_sel + 1, n);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, yellow_fab413, "%s ago", ago);

    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s %s",
              m->address[0] ? m->address : "-", m->flight);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, info_grey, "%s, blk %c",
              m->downlink ? "down" : "up", m->blockId);

    const char *name = acars_labelName(m->label);
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%s %s%s", m->label,
              name ? name : "", m->repaired ? ", fixed" : "");

    // The text, wrapped over the rest of the screen
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s",
              m->text[0] ? m->text : "(no text)");
    return true;
}
#endif

#ifdef CONFIG_AIS
/**
 * A latitude or longitude in 1/10000 minute as degrees with four decimals
 * and a hemisphere letter: "48.8566N".
 */
static void _ui_aisCoord(char *buf, size_t len, int32_t v, char pos, char neg)
{
    long long a = (v < 0) ? -(long long)v : v;
    unsigned deg = (unsigned)(a / 600000);
    unsigned frac = (unsigned)((a % 600000) * 10000 / 600000);
    sniprintf(buf, len, "%u.%04u%c", deg, frac, (v < 0) ? neg : pos);
}

/**
 * The name of a station, its MMSI until a static report names it.
 */
static void _ui_aisName(char *buf, size_t len, const struct aisShip *s)
{
    if(s->name[0] != '\0')
        sniprintf(buf, len, "%s", s->name);
    else
        sniprintf(buf, len, "%09lu", (unsigned long)s->mmsi);
}

/**
 * One line with what is known of a station's position and voyage.
 */
static void _ui_aisSummary(char *buf, size_t len, const struct aisShip *s)
{
    size_t o = 0;
    buf[0] = '\0';
    if(s->posValid)
    {
        char lat[16], lon[16];
        _ui_aisCoord(lat, sizeof(lat), s->lat, 'N', 'S');
        _ui_aisCoord(lon, sizeof(lon), s->lon, 'E', 'W');
        o += sniprintf(buf + o, len - o, "%s %s  ", lat, lon);
    }
    if(o < len && s->sog < 1023)
        o += sniprintf(buf + o, len - o, "%u.%ukn ", s->sog / 10, s->sog % 10);
    if(o < len && s->cog < 3600)
        o += sniprintf(buf + o, len - o, "%03u ", s->cog / 10);
    const char *st = ais_navStatusName(s->navStatus);
    if(o < len && st != NULL)
        o += sniprintf(buf + o, len - o, " %s ", st);
    if(o < len && s->destination[0] != '\0')
        o += sniprintf(buf + o, len - o, " >%s", s->destination);
    if(o == 0)
        sniprintf(buf, len, "Message %u", s->msgType);
}

static const char *_ui_aisClass(uint8_t cls)
{
    static const char *const names[] = { "Class A", "Class B", "Base",
                                         "AtoN",    "SAR",     "Station" };
    return (cls < 6) ? names[cls] : "";
}

/**
 * AIS: stations and reports, the frequency, and the newest station: its
 * name, class, age and position and voyage scrolling.
 */
static void _ui_richAis(void)
{
    struct aisStats st;
    ais_getStats(&st);
    const int16_t y = RICH_INFO_Y;

    int16_t x = _ui_richMemTag(y);
    x += _ui_richPrint(x, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, color_white,
                       "Ships %lu", (unsigned long)st.ships);

    // Reports and bad frames at the right, shorter if they do not fit
    char counts[24];
    const int16_t room = CONFIG_SCREEN_WIDTH - RICH_PAD - x - 6;
    sniprintf(counts, sizeof(counts), "rx %lu fix %lu bad %lu",
              (unsigned long)st.frames, (unsigned long)st.repaired,
              (unsigned long)st.crcErrors);
    if(gfx_getTextWidth(FONT_SIZE_6PT, counts) > room)
        sniprintf(counts, sizeof(counts), "%lu/%lu/%lu",
                  (unsigned long)st.frames, (unsigned long)st.repaired,
                  (unsigned long)st.crcErrors);
    _ui_richPrint(RICH_PAD, y + 3, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, rich_grey,
                  "%s", counts);

    _ui_richFrequency(33);

    struct aisShip s;
    const int16_t ly = 57, by = 68;
    gfx_drawRect((point_t){ RICH_PAD, by }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD,
                 17, rich_dark, false);
    if(ais_get(&s, 1) == 0)
    {
        _ui_richPrint(RICH_PAD, ly, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                      "Listening, %lu syncs", (unsigned long)st.syncs);
        _ui_richPrint(RICH_PAD + 3, by + 4, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_grey, "No station yet");
    }
    else
    {
        char ago[8], name[24], line[96];
        _ui_ago(ago, sizeof(ago), s.tick);
        _ui_aisName(name, sizeof(name), &s);
        char tag[16];
        sniprintf(tag, sizeof(tag), "%s %s", _ui_aisClass(s.cls), ago);
        // The name, cut to leave room for the tag (one scrolling text per
        // screen: _ui_richScroll() has a single state)
        int16_t room = CONFIG_SCREEN_WIDTH - 2 * RICH_PAD - 4
                     - gfx_getTextWidth(FONT_SIZE_6PT, tag);
        size_t nl = strlen(name);
        while(nl > 0 && gfx_getTextWidth(FONT_SIZE_6PT, name) > room)
            name[--nl] = '\0';
        _ui_richPrint(RICH_PAD, ly, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      color_white, "%s", name);
        _ui_richPrint(RICH_PAD, ly, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, ais_teal,
                      "%s", tag);

        _ui_aisSummary(line, sizeof(line), &s);
        _ui_richScroll(RICH_PAD + 3, by + 4,
                       CONFIG_SCREEN_WIDTH - 2 * RICH_PAD - 6, FONT_SIZE_6PT,
                       color_white, line);
    }

    _ui_richMeter(89, ais_teal, false, false);
}

/**
 * AIS station page, in place of the VFO or memory screen: one station of
 * the table whole, UP and DOWN to go through them, newest first.
 *
 * @return true if it was drawn.
 */
static bool _ui_drawAisPage(ui_state_t *ui_state)
{
    if((last_state.channel.mode != OPMODE_AIS) || !ui_state->ais_page)
        return false;

    static UI_BSS struct aisShip list[AIS_MAX_SHIPS];
    int n = ais_get(list, AIS_MAX_SHIPS);
    const fontSize_t font = FONT_SIZE_6PT;
    const int16_t step = 11;
    point_t pos = { layout.horizontal_pad, (int16_t)(layout.top_h + 11) };

    _ui_drawMainTop(ui_state);
    if(n == 0)
    {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "AIS");
        pos.y += 2 * step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "No station yet");
        return true;
    }
    if(ui_state->ais_sel >= n)
        ui_state->ais_sel = n - 1;
    const struct aisShip *s = &list[ui_state->ais_sel];

    char ago[8], buf[48];
    _ui_ago(ago, sizeof(ago), s->tick);
    gfx_print(pos, font, TEXT_ALIGN_LEFT, yellow_fab413, "AIS %d/%d",
              ui_state->ais_sel + 1, n);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, yellow_fab413, "%s ago", ago);

    pos.y += step;
    _ui_aisName(buf, sizeof(buf), s);
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%.20s", buf);

    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%09lu",
              (unsigned long)s->mmsi);
    gfx_print(pos, font, TEXT_ALIGN_RIGHT, ais_teal, "%s %s",
              _ui_aisClass(s->cls), s->callsign);

    if(s->posValid)
    {
        char lat[16], lon[16];
        _ui_aisCoord(lat, sizeof(lat), s->lat, 'N', 'S');
        _ui_aisCoord(lon, sizeof(lon), s->lon, 'E', 'W');
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s %s", lat, lon);
    }

    size_t o = 0;
    buf[0] = '\0';
    if(s->sog < 1023)
        o += sniprintf(buf + o, sizeof(buf) - o, "SOG %u.%ukn ", s->sog / 10,
                       s->sog % 10);
    if(o < sizeof(buf) && s->cog < 3600)
        o += sniprintf(buf + o, sizeof(buf) - o, "COG %03u ", s->cog / 10);
    if(o < sizeof(buf) && s->heading < 360)
        o += sniprintf(buf + o, sizeof(buf) - o, "HDG %03u", s->heading);
    if(o > 0)
    {
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", buf);
    }

    const char *st = ais_navStatusName(s->navStatus);
    const char *type = ais_shipTypeName(s->shipType);
    if(st != NULL || type != NULL)
    {
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%s%s%s",
                  st ? st : "", (st && type) ? ", " : "", type ? type : "");
    }

    if(s->destination[0] != '\0')
    {
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, ">%s",
                  s->destination);
    }

    if(s->length != 0 || s->draught != 0)
    {
        pos.y += step;
        gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey,
                  "%ux%um draught %u.%um", s->length, s->beam,
                  s->draught / 10, s->draught % 10);
    }

    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, info_grey, "%u reports, msg %u",
              s->reports, s->msgType);
    return true;
}
#endif

#ifdef CONFIG_TETRA
/**
 * TETRA: the cell, the frequency, the signal quality and the last SDS, or
 * the call in a card.
 */
static void _ui_richTetra(void)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    const int16_t y = RICH_INFO_Y;

    int16_t x = _ui_richMemTag(y);
    if(ts.cell)
    {
        uint16_t w = _ui_richPrint(x, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT,
                                   color_white, "%u-%u", ts.mcc, ts.mnc);

        // Air encryption on the right, the location area only if it fits
        // before it
        int16_t end = CONFIG_SCREEN_WIDTH - RICH_PAD;
        if(ts.airEncryption)
            end -= _ui_richPrint(RICH_PAD, y + 3, FONT_SIZE_6PT,
                                 TEXT_ALIGN_RIGHT, rich_grey, "AIE") + 4;
        if(ts.sysinfo)
        {
            char la[12];
            sniprintf(la, sizeof(la), "LA%u", ts.la);
            int16_t lx = x + w + 4;
            if(lx + gfx_getTextWidth(FONT_SIZE_6PT, la) <= end)
                _ui_richPrint(lx, y + 3, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                              rich_grey, "%s", la);
        }
    }
    else
    {
        _ui_richPrint(x, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, rich_grey,
                      "Searching");
    }

    if(_ui_tetraCallShown(&ts))
    {
        char dst[16], talker[16], detail[20];
        const char *state;
        color_t sc;
        sniprintf(dst, sizeof(dst), ts.callDst ? "%lu" : "-",
                  (unsigned long)ts.callDst);
        sniprintf(talker, sizeof(talker), ts.talker ? "%lu" : "-",
                  (unsigned long)ts.talker);
        if(ts.callEncrypted)
        {
            state = "Encrypted";
            sc = rich_red;
        }
        else if(ts.callUnknown)
        {
            state = "Encrypted?";
            sc = rich_orange;
        }
        else if(ts.callFiltered)
        {
            state = "Filtered";
            sc = rich_grey;
        }
        else if(ts.voice)
        {
            // "+1": a second clear call meanwhile, not played
            state = ts.callOther ? "Voice +1" : "Voice";
            sc = rich_green;
        }
        else
        {
            state = "Idle";
            sc = rich_grey;
        }
        // Followed to another carrier of the cell: say which
        if(ts.followHz != 0)
            sniprintf(detail, sizeof(detail), "call %u c%u", ts.callId,
                      ts.followCarrier);
        else if(ts.callId != 0)
            sniprintf(detail, sizeof(detail), "call %u", ts.callId);
        else
            detail[0] = '\0';

        static const color_t bg = { 15, 42, 46, 255 };
        _ui_richCard(31, 60, bg, rich_teal, ts.callGroup ? "G" : "I", dst,
                     talker, "", sc, state, detail);
        _ui_richMeter(93, rich_teal, false, false);
        return;
    }

    _ui_richFrequency(33);

    // Signal quality from the demodulator's phase error: 58% is noise.
    // Fixed places, so that the line does not move with the digits.
    const int16_t qy = 57;
    const int16_t ex = 72; // Phase error: room for "e58%" and "+9999Hz"
    _ui_richPrint(RICH_PAD, qy, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                  "Sig");
    _ui_richPrint(ex, qy, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white, "e%u%%",
                  ts.phaseErr);
    int hz = ts.carrierHz;
    if(hz > 9999)
        hz = 9999;
    if(hz < -9999)
        hz = -9999;
    _ui_richPrint(RICH_PAD, qy, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, color_white,
                  "%+dHz", hz);
    const int16_t bx = RICH_PAD + 22;
    const int16_t bw = ex - 4 - bx;
    int q = ((58 - (int)ts.phaseErr) * 100) / 48;
    if(q < 0)
        q = 0;
    if(q > 100)
        q = 100;
    color_t qc = (ts.phaseErr < 25) ? rich_green
               : ((ts.phaseErr < 40) ? yellow_fab413 : rich_red);
    gfx_drawRect((point_t){ bx, qy + 1 }, bw, 5, rich_off, true);
    gfx_drawRect((point_t){ bx, qy + 1 }, (bw * q) / 100, 5, qc, true);

    // Traffic of a call not signalled in clear, else the last SDS, else
    // no call
    const int16_t by = 68;
    gfx_drawRect((point_t){ RICH_PAD, by }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD,
                 17, rich_dark, false);
    long long age = getTick() - ts.sdsTime;
    if(ts.sync && ts.voice && ts.callUnknown)
    {
        gfx_drawRect((point_t){ RICH_PAD + 3, by + 6 }, 5, 5, rich_orange,
                     true);
        _ui_richPrint(RICH_PAD + 11, by + 4, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_orange, "Encrypted traffic, TN%u",
                      ts.lastTrafficTn);
    }
    else if(ts.sdsSrc != 0 && age < TETRA_SDS_SHOW_MS)
    {
        char sds[96];
        sniprintf(sds, sizeof(sds), "%lu: %s", (unsigned long)ts.sdsSrc,
                  ts.sds);
        _ui_richScroll(RICH_PAD + 3, by + 4,
                       CONFIG_SCREEN_WIDTH - 2 * RICH_PAD - 6, FONT_SIZE_6PT,
                       color_white, sds);
    }
    else
    {
        _ui_richPrint(RICH_PAD + 3, by + 4, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_grey, "No call");
    }

    _ui_richMeter(89, rich_teal, false, false);
}
#endif

#ifdef CONFIG_DMR_RX
/**
 * DMR: slots and colour code, the frequency and demodulator figures, or
 * the call in a card.
 */
static void _ui_richDmr(void)
{
    dmr_slot_info_t si;
    dmr_demod_status_t ds;
    int slot = _ui_dmrCallSlot(&si, &ds);
    bool ms = dmr_proto_is_ms_mode();
    const int16_t y = RICH_INFO_Y;

    // Slot pills: the one of the call shown filled
    int active = slot;
    int16_t x = _ui_richMemTag(y);
    for(int i = 0; i < 2 && !ms; i++)
    {
        char t[4];
        sniprintf(t, sizeof(t), "TS%d", i + 1);
        x = _ui_richBadge(x, y, 11, FONT_SIZE_6PT, t,
                          (i == active) ? rich_violet : rich_dark,
                          i == active) + 3;
    }

    int cc = -1;
    for(int i = 0; i < 2 && cc < 0; i++)
    {
        dmr_slot_info_t s;
        dmr_proto_get_slot(i, &s);
        cc = s.cc;
    }
    if(!ds.locked)
        _ui_richPrint(RICH_PAD, y + 3, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
                      rich_grey, "Searching");
    else if(cc >= 0)
        _ui_richPrint(RICH_PAD, y + 3, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
                      rich_grey, "%s CC%d", ds.bs ? "RPT" : (ms ? "MS" : "DM"),
                      cc);

    if(slot >= 0)
    {
        char dst[16], talker[16], id[12], detail[16];
        const char *state;
        color_t sc;
        sniprintf(dst, sizeof(dst), "%lu", (unsigned long)si.dst);
        if(si.alias[0] != '\0')
        {
            sniprintf(talker, sizeof(talker), "%.10s", si.alias);
            sniprintf(id, sizeof(id), "%lu", (unsigned long)si.src);
        }
        else
        {
            sniprintf(talker, sizeof(talker), "%lu", (unsigned long)si.src);
            id[0] = '\0';
        }
        if(si.encrypted)
        {
            state = "Encrypted";
            sc = rich_red;
        }
        else
        {
            state = "Voice";
            sc = rich_green;
        }
        // The talker's position, if sent, after the slot
        if(si.gps_valid)
            sniprintf(detail, sizeof(detail), "TS%d %s", slot + 1,
                      si.gps_grid);
        else
            sniprintf(detail, sizeof(detail), "TS%d", slot + 1);

        static const color_t bg = { 29, 24, 48, 255 };
        _ui_richCard(31, 60, bg, rich_violet, si.group ? "TG" : "ID", dst,
                     talker, id, sc, state, detail);
        _ui_richMeter(93, rich_violet, false, false);
        return;
    }

    _ui_richFrequency(33);
    _ui_richPrint(RICH_PAD, 57, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                  "Level %d  DC %+d  lost %lu%s", (int)ds.level, (int)ds.dc,
                  (unsigned long)ds.overruns, ds.inverted ? " inv" : "");
    // The latest LRRP position, else no call
    char fix[40];
    gfx_drawRect((point_t){ RICH_PAD, 68 }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD,
                 17, rich_dark, false);
    if(_ui_dmrLastFix(fix, sizeof(fix)))
    {
        int16_t bx = _ui_richBadge(RICH_PAD + 3, 71, 11, FONT_SIZE_6PT, "GPS",
                                   rich_violet, false);
        _ui_richPrint(bx + 4, 72, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
                      "%s", fix);
    }
    else
    {
        _ui_richPrint(RICH_PAD + 3, 72, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      rich_grey, "No call");
    }
    _ui_richMeter(89, rich_violet, false, false);
}
#endif

#ifdef CONFIG_M17
/**
 * M17: destination and CAN, the frequency, or the transmission in a card.
 */
static void _ui_richM17(ui_state_t *ui_state)
{
    rtxStatus_t rtx = rtx_getCurrentStatus();
    const int16_t y = RICH_INFO_Y;
    const char *dst = ui_state->edit_mode ? ui_state->new_callsign
                    : (strnlen(rtx.destination_address, 10) == 0)
                    ? currentLanguage->broadcast : rtx.destination_address;

    int16_t x = _ui_richMemTag(y);
    _ui_richPrint(x, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, color_white, "#%s",
                  dst);
    _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, rich_grey,
                  "CAN %u", last_state.settings.m17_can);

    if(rtx.lsfOk)
    {
        static const color_t bg = { 48, 30, 18, 255 };
        const char *extra = (rtx.M17_meta_text[0] != '\0') ? rtx.M17_meta_text
                          : (rtx.M17_refl[0] != '\0') ? rtx.M17_refl
                          : rtx.M17_link;
        char detail[16];
        sniprintf(detail, sizeof(detail), "%.13s", extra);
        _ui_richCard(31, 60, bg, rich_orange, "TO", rtx.M17_dst, rtx.M17_src,
                     "", rich_green, "Voice", detail);
        _ui_richMeter(93, rich_orange, false, false);
        return;
    }

    _ui_richFrequency(34);
    _ui_richPrint(RICH_PAD, 62, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, color_white,
                  "%d dBm", (int)last_state.rssi);
    _ui_richMeter(74, rich_orange, false, false);
}
#endif

/**
 * Bottom of the macro menu: the S-meter, with the squelch level and
 * threshold in FM and AM, where the macro menu sets them.
 */
void _ui_drawRichMacroBottom(void)
{
    const char *name;
    color_t accent = _ui_richModeColor(&name);
    bool analog = (last_state.channel.mode == OPMODE_FM);
    #ifdef CONFIG_AM
    analog = analog || (last_state.channel.mode == OPMODE_AM);
    #endif
    _ui_richMeter(CONFIG_SCREEN_HEIGHT - 31, accent, analog, analog);
}

/**
 * The main screen, VFO or memory.
 */
static void _ui_drawRichMain(ui_state_t *ui_state, bool mem)
{
    _ui_richTop(ui_state, _ui_callShown());

    switch(last_state.channel.mode)
    {
        #ifdef CONFIG_TETRA
        case OPMODE_TETRA:
            _ui_richTetra();
            return;
        #endif
        #ifdef CONFIG_DMR_RX
        case OPMODE_DMR:
            _ui_richDmr();
            return;
        #endif
        #ifdef CONFIG_M17
        case OPMODE_M17:
            _ui_richM17(ui_state);
            return;
        #endif
        #ifdef CONFIG_ACARS
        case OPMODE_ACARS:
            _ui_richAcars();
            return;
        #endif
        #ifdef CONFIG_AIS
        case OPMODE_AIS:
            _ui_richAis();
            return;
        #endif
        default:
            _ui_richAnalog(mem);
            _ui_richMeter(74, yellow_fab413, true, false);
            return;
    }
}
#endif
