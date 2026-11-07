/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/cps_io.h"
#include "interfaces/delays.h"
#include <stdio.h>
#include <stdint.h>
#include "ui/ui_default.h"
#include <string.h>
#include "ui/ui_strings.h"
#include "core/utils.h"
#include "ui/utils.h"
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraSds.h"
#include "protocols/TETRA/TetraSpeech.h"

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
 * Check whether a TETRA call takes over the main screen.
 */
static bool _ui_tetraCallShown(const struct tetraStatus *ts)
{
    return ts->sync && ts->call;
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

/* A DMR call stays on the main screen this long after its last burst */
#define DMR_CALL_SHOW_MS 1500

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
              "Skip unk %lu enc %lu dat %lu", (unsigned long)ss.unknown,
              (unsigned long)ss.encrypted, (unsigned long)ss.data);
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
    static struct tetraSds list[TETRA_SDS_MAX];
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
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_grey, "%s, %u/%u parts",
                  kind, m->partsGot, m->parts);
    else
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_grey, "%s", kind);

    // Message, wrapped over the rest of the screen
    pos.y += step;
    gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", m->text);
}
#endif

#ifdef CONFIG_UI_RICH_MAIN
static void _ui_drawRichMain(ui_state_t *ui_state, bool mem);
#endif

void _ui_drawMainVFO(ui_state_t* ui_state)
{
    gfx_clearScreen();

    #ifdef CONFIG_UI_RICH_MAIN
    #ifdef CONFIG_TETRA
    bool tetraPage = (last_state.channel.mode == OPMODE_TETRA)
                  && (ui_state->tetra_page != TETRA_PAGE_MAIN);
    #else
    bool tetraPage = false;
    #endif
    if(!tetraPage)
    {
        _ui_drawRichMain(ui_state, false);
        return;
    }
    #endif

    _ui_drawMainTop(ui_state);

    #ifdef CONFIG_TETRA
    if(last_state.channel.mode == OPMODE_TETRA)
    {
        if(ui_state->tetra_page == TETRA_PAGE_DEBUG)
        {
            _ui_drawTetraDebug();
            return;
        }
        if(ui_state->tetra_page == TETRA_PAGE_SDS)
        {
            _ui_drawTetraSds(ui_state);
            return;
        }
    }
    #endif
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

    // Range: first and last frequency, span in the middle
    int16_t y = fh + 1;
    _ui_spectrumFreq(buf, sizeof(buf), start, step);
    gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_LEFT, color_white, buf);
    _ui_spectrumFreq(buf, sizeof(buf), end, step);
    gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_RIGHT, color_white, buf);
    freq_t span = step * SPECTRUM_POINTS;
    if (span >= 1000000)
        snprintf(buf, sizeof(buf), "%luM", (unsigned long)(span / 1000000));
    else
        snprintf(buf, sizeof(buf), "%luk", (unsigned long)(span / 1000));
    gfx_print((point_t){ 0, y }, font, TEXT_ALIGN_CENTER, yellow_fab413, buf);

    // Graph area
    const int16_t top = y + 3;
    const int16_t bottom = CONFIG_SCREEN_HEIGHT - 2 * (fh + 2) - 2;
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

    // Marker frequency and level
    y = bottom + fh + 2;
    _ui_spectrumFreq(buf, sizeof(buf), marker, step);
    gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_LEFT, yellow_fab413, buf);
    if (ui_state->spec_sweeps > 0)
        gfx_print((point_t){ pad, y }, font, TEXT_ALIGN_RIGHT, color_white,
                  "%d dBm", (int)markerRssi);

    // Keys
    y += fh + 2;
    gfx_print((point_t){ 0, y }, font, TEXT_ALIGN_CENTER, color_grey,
              "PTT:VFO 0:peak *#:zoom");
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
    uint16_t w = gfx_getTextWidth(font, text) + 6;
    gfx_drawRect((point_t){ x, y }, w, h, color, filled);
    // Centred, a row higher: the glyphs look low otherwise
    int16_t ty = y + (h - _ui_richCap(font)) / 2 - 1;
    _ui_richPrint(x + 3, ty, font, TEXT_ALIGN_LEFT,
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
    if(sqlLabel)
        _ui_richPrint(RICH_PAD, segY + (segH - _ui_richCap(FONT_SIZE_8PT)) / 2,
                      FONT_SIZE_8PT, TEXT_ALIGN_LEFT, accent, "SQ%u",
                      last_state.settings.sqlLevel);
    else if(s <= 9)
        _ui_richPrint(RICH_PAD, ty, FONT_SIZE_10PT, TEXT_ALIGN_LEFT, accent,
                      "S%u", s);
    else
        _ui_richPrint(RICH_PAD, ty, FONT_SIZE_10PT, TEXT_ALIGN_LEFT, accent,
                      "+%u", (s - 9) * 10);

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
            || last_state.channel.mode == OPMODE_DMR)
        sniprintf(mid, sizeof(mid), "RX only");
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
 * FM and AM: tone and squelch, the frequency, the TX frequency or channel
 * name and the level.
 */
