/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "plum.h"
#include "addr.h"
#include "client.h"
#include "dummytls.h"
#include "log.h"
#include "lprandom.h"
#include "net.h"

#include <string.h>

static client_t *client = NULL;

PLUM_EXPORT int plum_init(const plum_config_t *config) {
	if (client)
		return PLUM_ERR_FAILED;

	plum_log_init();
	plum_set_log_level(config->log_level);
	plum_set_log_handler(config->log_callback);

	plum_random_init();

	dummytls_init();
	if (config->dummytls_domain)
		dummytls_set_domain(config->dummytls_domain);

	client = client_create();
	if (!client)
		return PLUM_ERR_FAILED;

	return PLUM_ERR_SUCCESS;
}

PLUM_EXPORT int plum_cleanup() {
	if (!client)
		return PLUM_ERR_FAILED;

	client_destroy(client);
	client = NULL;

	dummytls_cleanup();

	plum_log_cleanup();
	plum_random_cleanup();

	return PLUM_ERR_SUCCESS;
}

PLUM_EXPORT int plum_create_mapping(const plum_mapping_t *mapping, plum_mapping_callback_t callback) {
	if (!client)
		return PLUM_ERR_FAILED;

	if (!mapping)
		return PLUM_ERR_INVALID;

	int id = client_add_mapping(client, mapping, callback);
	if (id < 0)
		return PLUM_ERR_FAILED;

	// Ensure client is started
	if (client_start(client) < 0)
		return PLUM_ERR_FAILED;

	return id;
}

PLUM_EXPORT int plum_query_mapping(int id, plum_state_t *state, plum_mapping_t *mapping) {
	if (!client)
		return PLUM_ERR_FAILED;

	if (id < 0)
		return PLUM_ERR_INVALID;

	if (client_get_mapping(client, id, state, mapping) < 0)
		return PLUM_ERR_NOT_AVAIL;

	return PLUM_ERR_SUCCESS;
}

PLUM_EXPORT int plum_destroy_mapping(int id) {
	if (!client)
		return PLUM_ERR_FAILED;

	if (id < 0)
		return PLUM_ERR_INVALID;

	if (client_remove_mapping(client, id) < 0)
		return PLUM_ERR_NOT_AVAIL;

	return PLUM_ERR_SUCCESS;
}

PLUM_EXPORT int plum_get_local_address(char *buffer, size_t size) {
	addr_record_t record;
	if(net_get_default_interface(AF_INET, &record) < 0)
		return PLUM_ERR_NOT_AVAIL;

	int len = addr_get_host((const struct sockaddr *)&record.addr, buffer, size);
	if(len < 0)
		return PLUM_ERR_FAILED;

	return len;
}

PLUM_EXPORT int plum_get_dummytls_certificate(plum_dummytls_cert_type_t type, char *buffer, size_t size) {
	if (!buffer && size)
		return PLUM_ERR_INVALID;

	if (dummytls_renew_certs() < 0)
		return PLUM_ERR_FAILED;

	int len = dummytls_get_cert(type, buffer, size);
	if (len < 0)
		return PLUM_ERR_NOT_AVAIL;

	return len;
}

PLUM_EXPORT int plum_get_dummytls_host(const char *address, char *buffer, size_t size) {
	if (!address || (!buffer && size))
		return PLUM_ERR_INVALID;

	addr_record_t record;
	const uint16_t dummy_port = 443;
	if (addr_set(AF_UNSPEC, address, dummy_port, &record) < 0)
		return PLUM_ERR_INVALID;

	int len = dummytls_get_host((const struct sockaddr *)&record.addr, buffer, size);
	if (len < 0)
		return PLUM_ERR_FAILED;

	return len;
}
