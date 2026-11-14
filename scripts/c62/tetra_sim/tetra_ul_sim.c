/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host simulator of the C62 TETRA receive chain on a TMO uplink.
 *
 * Builds real TMO bursts with libtetra's TX blocks, modulates them
 * (pi/4-DQPSK, root raised cosine), adds noise, passes them through an IF
 * filter and an FM discriminator and decimates to the radio's 48 kHz int16
 * audio. That goes through TetraDemod and libtetra as on the radio.
 *
 * First a continuous downlink, so the demodulator converges as it does on
 * the cell, then the uplink: two radios, each with its own timing, carrier
 * offset and level, in bursts with silence between them.
 *
 *   tetra_ul_sim [snr_dB] [seed] [burst] [radios]
 *
 * snr_dB: signal to noise ratio of radio A in a 25 kHz channel (radio B is
 * 6 dB weaker); burst: 1 to put the demodulator in burst mode on the uplink;
 * radios: 1 radio A only, 2 radio B only, 3 both (default).
 */

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>
#include "protocols/TETRA/TetraDemod.h"

#define FS 144000.0 /* Simulation rate: 8 samples per symbol */
#define SPS 8
#define SLOT_SYM 255
#define DL_SECONDS 4
#define UL_FRAMES 700 /* About 40 s */
#define SPAN 8        /* Pulse span each side, symbols */
#define RADIO_OFFSET 300.0 /* The C62's own carrier error, Hz */
#define LSB_PER_HZ 1.0

static const uint16_t MCC = 901, MNC = 9999;
static const uint8_t CC = 5;

/* ---- Random numbers, deterministic ---- */

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static double urand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (rng >> 11) * (1.0 / 9007199254740992.0);
}

