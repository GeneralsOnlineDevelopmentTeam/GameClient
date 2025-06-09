/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_PROTOCOL_H
#define PLUM_PROTOCOL_H

#include "addr.h"
#include "timestamp.h"

typedef struct {
	addr_record_t gateway;
	void *impl;
} protocol_state_t;

struct client_mapping;

#define PROTOCOL_ERR_SUCCESS 0
#define PROTOCOL_ERR_UNKNOWN -1
#define PROTOCOL_ERR_INTERRUPTED -2
#define PROTOCOL_ERR_TIMEOUT -3
#define PROTOCOL_ERR_RESET -4
#define PROTOCOL_ERR_PROTOCOL_FAILED -5
#define PROTOCOL_ERR_NETWORK_FAILED -6
#define PROTOCOL_ERR_INSUFF_RESOURCES -7
#define PROTOCOL_ERR_UNSUPP_PROTOCOL -8
#define PROTOCOL_ERR_UNSUPP_VERSION -9
#define PROTOCOL_ERR_SKIPPED -10
#define PROTOCOL_ERR_NEED_DOWNGRADE -11

typedef enum {
	PROTOCOL_MAP_STATE_SUCCESS = 0,
	PROTOCOL_MAP_STATE_FAILURE = 1
} protocol_map_state_t;

typedef struct {
	protocol_map_state_t state;
	addr_record_t external_addr;
	timestamp_t refresh_timestamp;
	void *impl_record;
} protocol_map_output_t;

typedef struct {
	int (*init)(protocol_state_t *state);
	int (*cleanup)(protocol_state_t *state);
	int (*discover)(protocol_state_t *state, timediff_t duration);
	int (*map)(protocol_state_t *state, const struct client_mapping *mapping,
	           protocol_map_output_t *output, timediff_t duration);
	int (*unmap)(protocol_state_t *state, const struct client_mapping *mapping, timediff_t duration);
	int (*idle)(protocol_state_t *state, timediff_t duration);
	int (*interrupt)(protocol_state_t *state, bool hard);
} protocol_t;

#endif
