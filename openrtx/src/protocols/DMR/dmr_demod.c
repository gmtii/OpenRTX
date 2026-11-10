/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_demod.h"
#include "dmr_proto.h"
#include "dmr_port.h"

#include <math.h>
#include <string.h>

#define FS 48000
#define SPS 10    /* samples per symbol */
#define RING 8192 /* matched-filter output history (power of two) */
#define RING_MASK (RING - 1)

#define CH_TAPS 41  /* channel filter */
#define CH_CUTOFF_HZ 7000.0f
#define RRC_TAPS 81 /* +-4 symbols */
#define RRC_ALPHA 0.2f

/* Burst geometry (symbols). Sync occupies burst symbols 54..77; tsync below
 * is the sample index of the CENTRE of symbol 77, the last sync symbol. */
#define SYM_SYNC_LAST 77
#define SYM_BURST 132
#define SYM_CACH 12
#define PERIOD_30MS (144 * SPS)
#define PERIOD_60MS (288 * SPS)

#define RHO_ACQUIRE 0.90f /* search mode: strict (noise false-alarm rate) */
#define RHO_TRACK 0.78f   /* locked: sync expected at a known place */
#define REFINE 4          /* +-samples searched around the expected sync */
#define PEAK_HOLD 6       /* samples to keep looking for a better peak */
#define BAD_MAX_BS 12     /* consecutive undecodable bursts before unlock */
#define BAD_MAX_MS 6
#define POL_FLIP_NO_OK \
    24 /* bursts with a sync but no validation before trying the other polarity */

/* Sync patterns (ETSI TS 102 361-1 9.1.1), 24 dibits, MSB first. */
static const uint64_t k_sync_hex[DMR_SYNC_COUNT] = {
    0,
    0x755FD7DF75F7ull, /* BS voice */
    0xDFF57D75DF5Dull, /* BS data */
    0x7F7D5DD57DFDull, /* MS voice */
    0xD5D7F77FD757ull, /* MS data */
    0x5D577F7757FFull, /* TS1 voice (direct) */
    0xF7FDD5DDFD55ull, /* TS1 data */
    0x7DFFD5F55D5Full, /* TS2 voice */
    0xD7557F5FF7F5ull, /* TS2 data */
};

typedef enum { FAM_BS, FAM_MS, FAM_DIRECT } family_t;

static float s_sync_sign[DMR_SYNC_COUNT]
                        [24]; /* +1 / -1 (sync uses only the outer symbols) */
static float s_sync_mean[DMR_SYNC_COUNT];
static float s_sync_den[DMR_SYNC_COUNT]; /* sum (s - mean)^2 */

static float s_ch_taps[CH_TAPS];
static float s_rrc_taps[RRC_TAPS];

/* doubled rings for the FIRs (contiguous window, no shifting) */
static float s_ch_i[2 * CH_TAPS], s_ch_q[2 * CH_TAPS];
static int s_ch_idx;
static float s_mf_hist[2 * RRC_TAPS];
static int s_mf_idx;
static uint32_t s_rot_phase;
static float s_prev_i, s_prev_q;

/* Matched-filter output (PSRAM on the C62, cleared by dmr_demod_reset()) */
static EXT_RAM_BSS_ATTR float s_ring_mem[RING];
static float *s_ring;
static uint32_t s_n; /* samples written to s_ring */

/* framer */
static bool s_locked;
static family_t s_fam;
static uint32_t s_t_exp; /* expected tsync of the next burst */
static float s_level;    /* A: signed outer-symbol amplitude */
static float s_dc;
static int s_bad;
static bool s_first_frame;
/* search peak tracking */
static bool s_cand;
static float s_cand_rho;
static uint32_t s_cand_t;
static int s_cand_sync;
static int s_cand_hold;

static uint32_t s_syncs;
static volatile uint32_t s_overruns;
/* Assumed symbol polarity (+1 normal). It cannot be read off the sync: the
 * voice and data sync words are exact complements, so an inverted data burst
 * looks like a normal voice burst. It is found by elimination instead: if
 * bursts keep arriving with a sync but never decode, flip and re-acquire. */