static double grand(void)
{
    double u1 = urand(), u2 = urand();
    if (u1 < 1e-300)
        u1 = 1e-300;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static void randomBits(uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        b[i] = urand() < 0.5;
}

/* ---- PDUs, as in libtetra's uplink tests ---- */

struct pdu {
    uint8_t b[600];
    unsigned n;
};

static void put(struct pdu *p, uint32_t v, unsigned k)
{
    for (int i = (int)k - 1; i >= 0; i--)
        p->b[p->n++] = (v >> i) & 1;
}

static void fillTo(struct pdu *p, unsigned end)
{
    if (p->n >= end)
        return;
    put(p, 1, 1);
    while (p->n < end)
        put(p, 0, 1);
}

static void append(struct pdu *p, const struct pdu *s)
{
    memcpy(p->b + p->n, s->b, s->n);
    p->n += s->n;
}

/* BL-UDATA, MLE: CMCE, U-TX DEMAND */
static struct pdu uTxDemand(uint16_t callId)
{
    struct pdu p = { 0 };
    put(&p, 2, 4);
    put(&p, 2, 3);
    put(&p, 10, 5);
    put(&p, callId, 14);
    put(&p, 1, 2);
    put(&p, 0, 1);
    put(&p, 0, 1);
    put(&p, 0, 1);
    return p;
}

/* BL-UDATA, MLE: CMCE, U-SDS DATA with an SDS-TL text (GSM 7-bit) */
static struct pdu uSdsText(uint32_t dst, const char *text)
{
    struct pdu p = { 0 };
    unsigned len = (unsigned)strlen(text);
    unsigned octets = (7 * len + 7) / 8;
    uint8_t packed[200] = { 0 };

    for (unsigned i = 0; i < len; i++) {
        unsigned v = (unsigned)text[i] & 0x7f, bit = 7 * i;
        packed[bit / 8] |= (uint8_t)(v << (bit % 8));
        if (bit % 8 > 1)
            packed[bit / 8 + 1] |= (uint8_t)(v >> (8 - bit % 8));
    }
    put(&p, 2, 4);
    put(&p, 2, 3);
    put(&p, 15, 5);
    put(&p, 0, 4);
    put(&p, 1, 2);
    put(&p, dst, 24);
    put(&p, 3, 2);
    put(&p, 8 + 4 + 2 + 1 + 1 + 8 + 1 + 7 + 8 * octets, 11);
    put(&p, 0x82, 8);
    put(&p, 0, 4 + 2 + 1 + 1);
    put(&p, 9, 8);
    put(&p, 0, 1);
    put(&p, 0, 7);
    for (unsigned i = 0; i < octets; i++)
        put(&p, packed[i], 8);
    put(&p, 0, 1);
    return p;
}

/* MAC-ACCESS with SSI, filled to 92 bits */
static struct pdu macAccess(uint32_t ssi, const struct pdu *sdu)
{
    struct pdu m = { 0 };
    put(&m, 0, 1);
    put(&m, 1, 1);
    put(&m, 0, 1);
    put(&m, 0, 2);
    put(&m, ssi, 24);
    put(&m, 0, 1);
    append(&m, sdu);
    fillTo(&m, 92);
    return m;
}

/* MAC-DATA with SSI and length indication, filled to nbits */
static struct pdu macData(uint32_t ssi, const struct pdu *sdu, unsigned nbits)
{
    struct pdu m = { 0 };
    unsigned len = 37 + sdu->n, li = (len + 7) / 8;
    put(&m, 0, 2);
    put(&m, li * 8 != len, 1);
    put(&m, 0, 1);
    put(&m, 0, 2);
    put(&m, ssi, 24);
    put(&m, 0, 1);
    put(&m, li, 6);
    append(&m, sdu);
    fillTo(&m, li * 8);
    fillTo(&m, nbits);
    return m;
}

/* ---- Modulation ---- */

static float complex *iq;
static size_t iqLen;
static double sigPow; /* Sum of the squared pulse: power of a carrier */

static double rrc(double t)
{
    const double a = 0.35;
    if (fabs(t) < 1e-9)
        return 1.0 - a + 4 * a / M_PI;
    if (fabs(fabs(4 * a * t) - 1.0) < 1e-9)
        return a / sqrt(2)
             * ((1 + 2 / M_PI) * sin(M_PI / (4 * a))
                + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
    return (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a)))
         / (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
}

static double dibitPhase(unsigned b1, unsigned b2)
{
    /* EN 300 392-2 table 5.1 */
    if (!b1)
        return b2 ? 3 * M_PI / 4 : M_PI / 4;
    return b2 ? -3 * M_PI / 4 : -M_PI / 4;
}

/**
 * Add symbols to the signal.
 *
 * @param bits: burst bits, two per symbol.
 * @param guard: random symbols ramped up before and down after (0: none).
 * @param start: sample of the first burst symbol (fractional).
 * @param theta: carrier phase, carried across calls for a continuous signal.
 */
static void render(const uint8_t *bits, unsigned nbits, unsigned guard,
                   double start, double amp, double freq, double *theta)
{
    int nsym = (int)nbits / 2;
    for (int k = -(int)guard; k < nsym + (int)guard; k++) {
        unsigned b1, b2;
        double w = amp;
        if (k < 0 || k >= nsym) {
            b1 = urand() < 0.5;
            b2 = urand() < 0.5;
            int d = (k < 0) ? -k : k - nsym + 1;
            w *= 0.5 + 0.5 * cos(M_PI * d / (guard + 1));
        } else {
            b1 = bits[2 * k];
            b2 = bits[2 * k + 1];
        }
        *theta += dibitPhase(b1, b2);

        double tk = start + k * SPS;
        long n0 = (long)ceil(tk - SPAN * SPS), n1 = (long)floor(tk + SPAN * SPS);
        for (long n = n0; n <= n1; n++) {
            if (n < 0 || (size_t)n >= iqLen)
                continue;
            double ph = *theta + 2 * M_PI * freq * n / FS;
            iq[n] += (float)(w * rrc((n - tk) / SPS)) * cexpf(I * (float)ph);
        }
    }
}

/* ---- Receiver front end ---- */

static void lowpass(float *h, int n, double fc)
{
    double total = 0;
    for (int i = 0; i < n; i++) {
        double t = i - (n - 1) / 2.0;
        double s = (t == 0) ? 2 * fc / FS : sin(2 * M_PI * fc / FS * t) / (M_PI * t);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1))
                 + 0.08 * cos(4 * M_PI * i / (n - 1));
        h[i] = (float)(s * w);
        total += s * w;
    }
    for (int i = 0; i < n; i++)
        h[i] /= (float)total;
}

#define IF_TAPS 65
#define AF_TAPS 63

/**
 * IF filter, FM discriminator, audio filter, decimation by 3 to int16.
 */
