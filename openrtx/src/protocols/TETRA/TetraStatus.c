/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <tetra/tetra.h>
#include "interfaces/delays.h"
#include "protocols/TETRA/TetraHeard.h"
#include "protocols/TETRA/TetraSds.h"
#include "protocols/TETRA/TetraSpeech.h"
#include "protocols/TETRA/TetraStatus.h"
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define CALL_PRINT printk
#else
#define CALL_PRINT printf
#endif

/* A call is shown this long after its last activity, if no release is seen */
#define CALL_TIMEOUT_MS 10000
#define VOICE_TIMEOUT_MS 1000

static pthread_mutex_t statusMutex = PTHREAD_MUTEX_INITIALIZER;
static struct tetraStatus status; /* Under statusMutex */
static long long lastCall;        /* Tick of the last call activity */
static long long lastVoice;       /* Tick of the last traffic slot */

/*
 * Calls whose signalling was received, to tell which call a traffic slot
 * belongs to. libtetra matches traffic to its calls by usage marker only,
 * and in promiscuous mode passes on unmatched traffic flagged clear: it
 * would play air-encrypted calls, whose signalling it cannot read, as
 * noise. As on the ESP32-P4 SDR, a slot belongs to a call by its usage
 * marker or by the timeslots of the call's channel allocation.
 */
#define MAX_CALLS 4
/* A call is forgotten this long after its last signalling, if no release */
#define KNOWN_CALL_TIMEOUT_MS 60000

struct knownCall {
    bool used;
    uint16_t id;
    uint8_t usageMarker; /* 0 if unknown */
    uint8_t timeslots;   /* Bit 0 = TN1, 0 if unknown */
    bool encrypted;
    bool data;
    long long last; /* Tick of the last signalling */
};

static struct knownCall calls[MAX_CALLS]; /* Under statusMutex */

static void countCalls(void)
{
    status.knownCalls = 0;
    for (int i = 0; i < MAX_CALLS; i++) {
        if (calls[i].used)
            status.knownCalls++;
    }
}

/**
 * Update the known calls with call signalling.
 */
static void trackCall(const struct tetra_call *c)
{
    struct knownCall *k = NULL;
    for (int i = 0; i < MAX_CALLS; i++) {
        if (calls[i].used && calls[i].id == c->call_id)
            k = &calls[i];
    }

    if (c->what == TETRA_CALL_RELEASE) {
        if (k != NULL) {
            CALL_PRINT("TETRA call %u released\n", c->call_id);
            k->used = false;
        }
        countCalls();
        return;
    }

    bool isNew = (k == NULL);
    if (isNew) {
        /* A free entry, else the call with the oldest signalling */
        k = &calls[0];
        for (int i = 0; i < MAX_CALLS; i++) {
            if (!calls[i].used) {
                k = &calls[i];
                break;
            }
            if (calls[i].last < k->last)
                k = &calls[i];
        }
        memset(k, 0, sizeof(*k));
        k->used = true;
        k->id = c->call_id;
    }

    uint8_t usageMarker = k->usageMarker;
    uint8_t timeslots = k->timeslots;
    if (c->usage_marker != 0)
        k->usageMarker = c->usage_marker;
    if (c->timeslots != 0)
        k->timeslots = c->timeslots;
    k->encrypted |= c->encrypted;
    k->data = (c->circuit_type != 0);
    k->last = getTick();

    status.lastCallId = k->id;
    status.lastCallMarker = k->usageMarker;
    status.lastCallSlots = k->timeslots;

    countCalls();
    if (isNew || usageMarker != k->usageMarker || timeslots != k->timeslots)
        CALL_PRINT("TETRA call %u to %lu: usage marker %u, timeslots 0x%x%s\n",
                   k->id, (unsigned long)c->dst_ssi, k->usageMarker,
                   k->timeslots, k->encrypted ? ", encrypted" : "");
}

/**
 * Tell which known call a traffic slot belongs to.
 */
static enum tetraTrafficCall matchTraffic(const struct tetra_traffic *t)
{
    if (t->encrypted)
        return TETRA_TRAFFIC_ENCRYPTED;

    long long now = getTick();
    uint8_t slot = (t->time.tn >= 1) ? (uint8_t)(1u << (t->time.tn - 1)) : 0;
    for (int i = 0; i < MAX_CALLS; i++) {
        struct knownCall *k = &calls[i];
        if (!k->used)
            continue;
        if (now - k->last > KNOWN_CALL_TIMEOUT_MS) {
            k->used = false;
            continue;
        }
        bool marker = (k->usageMarker != 0)
                   && (k->usageMarker == t->usage_marker);
        if (marker || (k->timeslots & slot)) {
            if (k->encrypted)
                return TETRA_TRAFFIC_ENCRYPTED;
            return k->data ? TETRA_TRAFFIC_DATA : TETRA_TRAFFIC_CLEAR;
        }
    }

    /* libtetra matched it to a call of its own */
    if (t->call_id != 0 || t->talker_ssi != 0)
        return (t->circuit_type != 0) ? TETRA_TRAFFIC_DATA :
                                        TETRA_TRAFFIC_CLEAR;