/* This board's rtl_source delivers conjugated IQ (the channel sits at
 * -12 kHz and FM deviation comes out negated), confirmed on the radio: the
 * receiver locked and decoded with "INVERTED". Starting with that polarity
 * saves the ~0.7 s the elimination search needs; it still flips on its own
 * if a different front end ever delivers the other orientation. */
#ifndef DMR_DEFAULT_POLARITY
#define DMR_DEFAULT_POLARITY (-1.0f)
#endif
static float s_pol = DMR_DEFAULT_POLARITY;
static int s_pol_bad;
DMR_LOCK_DECLARE(s_lock);
static dmr_demod_status_t s_pub;

/* ------------------------------------------------------------------------- */

static void design_filters(void)
{
    const float pi = 3.14159265358979f;
    float sum = 0.0f;

    for (int i = 0; i < CH_TAPS; i++) {
        float m = (float)i - (CH_TAPS - 1) / 2.0f;
        float x = 2.0f * CH_CUTOFF_HZ / FS;
        float sinc = (m == 0.0f) ? x : sinf(pi * x * m) / (pi * m);
        float w = 0.42f - 0.5f * cosf(2.0f * pi * i / (CH_TAPS - 1))
                + 0.08f * cosf(4.0f * pi * i / (CH_TAPS - 1));
        s_ch_taps[i] = sinc * w;
        sum += s_ch_taps[i];
    }
    for (int i = 0; i < CH_TAPS; i++) {
        s_ch_taps[i] /= sum;
    }

    sum = 0.0f;
    for (int i = 0; i < RRC_TAPS; i++) {
        float t = ((float)i - (RRC_TAPS - 1) / 2.0f) / SPS; /* in symbols */
        float a = RRC_ALPHA, h;
        if (fabsf(t) < 1e-6f) {
            h = 1.0f - a + 4.0f * a / pi;
        } else if (fabsf(fabsf(t) - 1.0f / (4.0f * a)) < 1e-6f) {
            h = a / sqrtf(2.0f)
              * ((1.0f + 2.0f / pi) * sinf(pi / (4.0f * a))
                 + (1.0f - 2.0f / pi) * cosf(pi / (4.0f * a)));
        } else {
            h = (sinf(pi * t * (1.0f - a))
                 + 4.0f * a * t * cosf(pi * t * (1.0f + a)))
              / (pi * t * (1.0f - (4.0f * a * t) * (4.0f * a * t)));
        }
        s_rrc_taps[i] = h;
        sum += h;
    }
    for (int i = 0; i < RRC_TAPS; i++) {
        s_rrc_taps[i] /=
            sum; /* unity DC gain: output stays in input units (Hz) */
    }

    for (int p = 1; p < DMR_SYNC_COUNT; p++) {
        float m = 0.0f, d = 0.0f;
        for (int k = 0; k < 24; k++) {
            unsigned dib = (unsigned)((k_sync_hex[p] >> (46 - 2 * k)) & 3u);
            s_sync_sign[p][k] = (dib == 1u) ? 1.0f :
                                              -1.0f; /* 01 = +3, 11 = -3 */
            m += s_sync_sign[p][k];
        }
        m /= 24.0f;
        for (int k = 0; k < 24; k++) {
            d += (s_sync_sign[p][k] - m) * (s_sync_sign[p][k] - m);
        }
        s_sync_mean[p] = m;
        s_sync_den[p] = d;
    }
}

bool dmr_demod_init(void)
{
    if (s_ring == NULL) {
        s_ring = s_ring_mem;
    }
    design_filters();
    dmr_demod_reset();
    return true;
}

