/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_UDP_H
#define PLUM_UDP_H

#include "addr.h"
#include "socket.h"
#include "timestamp.h"

#include <stdint.h>

typedef struct udp_socket_config {
	int family;               // AF_UNSPEC means any
	uint16_t port;            // 0 means any
	const char *bind_address; // NULL means any
	const char *multicast_group;
	bool enable_broadcast;
	bool enable_reuseaddr;
	bool enable_dontfrag;
} udp_socket_config_t;

socket_t udp_create_socket(const udp_socket_config_t *config);
int udp_recvfrom(socket_t sock, char *buffer, size_t size, addr_record_t *src);
int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst);
int udp_sendto_self(socket_t sock, const char *data, size_t size);
int udp_set_diffserv(socket_t sock, int ds);
uint16_t udp_get_port(socket_t sock);
int udp_get_bound_addr(socket_t sock, addr_record_t *record);
int udp_get_local_addr(socket_t sock, int family_hint,
                       addr_record_t *record); // family_hint may be AF_UNSPEC

#endif // PLUM_UDP_H
