/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR transmission (protocols/DMR/dmr_tx.c) checked against the receiver of
 * the same tree: the bursts straight into dmr_proto, and a whole call
 * through the modulator, dmr_demod and dmr_proto. The vocoder is replaced
 * by the stub below, which keeps the voice bursts dmr_proto hands it.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_tx.h"
#include "protocols/DMR/dmr_voice.h"
}

/* ---- Vocoder stub: the voice bursts received ---------------------------- */

struct VoiceBurst {
    int idx;
    uint8_t dibits[DMR_TX_BURST_DIBITS];
};

static std::vector<VoiceBurst> voiceRx;
static long long tick;

extern "C" {
long long getTick()
{
    return tick;
}

bool dmr_voice_available(void)
{
    return false;
}
void dmr_voice_set_sink(dmr_voice_sink_t)
{
}
void dmr_voice_reset(void)
{
}
void dmr_voice_burst(int, const uint8_t burst[132], int voice_idx, uint32_t)
{
    VoiceBurst b;
    b.idx = voice_idx;
    memcpy(b.dibits, burst, DMR_TX_BURST_DIBITS);
    voiceRx.push_back(b);
}
void dmr_voice_call_end(int)
{
}
void dmr_voice_set_encrypted(int, bool)
{
}
void dmr_voice_set_pref(int)
{
}
int dmr_voice_get_pref(void)
{
    return DMR_VOICE_PREF_AUTO;
}
int dmr_voice_get_playing(void)
{
    return -1;
}
void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us)
{
    *frames = 0;
    *avg_us = 0;
}
uint32_t dmr_voice_get_concealed(void)
{
    return 0;
}
bool dmr_voice_decode_next(void)
{
    return false;
}
int dmr_voice_queued(void)
{
    return 0;
}
void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops)
{
    *max_us = 0;
    *queue_drops = 0;
}
}

/* ---- A call -------------------------------------------------------------- */

static const uint32_t SRC = 2141234;
static const uint32_t TG = 214;
static const uint8_t CC = 1;
static const int SUPERFRAMES = 4;

/* AMBE frames of voice burst k, from a fixed pseudo random sequence */
static void ambeFor(int k, uint8_t ambe[3][DMR_TX_AMBE_BYTES])
{
    uint32_t x = 0x12345u + 7919u * (uint32_t)k;
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < DMR_TX_AMBE_BYTES; i++) {
            x = x * 1103515245u + 12345u;
            ambe[f][i] = (uint8_t)(x >> 16);
        }
    }
}

struct Burst {
    dmr_sync_t sync;
    uint8_t dibits[DMR_TX_BURST_DIBITS];
};

/* Wake-up, two headers, the superframes, the terminator */
static std::vector<Burst> buildCall(bool group)
{
    std::vector<Burst> call;
    dmr_tx_t tx;
    Burst b;

    dmr_tx_setup(&tx, SRC, TG, group, CC);
    dmr_tx_wakeup(&tx, 0xFFFFFFu, SRC, b.dibits);
    b.sync = DMR_SYNC_MS_DATA;
    call.push_back(b);
    for (int i = 0; i < 2; i++) {
        dmr_tx_header(&tx, b.dibits);
        call.push_back(b);
    }
    for (int k = 0; k < 6 * SUPERFRAMES; k++) {
        uint8_t ambe[3][DMR_TX_AMBE_BYTES];
        ambeFor(k, ambe);
        dmr_tx_voice(&tx, ambe, b.dibits);
        b.sync = (k % 6 == 0) ? DMR_SYNC_MS_VOICE : DMR_SYNC_NONE;
        call.push_back(b);
    }
    dmr_tx_terminator(&tx, b.dibits);
    b.sync = DMR_SYNC_MS_DATA;
    call.push_back(b);
    return call;
}

/* The AMBE dibits of a voice burst, around the centre field */
static bool sameVoice(const uint8_t a[DMR_TX_BURST_DIBITS],
                      const uint8_t b[DMR_TX_BURST_DIBITS])
{
    return memcmp(a, b, 54) == 0 && memcmp(&a[78], &b[78], 54) == 0;
}

/* The receiver saw the call: IDs, colour code, headers, embedded LC,
 * terminator, CSBK, and every voice burst as sent */
static void checkReceived(const std::vector<Burst> &call, bool group)
{
    dmr_slot_info_t si;
    dmr_proto_stats_t st;
    dmr_proto_get_slot(0, &si);
    dmr_proto_get_stats(&st);

    REQUIRE(dmr_proto_is_ms_mode());
    REQUIRE(si.ids_valid);
    REQUIRE(si.src == SRC);
    REQUIRE(si.dst == TG);
    REQUIRE(si.group == group);
    REQUIRE(si.cc == CC);
    REQUIRE(si.activity == DMR_ACT_IDLE); /* After the terminator */
    REQUIRE(st.csbk == 1);
    REQUIRE(st.lc_headers >= 1);
    REQUIRE(st.terminators == 1);
    REQUIRE(st.emb_lc_ok == SUPERFRAMES);
    REQUIRE(st.emb_lc_bad == 0);

    REQUIRE(voiceRx.size() == 6 * SUPERFRAMES);
    for (size_t k = 0; k < voiceRx.size(); k++) {
        INFO("voice burst " << k);
        REQUIRE(voiceRx[k].idx == (int)(k % 6));
        REQUIRE(sameVoice(voiceRx[k].dibits, call[3 + k].dibits));
    }
}

