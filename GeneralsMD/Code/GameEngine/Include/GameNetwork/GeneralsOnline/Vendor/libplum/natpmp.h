/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_NATPMP_H
#define PLUM_NATPMP_H

#include "pcp.h"

// RFC 6686 specifies version 0
#define NATPMP_VERSION 0

typedef struct {
	socket_t sock;
	socket_t mcast_sock;
	uint32_t prev_server_time;
	bool has_prev_server_time;
	bool interrupted;
} natpmp_impl_t;

int natpmp_impl_probe(pcp_impl_t *impl, addr_record_t *found_gateway, timestamp_t end_timestamp);
int natpmp_impl_map(pcp_impl_t *impl, const client_mapping_t *mapping,
                    protocol_map_output_t *output, uint32_t lifetime, const addr_record_t *gateway,
                    timestamp_t end_timestamp);
int natpmp_impl_process_mcast_response(pcp_impl_t *impl, const char *buffer, int len);
int natpmp_impl_check_epoch_time(pcp_impl_t *impl, uint32_t new_epoch_time);

#pragma pack(push, 1)

/* NAT-PMP response header (4 bytes)
 * See https://tools.ietf.org/html/rfc6886
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Vers = 0      | OP = 128 + 0  | Result Code (net byte order)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct natpmp_header {
	uint8_t version;
	uint8_t opcode;
	uint16_t result;
};

typedef enum natpmp_opcode {
	NATPMP_OPCODE_ANNOUNCE = 0,
	NATPMP_OPCODE_MAP_UDP = 1,
	NATPMP_OPCODE_MAP_TCP = 2
} natpmp_opcode_t;

#define NATPMP_OPCODE_RESPONSE_BIT 0x80

/* RFC 6886 3.5. Result Codes */
typedef enum natpmp_result_code {
	NATPMP_RESULT_SUCCESS = 0,
	NATPMP_RESULT_UNSUPP_VERSION = 1,
	NATPMP_RESULT_NOT_AUTHORIZED = 2,
	NATPMP_RESULT_NETWORK_FAILURE = 3,
	NATPMP_RESULT_NO_RESOURCES = 4,
	NATPMP_RESULT_UNSUPP_OPCODE = 5
} natpmp_result_code_t;

/* RFC 6886 3.2. Determining the External Address:
 *
 *  0                   1
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
 *	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *	| Vers = 0      | OP = 0        |
 *	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct natpmp_announce_request {
	uint8_t version;
	uint8_t opcode;
};

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Vers = 0      | OP = 128 + 0  | Result Code (net byte order)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Seconds Since Start of Epoch (in network byte order)          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | External IPv4 Address (a.b.c.d)                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct natpmp_announce_response {
	uint8_t version;
	uint8_t opcode;
	uint16_t result;
	uint32_t epoch_time;
	char external_addr[4];
};

/* RFC 6886 3.3. Requesting a Mapping
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Vers = 0      | OP = x        | Reserved                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Internal Port                 | Suggested External Port       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Requested Port Mapping Lifetime in Seconds                    |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct natpmp_map_request {
	uint8_t version;
	uint8_t opcode;
	uint16_t reserved;
	uint16_t internal_port;
	uint16_t suggested_external_port;
	uint32_t lifetime;
};

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Vers = 0      | OP = 128 + x  | Result Code                   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Seconds Since Start of Epoch                                  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Internal Port                 | Mapped External Port          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Port Mapping Lifetime in Seconds                              |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct natpmp_map_response {
	uint8_t version;
	uint8_t opcode;
	uint16_t result;
	uint32_t epoch_time;
	uint16_t internal_port;
	uint16_t external_port;
	uint32_t lifetime;
};

#pragma pack(pop)

#endif
