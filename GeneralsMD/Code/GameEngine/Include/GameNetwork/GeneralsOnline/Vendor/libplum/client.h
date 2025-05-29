/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_CLIENT_H
#define PLUM_CLIENT_H

#include "addr.h"
#include "plum.h"
#include "protocol.h"
#include "thread.h"
#include "timestamp.h"

#define CLIENT_MAX_DISCOVER_TIMEOUT 10000 // msecs
#define CLIENT_MAX_MAPPING_TIMEOUT 10000  // msecs
#define CLIENT_RECHECK_PERIOD 300000      // msecs

typedef struct client_mapping {
	plum_ip_protocol_t protocol;
	uint16_t internal_port;
	addr_record_t suggested_addr;
	addr_record_t external_addr;
	plum_mapping_callback_t callback;
	void *user_ptr;
	plum_state_t state;
	timestamp_t refresh_timestamp;
	void *impl_record;
} client_mapping_t;

typedef struct {
	client_mapping_t *mappings;
	int mappings_size;
	mutex_t mappings_mutex;
	const protocol_t *protocol;
	protocol_state_t protocol_state;
	mutex_t protocol_mutex;
	bool is_started;
	atomic(bool) is_stopping;
	thread_t thread;
} client_t;

client_t *client_create(void);
void client_destroy(client_t *client);
int client_start(client_t *client);
int client_add_mapping(client_t *client, const plum_mapping_t *mapping,
                       plum_mapping_callback_t callback);
int client_get_mapping(client_t *client, int i, plum_state_t *state, plum_mapping_t *mapping);
int client_remove_mapping(client_t *client, int i);
void client_run(client_t *client);
int client_run_protocol(client_t *client, const protocol_t *protocol,
                        protocol_state_t *protocol_state, timediff_t duration);
int client_interrupt(client_t *client, bool stop);

#endif
