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

void _ui_drawMainVFO(ui_state_t* ui_state)
{
    gfx_clearScreen();
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