void dmr_demod_reset(void)
{
    memset(s_ch_i, 0, sizeof(s_ch_i));
    memset(s_ch_q, 0, sizeof(s_ch_q));
    memset(s_mf_hist, 0, sizeof(s_mf_hist));
    s_ch_idx = s_mf_idx = 0;
    s_rot_phase = 0;
    s_prev_i = 1.0f;
    s_prev_q = 0.0f;
    if (s_ring != NULL) {
        memset(s_ring, 0, RING * sizeof(float));
    }
    s_n = 0;
    s_locked = false;
    s_cand = false;
    s_level = 0.0f;
    s_dc = 0.0f;
    s_bad = 0;
    s_syncs = 0;
    s_pol_bad =
        0; /* keep s_pol: the last polarity that worked is the best guess */
    dmr_proto_reset();
}

/* ------------------------------------------------------------------------- */
/* Sync correlation                                                           */
/* ------------------------------------------------------------------------- */

static inline float ring_at(uint32_t t)
{
    return s_ring[t & RING_MASK];
}

/* Pearson correlation of the 24 symbol-spaced samples ending at t against
 * pattern p. Also returns the least-squares level/offset for that fit. */
static float sync_rho(uint32_t t, int p, float *amp, float *dc)
{
    float x[24], m = 0.0f, s2 = 0.0f, c = 0.0f;

    for (int k = 0; k < 24; k++) {
        x[k] = ring_at(t - (uint32_t)(SPS * (23 - k)));
        m += x[k];
    }
    m /= 24.0f;
    for (int k = 0; k < 24; k++) {
        float d = x[k] - m;
        s2 += d * d;
        c += d * s_sync_sign[p][k];
    }
    if (s2 <= 1e-20f) {
        return 0.0f;
    }
    if (amp != NULL) {
        *amp = c / s_sync_den[p];
        *dc = m - *amp * s_sync_mean[p];
    }
    return c / sqrtf(s2 * s_sync_den[p]);
}

static family_t family_of(int p)
{
    if (p == DMR_SYNC_BS_VOICE || p == DMR_SYNC_BS_DATA) {
        return FAM_BS;
    }
    if (p == DMR_SYNC_MS_VOICE || p == DMR_SYNC_MS_DATA) {
        return FAM_MS;
    }
    return FAM_DIRECT;
}

static uint8_t slice(float x)
{
    float y = (x - s_dc) / s_level; /* outer symbols at +-1, inner at +-1/3 */
    if (y > 2.0f / 3.0f) {
        return 1;                   /* +3 */
    }
    if (y > 0.0f) {
        return 0; /* +1 */
    }
    if (y > -2.0f / 3.0f) {
        return 2; /* -1 */
    }
    return 3;     /* -3 */
}

static void publish(void)
{
    DMR_LOCK(s_lock);
    s_pub.locked = s_locked;
    s_pub.bs = s_locked && s_fam == FAM_BS;
    s_pub.ms = s_locked && s_fam == FAM_MS;
    s_pub.inverted = s_level < 0.0f;
    s_pub.level = fabsf(s_level);
    /* With inverted polarity (conjugated IQ) the discriminator's DC has the
     * opposite sign of the real RF offset: report the RF one. */
    s_pub.dc = (s_level < 0.0f) ? -s_dc : s_dc;
    s_pub.syncs = s_syncs;
    s_pub.now_ms = s_n / (FS / 1000);
    s_pub.overruns = s_overruns;
    DMR_UNLOCK(s_lock);
}

/* Cut the burst whose last sync symbol is centred at tsync and decode it. */
static int emit_frame(uint32_t tsync, dmr_sync_t sync)
{
    dmr_frame_t f;
    uint32_t t0 =
        tsync - (uint32_t)(SYM_SYNC_LAST * SPS); /* centre of burst symbol 0 */

    f.sync = sync;
    f.bs = (s_fam == FAM_BS);
    for (int i = 0; i < SYM_CACH; i++) {
        f.cach[i] = slice(ring_at(t0 - (uint32_t)((SYM_CACH - i) * SPS)));
    }
    for (int j = 0; j < SYM_BURST; j++) {
        f.burst[j] = slice(ring_at(t0 + (uint32_t)(j * SPS)));
    }
    f.t_ms = tsync / (FS / 1000);
    return dmr_proto_frame(&f);
}

