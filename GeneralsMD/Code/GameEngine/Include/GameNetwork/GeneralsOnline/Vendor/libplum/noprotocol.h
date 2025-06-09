/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_NOPROTOCOL_H
#define PLUM_NOPROTOCOL_H

#include "client.h"
#include "protocol.h"
#include "timestamp.h"
#include "thread.h"

#include <stdbool.h>

typedef struct {
	mutex_t mutex;
	cond_t interrupt_cond;
	bool interrupted;
} noprotocol_impl_t;

int noprotocol_init(protocol_state_t *state);
int noprotocol_cleanup(protocol_state_t *state);
int noprotocol_discover(protocol_state_t *state, timediff_t duration);
int noprotocol_map(protocol_state_t *state, const client_mapping_t *mapping,
                   protocol_map_output_t *output, timediff_t duration);
int noprotocol_unmap(protocol_state_t *state, const client_mapping_t *mapping, timediff_t duration);
int noprotocol_idle(protocol_state_t *state, timediff_t duration);
int noprotocol_interrupt(protocol_state_t *state, bool hard);

#endif