    return TETRA_TRAFFIC_UNKNOWN;
}

void tetra_getStatus(struct tetraStatus *out)
{
    pthread_mutex_lock(&statusMutex);
    long long now = getTick();
    status.call = status.call && (now - lastCall < CALL_TIMEOUT_MS);
    status.voice = status.call && (now - lastVoice < VOICE_TIMEOUT_MS);
    *out = status;
    pthread_mutex_unlock(&statusMutex);
}

static void onSds(const struct tetra_sds *s)
{
    struct tetraSds m;
    if (!tetraSds_receive(s, &m))
        return;
    tetraHeard_sds(&m);

    /* Line 1 shows text and status; locations and data are listed only */
    if (m.kind != TETRA_SDS_KIND_TEXT && m.kind != TETRA_SDS_KIND_STATUS)
        return;

    strncpy(status.sds, m.text, sizeof(status.sds) - 1);
    status.sds[sizeof(status.sds) - 1] = '\0';
    status.sdsSrc = m.from;
    status.sdsTime = getTick();
}

static void onCall(const struct tetra_call *c)
{
    trackCall(c);
    tetraHeard_call(c);

    switch (c->what) {
        case TETRA_CALL_RELEASE:
            status.call = false;
            status.talker = 0;
            return;
        case TETRA_CALL_TX_CEASED:
            status.talker = 0;
            break;
        case TETRA_CALL_ALERT:
        case TETRA_CALL_PROCEEDING:
            return;
        default:
            if (c->src_ssi != 0)
                status.talker = c->src_ssi;
            break;
    }
    /* A new call identifier replaces the shown call */
    if (!status.call || c->call_id != status.callId) {
        status.callDst = 0;
        status.callEncrypted = false;
        status.callUnknown = false;
    }
    status.call = true;
    status.callId = c->call_id;
    if (c->dst_ssi != 0) {
        status.callDst = c->dst_ssi;
        status.callGroup = c->group;
    }
    status.callEncrypted |= c->encrypted;
    lastCall = getTick();
}

void tetraStatus_onEvent(void *user, const struct tetra_event *ev)
{
    (void)user;

    /* Speech first, outside the status lock */
    enum tetraTrafficCall call = TETRA_TRAFFIC_UNKNOWN;
    if (ev->type == TETRA_EV_TRAFFIC) {
        pthread_mutex_lock(&statusMutex);
        call = matchTraffic(&ev->u.traffic);
        pthread_mutex_unlock(&statusMutex);
        tetraSpeech_traffic(&ev->u.traffic, call);
    }

    pthread_mutex_lock(&statusMutex);
    switch (ev->type) {
        case TETRA_EV_SYNC:
            status.sync = ev->u.sync.locked;
            if (!status.sync)
                status.call = false;
            break;

        case TETRA_EV_CELL: {
            const struct tetra_cell *c = ev->u.cell;
            status.cell = true;
            status.mcc = c->mcc;
            status.mnc = c->mnc;
            status.colourCode = c->colour_code;
            if (c->have_sysinfo) {
                status.sysinfo = true;
                status.la = c->location_area;
                status.dlFreq = c->dl_freq_hz;
                status.airEncryption = c->air_encryption;
            }
            break;
        }

        case TETRA_EV_CALL:
            onCall(&ev->u.call);
            break;

        case TETRA_EV_TRAFFIC: {
            const struct tetra_traffic *t = &ev->u.traffic;
            if (t->talker_ssi != 0)
                status.talker = t->talker_ssi;
            status.callEncrypted |= (call == TETRA_TRAFFIC_ENCRYPTED);
            status.callUnknown = (call == TETRA_TRAFFIC_UNKNOWN);
            status.lastTrafficMarker = t->usage_marker;
            status.lastTrafficTn = t->time.tn;
            if (!status.call)
                status.callId = t->call_id;
            status.call = true;
            lastCall = lastVoice = getTick();
            break;
        }

        case TETRA_EV_SDS:
            onSds(&ev->u.sds);
            break;

        case TETRA_EV_MM:
            tetraHeard_mm(ev->u.mm);
            break;

        default:
            break;
    }
    pthread_mutex_unlock(&statusMutex);
}

void tetraStatus_reset(void)
{
    pthread_mutex_lock(&statusMutex);
    memset(&status, 0, sizeof(status));
    memset(calls, 0, sizeof(calls));
    pthread_mutex_unlock(&statusMutex);
    tetraSds_reset();
    tetraHeard_reset();
}

void tetraStatus_lostSync(bool overrun)
{
    pthread_mutex_lock(&statusMutex);
    status.sync = false;
    status.resyncs++;
    if (overrun)
        status.overruns++;
    pthread_mutex_unlock(&statusMutex);
}

void tetraStatus_setDemod(unsigned phaseErr, int carrierHz)
{
    pthread_mutex_lock(&statusMutex);
    status.phaseErr = phaseErr;
    status.carrierHz = carrierHz;
    pthread_mutex_unlock(&statusMutex);
}

bool tetraStatus_sync(void)
{
    pthread_mutex_lock(&statusMutex);
    bool sync = status.sync;
    pthread_mutex_unlock(&statusMutex);
    return sync;
}
