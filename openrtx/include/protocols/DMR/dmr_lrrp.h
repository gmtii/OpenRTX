/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_LRRP_H
#define DMR_LRRP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motorola LRRP (Location Request/Response Protocol), binary MBXML form, as
 * carried in UDP port 4001 over DMR packet data.
 *
 * Only what a listener needs is extracted: the document type and, for
 * reports, position, accuracy, altitude, speed, heading and fix time.
 * Token encodings follow ok-dmrlib (okdmr/dmrlib/motorola/lrrp.py, mbxml.py)
 * and are checked against its test vectors in test/host_dmr_lrrp.
 */

typedef enum {
    DMR_LRRP_NONE = 0,
    DMR_LRRP_REQUEST, /* immediate / triggered / stop / protocol request */
    DMR_LRRP_REPORT,  /* immediate / triggered / unsolicited report */
    DMR_LRRP_ANSWER,  /* triggered start / stop answer, protocol report */
} dmr_lrrp_kind_t;

typedef struct {
    dmr_lrrp_kind_t kind;
    uint8_t doc_id;      /* MBXML document id (0x04..0x15) */
    bool has_request_id;
    uint32_t request_id; /* up to 4 bytes of the opaque request-id */
    int result; /* result-code of an answer / operation-error, -1 none */

    bool has_pos;
    double lat, lon; /* degrees, + = N / E */
    bool has_radius;
    float radius_m;  /* horizontal accuracy (circle shapes) */
    bool has_alt;
    float alt_m;
    bool has_speed;
    float speed_mps; /* horizontal speed */
    bool has_vspeed;
    float vspeed_mps;
    bool has_dir;
    int dir_deg;    /* heading, 0..358 in 2 degree steps */
    bool has_time;
    uint16_t year;  /* fix time, UTC */
    uint8_t month, day, hour, min, sec;
    int interval_s; /* periodic-trigger interval of a request, -1 none */
} dmr_lrrp_t;

/* True when p[0..len) starts with a well-formed LRRP document (id in the LRRP
 * range and a document length that fits len). Fills out as far as the known
 * tokens go: an unknown token stops parsing but keeps what was read. */
bool dmr_lrrp_parse(const uint8_t *p, int len, dmr_lrrp_t *out);

/* Name of the document type ("Triggered-Location-Report"...) */
const char *dmr_lrrp_doc_name(uint8_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DMR_LRRP_H */
