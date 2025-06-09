/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_UPNP_H
#define PLUM_UPNP_H

#include "addr.h"
#include "client.h"
#include "protocol.h"
#include "socket.h"
#include "thread.h"
#include "timestamp.h"

#define UPNP_SSDP_ADDRESS "239.255.255.250"
#define UPNP_SSDP_PORT 1900

#define UPNP_SSDP_MAX_ATTEMPTS 4
#define UPNP_MAP_MAX_ATTEMPTS 10
#define UPNP_QUERY_TIMEOUT 5000 // msecs

#define UPNP_BUFFER_SIZE 2048

typedef enum { UPNP_INTERRUPT_NONE, UPNP_INTERRUPT_SOFT, UPNP_INTERRUPT_HARD } upnp_interrupt_t;

typedef struct {
	socket_t sock;
	char external_addr_str[ADDR_MAX_STRING_LEN];
	char *location_url;
	const char *service;
	int version;
	char *control_url;
	atomic(upnp_interrupt_t) interrupt;
} upnp_impl_t;

int upnp_init(protocol_state_t *state);
int upnp_cleanup(protocol_state_t *state);
int upnp_discover(protocol_state_t *state, timediff_t duration);
int upnp_map(protocol_state_t *state, const client_mapping_t *mapping,
             protocol_map_output_t *output, timediff_t duration);
int upnp_unmap(protocol_state_t *state, const client_mapping_t *mapping, timediff_t duration);
int upnp_idle(protocol_state_t *state, timediff_t duration);
int upnp_interrupt(protocol_state_t *state, bool hard);

int upnp_impl_probe(upnp_impl_t *impl, addr_record_t *found_gateway, timestamp_t end_timestamp,
                    timestamp_t query_end_timestamp);
int upnp_impl_query_control_url(upnp_impl_t *impl, timestamp_t end_timestamp);
int upnp_impl_query_external_addr(upnp_impl_t *impl, timestamp_t end_timestamp);
int upnp_impl_map(upnp_impl_t *impl, plum_ip_protocol_t protocol, uint16_t external_port,
                  uint16_t internal_port, unsigned int lifetime, timestamp_t end_timestamp);
int upnp_impl_unmap(upnp_impl_t *impl, plum_ip_protocol_t protocol, uint16_t external_port,
                    timestamp_t end_timestamp);

int upnp_impl_wait_response(upnp_impl_t *impl, char *buffer, size_t size, addr_record_t *src,
                            timestamp_t end_timestamp, bool interruptible);

#endif
