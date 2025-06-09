/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_PCP_H
#define PLUM_PCP_H

#include "addr.h"
#include "client.h"
#include "protocol.h"
#include "socket.h"
#include "thread.h"
#include "timestamp.h"

// RFC 6687 specifies version 2
#define PCP_VERSION 2
#define PCP_CLIENT_PORT 5350
#define PCP_SERVER_PORT 5351

// RFC 6887: All PCP messages are sent over UDP, with a maximum UDP payload length of 1100 octets.
#define PCP_MAX_PAYLOAD_LENGTH 1100

#define PCP_MAX_ATTEMPTS 4 // RFC 6886 recommends 9

typedef enum { PCP_INTERRUPT_NONE, PCP_INTERRUPT_SOFT, PCP_INTERRUPT_HARD } pcp_interrupt_t;

typedef struct {
	socket_t sock;
	socket_t mcast_sock;
	addr_record_t external_addr;
	uint32_t prev_server_time;
	uint32_t prev_client_time;
	bool has_prev_server_time;
	bool use_natpmp;
	atomic(pcp_interrupt_t) interrupt;
} pcp_impl_t;

int pcp_init(protocol_state_t *state);
int pcp_cleanup(protocol_state_t *state);
int pcp_discover(protocol_state_t *state, timediff_t duration);
int pcp_map(protocol_state_t *state, const client_mapping_t *mapping, protocol_map_output_t *output,
            timediff_t duration);
int pcp_unmap(protocol_state_t *state, const client_mapping_t *mapping, timediff_t duration);
int pcp_idle(protocol_state_t *state, timediff_t duration);
int pcp_interrupt(protocol_state_t *state, bool hard);

int pcp_impl_probe(pcp_impl_t *impl, addr_record_t *found_gateway, timestamp_t end_timestamp);
int pcp_impl_map(pcp_impl_t *impl, const client_mapping_t *mapping, protocol_map_output_t *output,
                 uint32_t lifetime, const addr_record_t *gateway, timestamp_t end_timestamp);
int pcp_impl_process_mcast_response(pcp_impl_t *impl, const char *buffer, int len);
int pcp_impl_check_epoch_time(pcp_impl_t *impl, uint32_t new_epoch_time);

int pcp_natpmp_impl_wait_response(pcp_impl_t *impl, char *buffer, addr_record_t *src,
                                  timestamp_t end_timestamp, bool interruptible);

#pragma pack(push, 1)

/* PCP/NAT-PMP common response header (4 bytes)
 * See https://tools.ietf.org/html/rfc6886
 * See https://tools.ietf.org/html/rfc6887
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |    Version    |R|   Opcode    |   Reserved    |  Result Code  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
struct pcp_common_header {
	uint8_t version;
	uint8_t opcode;
	uint8_t reserved;
	uint8_t result;
};

typedef enum pcp_opcode {
	PCP_OPCODE_ANNOUNCE = 0,
	PCP_OPCODE_MAP = 1,
	PCP_OPCODE_PEER = 2
} pcp_opcode_t;

#define PCP_OPCODE_RESPONSE_BIT 0x80

/* RFC 6887 7.1. Request Header
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Version = 2  |R|   Opcode    |         Reserved              |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 Requested Lifetime (32 bits)                  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * |            PCP Client's IP Address (128 bits)                 |
 * |                                                               |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * :                                                               :
 * :             (optional) Opcode-specific information            :
 * :                                                               :
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * :                                                               :
 * :             (optional) PCP Options                            :
 * :                                                               :
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct pcp_request_header {
	uint8_t version;
	uint8_t opcode;
	uint16_t reserved;
	uint32_t lifetime;
	unsigned char client_address[16];
};

/* RFC 6887 7.2. Response Header
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Version = 2  |R|   Opcode    |   Reserved    |  Result Code  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                      Lifetime (32 bits)                       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     Epoch Time (32 bits)                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * |                      Reserved (96 bits)                       |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * :                                                               :
 * :             (optional) Opcode-specific response data          :
 * :                                                               :
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * :             (optional) Options                                :
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct pcp_response_header {
	uint8_t version;
	uint8_t opcode;
	uint8_t reserved;
	uint8_t result;
	uint32_t lifetime;
	uint32_t epoch_time;
	unsigned char reserved96[12];
};

#define PCP_RESPONSE_HEADER_LENGTH 24

/* RFC 6887 7.3. Options
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Option Code  |  Reserved     |       Option Length           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * :                       (optional) Data                         :
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct pcp_option {
	uint8_t code;
	uint8_t reserved;
	uint16_t length;
	unsigned char data[0];
};

/* RFC 6887 7.4. Result Codes */
typedef enum pcp_result_code {
	PCP_RESULT_SUCCESS = 0,
	PCP_RESULT_UNSUPP_VERSION = 1,
	PCP_RESULT_NOT_AUTHORIZED = 2,
	PCP_RESULT_MALFORMED_REQUEST = 3,
	PCP_RESULT_UNSUPP_OPCODE = 4,
	PCP_RESULT_UNSUPP_OPTION = 5,
	PCP_RESULT_MALFORMED_OPTION = 6,
	PCP_RESULT_NETWORK_FAILURE = 7,
	PCP_RESULT_NO_RESOURCES = 8,
	PCP_RESULT_UNSUPP_PROTOCOL = 9,
	PCP_RESULT_USER_EX_QUOTA = 10,
	PCP_RESULT_CANNOT_PROVIDE_EXTERNAL = 11,
	PCP_RESULT_ADDRESS_MISMATCH = 12,
	PCP_RESULT_EXCESSIVE_REMOTE_PEERS = 13
} pcp_result_code_t;

/* RFC 6887 11.1. MAP Operation Packet Formats
 *
 * MAP Opcode Request
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * |                 Mapping Nonce (96 bits)                       |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Protocol    |          Reserved (24 bits)                   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |        Internal Port          |    Suggested External Port    |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * |           Suggested External IP Address (128 bits)            |
 * |                                                               |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
#define PCP_MAP_NONCE_SIZE 12
struct pcp_map_request {
	unsigned char nonce[PCP_MAP_NONCE_SIZE];
	uint8_t protocol;
	unsigned char reserved[3];
	uint16_t internal_port;
	uint16_t suggested_external_port;
	unsigned char suggested_external_addr[16];
};

/* MAP Opcode Response
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * |                 Mapping Nonce (96 bits)                       |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Protocol    |          Reserved (24 bits)                   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |        Internal Port          |    Assigned External Port     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * |            Assigned External IP Address (128 bits)            |
 * |                                                               |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct pcp_map_response {
	unsigned char nonce[PCP_MAP_NONCE_SIZE];
	uint8_t protocol;
	unsigned char reserved[3];
	uint16_t internal_port;
	uint16_t external_port;
	unsigned char external_addr[16];
};

// See the IANA protocol registry
// https://www.iana.org/assignments/protocol-numbers/protocol-numbers.xhtml
typedef enum pcp_protocol {
	PCP_PROTOCOL_ALL = 0,
	PCP_PROTOCOL_TCP = 6,
	PCP_PROTOCOL_UDP = 17
} pcp_protocol_t;

#pragma pack(pop)

#endif