static void _ui_richAnalog(bool mem)
{
    const int16_t y = RICH_INFO_Y;
    if(mem)
        _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, rich_grey,
                      "M%03u", last_state.channel_index + 1);
    else
        _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, rich_grey,
                      "VFO");

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
    _ui_richPrint(0, y, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, color_white, "%s",
                  mid);
    _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, rich_grey,
                  "SQ %u", last_state.settings.sqlLevel);

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

    if(ts.cell)
    {
        uint16_t w = _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT,
                                   color_white, "%u-%u", ts.mcc, ts.mnc);
        if(ts.sysinfo)
            _ui_richPrint(RICH_PAD + w + 6, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT,
                          rich_grey, "LA %u", ts.la);
        if(ts.airEncryption)
            _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT,
                          rich_grey, "AIE");
    }
    else
    {
        _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, rich_grey,
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
        else if(ts.voice)
        {
            state = "Voice";
            sc = rich_green;
        }
        else
        {
            state = "Idle";
            sc = rich_grey;
        }
        if(ts.callId != 0)
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

    // Signal quality from the demodulator's phase error: 58% is noise
    const int16_t qy = 57;
    uint16_t lw = _ui_richPrint(RICH_PAD, qy, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                                rich_grey, "Sig");
    char fig[16];
    sniprintf(fig, sizeof(fig), "e%u%% %+dHz", ts.phaseErr, ts.carrierHz);
    uint16_t fw = _ui_richPrint(RICH_PAD, qy, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
                                color_white, "%s", fig);
    int16_t bx = RICH_PAD + lw + 4;
    int16_t bw = CONFIG_SCREEN_WIDTH - RICH_PAD - fw - 4 - bx;
    int q = ((58 - (int)ts.phaseErr) * 100) / 48;
    if(q < 0)
        q = 0;
    if(q > 100)
        q = 100;
    color_t qc = (ts.phaseErr < 25) ? rich_green
               : ((ts.phaseErr < 40) ? yellow_fab413 : rich_red);
    gfx_drawRect((point_t){ bx, qy + 1 }, bw, 5, rich_off, true);
    gfx_drawRect((point_t){ bx, qy + 1 }, (bw * q) / 100, 5, qc, true);

    // Last SDS, else no call
    const int16_t by = 68;
    gfx_drawRect((point_t){ RICH_PAD, by }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD,
                 17, rich_dark, false);
    long long age = getTick() - ts.sdsTime;
    if(ts.sdsSrc != 0 && age < TETRA_SDS_SHOW_MS)
    {
        _ui_richPrint(RICH_PAD + 3, by + 4, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
                      color_white, "%lu: %.16s", (unsigned long)ts.sdsSrc,
                      ts.sds);
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
    int16_t x = RICH_PAD;
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
        _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, rich_grey,
                      "Searching");
    else if(cc >= 0)
        _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, rich_grey,
                      "%s CC%d", ds.bs ? "RPT" : (ms ? "MS" : "DM"), cc);

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
    gfx_drawRect((point_t){ RICH_PAD, 68 }, CONFIG_SCREEN_WIDTH - 2 * RICH_PAD,
                 17, rich_dark, false);
    _ui_richPrint(RICH_PAD + 3, 72, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, rich_grey,
                  "No call");
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

    _ui_richPrint(RICH_PAD, y, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, color_white,
                  "#%s", dst);
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
        default:
            _ui_richAnalog(mem);
            _ui_richMeter(74, yellow_fab413, true, false);
            return;
    }
}
#endif