/* Called for every new matched-filter sample (index s_n - 1). */
static void framer_step(void)
{
    uint32_t t = s_n - 1;

    if (!s_locked) {
        /* acquisition: strict threshold, all patterns, assumed polarity only */
        /* The 24 samples, their mean and energy are shared by all 8 patterns:
         * compute them once (this loop runs at the full 48 kHz). */
        int best_p = 0;
        float best = 0.0f, x[24], m = 0.0f, s2 = 0.0f;
        for (int k = 0; k < 24; k++) {
            x[k] = ring_at(t - (uint32_t)(SPS * (23 - k)));
            m += x[k];
        }
        m *= (1.0f / 24.0f);
        for (int k = 0; k < 24; k++) {
            x[k] -= m;
            s2 += x[k] * x[k];
        }
        if (s2 > 1e-20f) {
            for (int p = 1; p < DMR_SYNC_COUNT; p++) {
                float cc = 0.0f, r;
                for (int k = 0; k < 24; k++) {
                    cc += x[k] * s_sync_sign[p][k];
                }
                r = s_pol * cc / sqrtf(s2 * s_sync_den[p]);
                if (r > best) {
                    best = r;
                    best_p = p;
                }
            }
        }
        if (best >= RHO_ACQUIRE && (!s_cand || best > s_cand_rho)) {
            s_cand = true;
            s_cand_rho = best;
            s_cand_t = t;
            s_cand_sync = best_p;
            s_cand_hold = PEAK_HOLD;
        } else if (s_cand && --s_cand_hold <= 0) {
            /* peak confirmed: take levels from it and wait for the burst tail */
            float amp, dc;
            (void)sync_rho(s_cand_t, s_cand_sync, &amp, &dc);
            s_level = amp;
            s_dc = dc;
            s_fam = family_of(s_cand_sync);
            s_locked = true;
            s_first_frame = true;
            dmr_proto_new_lock();
            s_bad = 0;
            s_t_exp = s_cand_t;
            s_cand = false;
            s_syncs++;
        }
        if (!s_locked) {
            return;
        }
    }

    /* locked: wait until the whole burst (and the refine window) is in */
    if ((int32_t)(t
                  - (s_t_exp
                     + (uint32_t)((SYM_BURST - 1 - SYM_SYNC_LAST) * SPS
                                  + REFINE)))
        < 0) {
        return;
    }

    {
        uint32_t tsync = s_t_exp;
        dmr_sync_t sync = DMR_SYNC_NONE;
        float best = 0.0f, amp = 0.0f, dc = 0.0f;
        int grade;

        /* re-centre on this burst's sync field, if it carries one */
        for (int d = -REFINE; d <= REFINE; d++) {
            for (int p = 1; p < DMR_SYNC_COUNT; p++) {
                float a, o, r;
                if (family_of(p) != s_fam) {
                    continue;
                }
                r = s_pol * sync_rho(s_t_exp + (uint32_t)d, p, &a, &o);
                if (r > best) {
                    best = r;
                    tsync = s_t_exp + (uint32_t)d;
                    sync = (dmr_sync_t)p;
                    amp = a;
                    dc = o;
                }
            }
        }
        if (best >= RHO_TRACK) {
            s_level = 0.5f * s_level + 0.5f * amp;
            s_dc = 0.5f * s_dc + 0.5f * dc;
            s_syncs++;
        } else {
            sync = DMR_SYNC_NONE;
            tsync = s_t_exp;
        }

        grade = emit_frame(tsync, sync);
        if (grade == DMR_FRAME_STRONG) {
            s_bad = 0;
            s_pol_bad = 0;
        } else if (sync != DMR_SYNC_NONE && ++s_pol_bad >= POL_FLIP_NO_OK) {
            /* Syncs keep matching but nothing validates with a margin. With
             * inverted polarity an idle repeater's data bursts all read as
             * voice syncs (NEUTRAL), which a correct stream never produces
             * back to back - so NEUTRAL counts here too; and the loose FEC
             * checks (OK) pass a quarter to a third of random words, so
             * they do not count as proof either (in simplex they kept the
             * wrong polarity forever). Flip and re-acquire. */
            s_pol = -s_pol;
            s_pol_bad = 0;
            s_locked = false;
            s_cand = false;
            dmr_proto_unprove();
        } else if (grade == DMR_FRAME_OK) {
            s_bad = 0; /* Still a stream: keep the lock */
        } else if (grade == DMR_FRAME_BAD) {
            if (s_first_frame
                || ++s_bad >= (s_fam == FAM_MS ? BAD_MAX_MS : BAD_MAX_BS)) {
                /* a first burst that doesn't decode was a false sync; otherwise
                 * the stream has gone - back to acquisition either way */
                s_locked = false;
                s_cand = false;
            }
        }
        s_first_frame = false;
        s_t_exp = tsync
                + (uint32_t)(s_fam == FAM_MS ? PERIOD_60MS : PERIOD_30MS);
    }
}