static size_t frontEnd(int16_t *out)
{
    float hif[IF_TAPS], haf[AF_TAPS];
    lowpass(hif, IF_TAPS, 13000);
    lowpass(haf, AF_TAPS, 20000);

    float complex prev = 1;
    float af[AF_TAPS] = { 0 };
    unsigned afPos = 0;
    size_t m = 0;
    for (size_t n = IF_TAPS; n < iqLen; n++) {
        float complex z = 0;
        for (int i = 0; i < IF_TAPS; i++)
            z += hif[i] * iq[n - i];
        float f = cargf(z * conjf(prev)) * (float)(FS / (2 * M_PI));
        prev = z;

        af[afPos] = f;
        afPos = (afPos + 1) % AF_TAPS;
        if (n % 3)
            continue;
        float acc = 0;
        unsigned j = afPos;
        for (int i = 0; i < AF_TAPS; i++) {
            acc += haf[i] * af[j];
            j = (j + 1) % AF_TAPS;
        }
        float v = acc * (float)LSB_PER_HZ;
        out[m++] = (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
    }
    return m;
}

/* ---- Decoded events ---- */

static unsigned evDemand, evSds, evTraffic, evUplink, evSync;

static void onEvent(void *user, const struct tetra_event *ev)
{
    (void)user;
    if (ev->type == TETRA_EV_SYNC && ev->u.sync.locked)
        evSync++;
    if (!ev->uplink)
        return;
    evUplink++;
    switch (ev->type) {
        case TETRA_EV_CALL:
            if (ev->u.call.what == TETRA_CALL_TX_DEMAND)
                evDemand++;
            break;
        case TETRA_EV_SDS:
            evSds++;
            break;
        case TETRA_EV_TRAFFIC:
            evTraffic++;
            break;
        default:
            break;
    }
}

int main(int argc, char **argv)
{
    double snr = (argc > 1) ? atof(argv[1]) : 20;
    unsigned seed = (argc > 2) ? (unsigned)atoi(argv[2]) : 1;
    bool burst = (argc > 3) && atoi(argv[3]) != 0;
    int radios = (argc > 4) ? atoi(argv[4]) : 3;
    for (unsigned i = 0; i < seed; i++)
        urand();

    const size_t dlSamples = (size_t)(DL_SECONDS * FS);
    const size_t ulStart = dlSamples + 1000;
    const size_t frameSamples = 4 * SLOT_SYM * SPS;
    iqLen = ulStart + (UL_FRAMES + 1) * frameSamples;
    iq = calloc(iqLen, sizeof(*iq));
    int16_t *audio = malloc(iqLen / 3 * sizeof(*audio));

    /* Downlink: continuous, a sync burst every 18 slots */
    double theta = 0;
    uint8_t slot[510], a[216], b[216], bb[30], sb[120];
    unsigned dlSlots = (unsigned)(dlSamples / (SLOT_SYM * SPS));
    for (unsigned s = 0; s < dlSlots; s++) {
        randomBits(a, 216);
        randomBits(b, 216);
        randomBits(bb, 30);
        randomBits(sb, 120);
        if (s % 18 == 0)
            tetra_tmo_build_sb(slot, sb, bb, b);
        else
            tetra_tmo_build_ndb(slot, a, bb, b, false);
        render(slot, 510, 0, 100.0 + s * SLOT_SYM * SPS, 1.0, RADIO_OFFSET,
               &theta);
    }
    double p = 0;
    for (size_t n = dlSamples / 4; n < dlSamples * 3 / 4; n++)
        p += crealf(iq[n] * conjf(iq[n]));
    sigPow = p / (dlSamples / 2);

    /* Uplink: radio A on timeslot 1, radio B on timeslot 3 */
    uint32_t scramb = tetra_scramb_init(tetra_tmo_colour_code(MCC, MNC, CC));
    double tA = urand() * SPS, tB = urand() * SPS * 40;
    double fA = RADIO_OFFSET + 200 * (urand() - 0.5);
    double fB = RADIO_OFFSET + 200 * (urand() - 0.5);
    double ampA = 1.0, ampB = pow(10, -6 / 20.0);
    unsigned sentDemand = 0, sentSds = 0, sentTraffic = 0;
    uint16_t callId = 100;
    for (unsigned f = 0; f < UL_FRAMES; f++) {
        double frame = ulStart + (double)f * frameSamples;
        if (f % 18 == 17)
            continue; /* Control frame */

        uint8_t t5[432], burstBits[TETRA_NUB_BITS];
        if (!(radios & 1)) {
            /* No radio A */
        } else if (f % 6 == 0) {
            struct pdu sdu = uSdsText(9001, "TETRA UPLINK SIM");
            struct pdu m = macData(1234567, &sdu, 268);
            tetra_encode_sch(TETRA_SCH_F, m.b, scramb, t5);
            tetra_tmo_build_nub(burstBits, t5, t5 + 216, false);
            sentSds++;
        } else {
            uint8_t fa[137], fb[137];
            randomBits(fa, 137);
            randomBits(fb, 137);
            tetra_speech_encode_slot(fa, fb, t5);
            tetra_scramble(t5, 432, scramb);
            tetra_tmo_build_nub(burstBits, t5, t5 + 216, false);
            sentTraffic++;
        }
        double ph = urand() * 2 * M_PI;
        if (radios & 1)
            render(burstBits, TETRA_NUB_BITS, 5,
                   frame + (1 * SLOT_SYM + 17) * SPS + tA, ampA, fA, &ph);

        if ((radios & 2) && f % 3 == 0) {
            struct pdu sdu = uTxDemand(callId++);
            struct pdu m = macAccess(7654321, &sdu);
            uint8_t cb[168], cbBits[TETRA_CB_BITS];
            tetra_encode_sch(TETRA_SCH_HU, m.b, scramb, cb);
            tetra_tmo_build_cb(cbBits, cb);
            ph = urand() * 2 * M_PI;
            render(cbBits, TETRA_CB_BITS, 5,
                   frame + (3 * SLOT_SYM + 17) * SPS + tB, ampB, fB, &ph);
            sentDemand++;
        }
    }

    /* Noise, in the 25 kHz channel at the wanted SNR of radio A */
    float hif[IF_TAPS];
    lowpass(hif, IF_TAPS, 13000);
    double gain = 0;
    for (int i = 0; i < IF_TAPS; i++)
        gain += hif[i] * hif[i];
    double sigma = sqrt(sigPow / pow(10, snr / 10) / gain / 2);
    for (size_t n = 0; n < iqLen; n++)
        iq[n] += (float)(sigma * grand()) + I * (float)(sigma * grand());

    size_t na = frontEnd(audio);
    size_t ulAudio = (ulStart - IF_TAPS) / 3;

    /* Receiver */
    static struct tetraDemod dm;
    static struct tetra_tmo_rx rx;
    tetraDemod_init(&dm);
    struct tetra_rx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.promiscuous = true;
    tetra_tmo_init(&rx, &cfg);

    static int8_t soft[960];
    const size_t chunk = 960;
    for (size_t n = 0; n < na; n += chunk) {
        size_t len = (na - n < chunk) ? na - n : chunk;
        if (n <= ulAudio && ulAudio < n + chunk) {
            printf("downlink end: phase error %u%%, offset %d Hz\n",
                   tetraDemod_phaseError(&dm),
                   tetraDemod_carrierOffset(&dm));
            /* Retuned to the uplink */
            tetraDemod_setHold(&dm, true);
            tetraDemod_setBurst(&dm, burst);
            tetra_tmo_init(&rx, &cfg);
            tetra_tmo_set_uplink_cell(&rx, MCC, MNC, CC);
        }
        size_t k = tetraDemod_process(&dm, audio + n, len, soft);
        if (n >= ulAudio)
            tetra_tmo_push_soft(&rx, soft, k);
    }

    printf("SNR %.1f dB seed %u burst %d: radio A %+.0f Hz, radio B %+.0f Hz\n",
           snr, seed, burst, fA - RADIO_OFFSET, fB - RADIO_OFFSET);
    printf("  SDS      %3u/%3u\n", evSds, sentSds);
    printf("  traffic  %3u/%3u\n", evTraffic, sentTraffic);
    printf("  TX dem.  %3u/%3u (radio B, -6 dB)\n", evDemand, sentDemand);
    printf("  uplink events %u, phase error %u%%\n", evUplink,
           tetraDemod_phaseError(&dm));
    free(iq);
    free(audio);
    return 0;
}
