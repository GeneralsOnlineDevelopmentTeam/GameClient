/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_TCP_H
#define PLUM_TCP_H

#include "addr.h"
#include "socket.h"
#include "timestamp.h"

#include <stdint.h>

#define TCP_ERR_UNKNOWN -1
#define TCP_ERR_TIMEOUT -2

socket_t tcp_connect_socket(const addr_record_t *remote_addr, timestamp_t end_timestamp);
int tcp_recv(socket_t sock, char *buffer, size_t size, timestamp_t end_timestamp);
int tcp_send(socket_t sock, const char *data, size_t size, timestamp_t end_timestamp);

#endif // PLUM_UDP_H