TEST_CASE("DMR TX bursts decode in the receiver", "[dmr][tx]")
{
    const bool group = GENERATE(true, false);
    INFO((group ? "group call" : "private call"));
    const std::vector<Burst> call = buildCall(group);

    dmr_proto_reset();
    voiceRx.clear();
    uint32_t t = 1000;
    for (const Burst &b : call) {
        dmr_frame_t f;
        memset(&f, 0, sizeof(f));
        f.sync = b.sync;
        f.bs = false;
        memcpy(f.burst, b.dibits, sizeof(f.burst));
        f.t_ms = t;
        t += 60;
        REQUIRE(dmr_proto_frame(&f) != DMR_FRAME_BAD);
    }
    checkReceived(call, group);
}

/* ---- The baseband -------------------------------------------------------- */

static const float GAIN = 1000.0f;
/* A burst every 60 ms: 132 symbols, then 156 silent ones */
static const int FRAME_SYMS = 288;

static void feed(const int16_t *x, int n)
{
    std::vector<float> f(x, x + n);
    dmr_demod_feed_disc(f.data(), n);
}

static void feedSilence(dmr_mod_t *mod, int syms)
{
    std::vector<int16_t> out((size_t)syms * DMR_TX_SPS);
    dmr_mod_dibits(mod, NULL, syms, out.data());
    feed(out.data(), (int)out.size());
}

TEST_CASE("DMR TX call through the modulator and the demodulator",
          "[dmr][tx]")
{
    const std::vector<Burst> call = buildCall(true);
    dmr_mod_t mod;
    dmr_mod_init(&mod, GAIN, false);

    REQUIRE(dmr_demod_init());
    dmr_demod_reset();
    dmr_proto_reset();
    voiceRx.clear();

    feedSilence(&mod, 960); /* 200 ms */
    std::vector<int16_t> out(DMR_TX_BURST_DIBITS * DMR_TX_SPS);
    for (const Burst &b : call) {
        dmr_mod_dibits(&mod, b.dibits, DMR_TX_BURST_DIBITS, out.data());
        feed(out.data(), (int)out.size());
        feedSilence(&mod, FRAME_SYMS - DMR_TX_BURST_DIBITS);
    }

    /* Still locked on the terminator */
    dmr_demod_status_t ds;
    dmr_demod_get_status(&ds);
    REQUIRE(ds.locked);
    REQUIRE(ds.ms);
    REQUIRE_FALSE(ds.inverted);
    /* The outer symbols at three times the gain */
    REQUIRE(std::fabs(ds.level - 3.0f * GAIN) < 0.05f * 3.0f * GAIN);
    checkReceived(call, true);
}

TEST_CASE("DMR TX modulator levels", "[dmr][tx]")
{
    dmr_mod_t mod;
    std::vector<int16_t> out(40 * DMR_TX_SPS);
    std::vector<uint8_t> run(40);

    SECTION("silence is no deviation")
    {
        dmr_mod_init(&mod, GAIN, false);
        dmr_mod_dibits(&mod, NULL, 40, out.data());
        for (int16_t s : out)
            REQUIRE(s == 0);
    }
    SECTION("a run of a symbol settles at the gain times its value")
    {
        const bool invert = GENERATE(false, true);
        /* Dibit 00 = +1, 01 = +3, 10 = -1, 11 = -3 */
        const uint8_t dibit = GENERATE(0, 1, 2, 3);
        static const float value[4] = { 1.0f, 3.0f, -1.0f, -3.0f };
        INFO("dibit " << (int)dibit << (invert ? " inverted" : ""));
        dmr_mod_init(&mod, GAIN, invert);
        std::fill(run.begin(), run.end(), dibit);
        dmr_mod_dibits(&mod, run.data(), 40, out.data());
        const float want = (invert ? -1.0f : 1.0f) * value[dibit] * GAIN;
        /* Past the filter's span: the mean of a symbol at the level, its
         * samples within the ripple of the RRC's phases (not a Nyquist
         * filter on its own) */
        const size_t first = 10 * DMR_TX_SPS;
        float sum = 0.0f;
        for (size_t i = first; i < out.size(); i++) {
            REQUIRE(std::fabs(out[i] - want) <= 0.05f * std::fabs(want));
            sum += out[i];
        }
        const float mean = sum / (float)(out.size() - first);
        REQUIRE(std::fabs(mean - want) <= 0.01f * std::fabs(want));
    }
    SECTION("peaks saturate")
    {
        dmr_mod_init(&mod, 20000.0f, false);
        std::fill(run.begin(), run.end(), 1); /* +3: 60000 */
        dmr_mod_dibits(&mod, run.data(), 40, out.data());
        REQUIRE(out.back() == 32767);
        std::fill(run.begin(), run.end(), 3); /* -3 */
        dmr_mod_dibits(&mod, run.data(), 40, out.data());
        REQUIRE(out.back() == -32768);
    }
}
