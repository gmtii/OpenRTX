/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_DATA_H
#define DMR_DATA_H

#include <stdbool.h>
#include <stdint.h>

#include "dmr_lrrp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR packet data (ETSI TS 102 361-1 clause 9.2 / Annex B, TS 102 361-3):
 * data header, rate 1/2, 3/4 and 1 data blocks, unconfirmed and confirmed
 * delivery, CRC-9 per confirmed block and CRC-32 per packet, and the IPv4 /
 * UDP (full or ETSI compressed) layer above. UDP port 4001 is handed to the
 * LRRP parser (dmr_lrrp.h).
 *
 * Called from dmr_proto.c on the DMR task only; one assembler per timeslot.
 *
 * Confirmed data: blocks are placed by their serial number (DBSN), so a
 * selective retry (header with F = 0, same source / destination / N(S))
 * fills the gaps of the previous attempt instead of starting over.
 */

/* Slot type data types of the blocks (same values as ETSI data type) */
#define DMR_DATA_DT_RATE_12 7
#define DMR_DATA_DT_RATE_34 8
#define DMR_DATA_DT_RATE_1 10

/* Longest packet assembled, in blocks (LRRP needs 3..6; longer packets are
 * counted and skipped). 32 blocks x 24 bytes x 2 slots = 1.5 kB of RAM. */
#define DMR_DATA_MAX_BLOCKS 32

/* Data packet formats (DPF) */
enum {
    DMR_DPF_UDT = 0,
    DMR_DPF_RESPONSE = 1,
    DMR_DPF_UNCONFIRMED = 2,
    DMR_DPF_CONFIRMED = 3,
    DMR_DPF_SHORT_DEFINED = 13,
    DMR_DPF_SHORT_RAW = 14,
    DMR_DPF_PROPRIETARY = 15,
};

/* Service access points (SAP) */
enum {
    DMR_SAP_UDT = 0,
    DMR_SAP_TCPIP_COMP = 2,
    DMR_SAP_UDPIP_COMP = 3,
    DMR_SAP_IP = 4,
    DMR_SAP_ARP = 5,
    DMR_SAP_PROPRIETARY = 9,
    DMR_SAP_SHORT_DATA = 10,
};

typedef struct {
    uint8_t dpf, sap;
    bool group;        /* destination is a talkgroup */
    bool resp_req;     /* A: response requested */
    uint32_t dst, src; /* LLIDs (radio / talkgroup IDs) */
    int btf;           /* blocks to follow (appended blocks for short data) */
    int poc;           /* pad octet count */
    bool full;         /* F: first try / complete packet (confirmed data) */
    int ns;            /* N(S) send sequence number (confirmed data) */
    int fsn;           /* fragment sequence number */
} dmr_data_header_t;

typedef struct {
    dmr_data_header_t hdr;
    int len;                     /* user data octets (pad and CRC-32 removed) */
    uint32_t crc32;
    bool udp;                    /* an IPv4 / UDP datagram was found */
    bool compressed;             /* ETSI UDP/IPv4 compressed header (SAP 3) */
    uint32_t ip_src, ip_dst;     /* full header only, else 0 */
    uint16_t src_port, dst_port; /* 0 when only a port ID was sent */
    uint8_t src_port_id, dst_port_id; /* compressed header port IDs */
    int udp_len;                      /* UDP payload octets */
    bool lrrp_valid;
    dmr_lrrp_t lrrp;
} dmr_packet_t;

typedef struct {
    uint32_t headers, headers_bad; /* data headers (CRC-CCITT good / bad) */
    uint32_t blocks, crc9_bad;
    uint32_t packets, crc32_bad, dup, too_long;
    uint32_t lrrp;
} dmr_data_stats_t;

/* Results of dmr_data_block() */
#define DMR_DATA_PENDING 0    /* block stored, packet not complete yet */
#define DMR_DATA_PACKET 1     /* *out holds a complete packet (CRC-32 good) */
#define DMR_DATA_DUP 2        /* complete, but a repeat of the last packet */
#define DMR_DATA_CRC_BAD (-1)
#define DMR_DATA_IGNORED (-2) /* no packet being assembled on this slot */

void dmr_data_reset(void);

/* data: the 96 bits of a data header burst after BPTC decoding. Returns true
 * (and fills *out) when the CRC-CCITT checks; a packet header with blocks to
 * follow then starts (or resumes) assembly on that slot. */
bool dmr_data_header(int slot, const uint8_t data[96], uint32_t t_ms,
                     dmr_data_header_t *out);

/* dt: DMR_DATA_DT_*; info: the burst's 196 info bits (bits 0..97, 166..263). */
int dmr_data_block(int slot, int dt, const uint8_t info[196], uint32_t t_ms,
                   dmr_packet_t *out);

/* Any other burst type on the slot: the block run is over. */
void dmr_data_interrupt(int slot);

void dmr_data_get_stats(dmr_data_stats_t *out);

/* CRCs, exposed for the host tests */
uint16_t dmr_crc9(const uint8_t *bytes, int nbytes, int dbsn, uint16_t mask);
uint32_t dmr_crc32(const uint8_t *bytes, int nbytes);

const char *dmr_sap_name(uint8_t sap);

#ifdef __cplusplus
}
#endif

#endif /* DMR_DATA_H */