/* ------------------------------------------------------------------------- */
/* Sample input                                                               */
/* ------------------------------------------------------------------------- */

static void push_disc(float x)
{
    const float *w;
    float acc = 0.0f;

    s_mf_hist[s_mf_idx] = x;
    s_mf_hist[s_mf_idx + RRC_TAPS] = x;
    s_mf_idx = (s_mf_idx + 1) % RRC_TAPS;
    w = &s_mf_hist[s_mf_idx];        /* oldest first */
    for (int k = 0; k < RRC_TAPS; k++) {
        acc += s_rrc_taps[k] * w[k]; /* symmetric taps: order irrelevant */
    }
    s_ring[s_n & RING_MASK] = acc;
    s_n++;
    framer_step();
}

void dmr_demod_feed_disc(const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        push_disc(x[i]);
    }
    publish();
}

static volatile bool s_fs4_rotation = true;

void dmr_demod_set_fs4_rotation(bool on)
{
    s_fs4_rotation = on;
}

void dmr_demod_feed_iq(const float *in_i, const float *in_q, int n)
{
    const bool rotate = s_fs4_rotation;

    const float k_hz = (float)FS / (2.0f * 3.14159265358979f);

    for (int s = 0; s < n; s++) {
        float i = in_i[s], q = in_q[s], ri, rq, fi = 0.0f, fq = 0.0f;
        const float *wi, *wq;

        /* multiply by j^n: the same fs/4 rotation sdr.c applies before its
         * SSB/AM demodulators, which brings the tuned channel to 0 Hz. Not
         * needed when the receiver already demodulates at the centre. */
        switch (rotate ? (s_rot_phase & 3u) : 0u) {
            case 0:
                ri = i;
                rq = q;
                break;
            case 1:
                ri = -q;
                rq = i;
                break;
            case 2:
                ri = -i;
                rq = -q;
                break;
            default:
                ri = q;
                rq = -i;
                break;
        }
        s_rot_phase++;

        s_ch_i[s_ch_idx] = ri;
        s_ch_i[s_ch_idx + CH_TAPS] = ri;
        s_ch_q[s_ch_idx] = rq;
        s_ch_q[s_ch_idx + CH_TAPS] = rq;
        s_ch_idx = (s_ch_idx + 1) % CH_TAPS;
        wi = &s_ch_i[s_ch_idx];
        wq = &s_ch_q[s_ch_idx];
        for (int k = 0; k < CH_TAPS; k++) {
            fi += s_ch_taps[k] * wi[k];
            fq += s_ch_taps[k] * wq[k];
        }

        /* FM discriminator: angle of z[n] * conj(z[n-1]), in Hz */
        {
            float re = fi * s_prev_i + fq * s_prev_q;
            float im = fq * s_prev_i - fi * s_prev_q;
            s_prev_i = fi;
            s_prev_q = fq;
            push_disc(atan2f(im, re) * k_hz);
        }
    }
    publish();
}

void dmr_demod_get_status(dmr_demod_status_t *out)
{
    DMR_LOCK(s_lock);
    *out = s_pub;
    DMR_UNLOCK(s_lock);
}

void dmr_demod_note_overrun(void)
{
    s_overruns++;
}
