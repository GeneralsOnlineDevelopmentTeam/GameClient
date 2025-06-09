/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "client.h"
#include "addr.h"
#include "dummytls.h"
#include "log.h"
#include "net.h"
#include "noprotocol.h"
#include "pcp.h"
#include "random.h"
#include "upnp.h"

#include <string.h>

#define DEFAULT_MAPPINGS_SIZE 16

#define PROTOCOL_PCP 0
#define PROTOCOL_UPNP 1
#define PROTOCOL_NOPROTOCOL 2
#define PROTOCOLS_COUNT 3

static const protocol_t protocols[PROTOCOLS_COUNT] = {
    {pcp_init, pcp_cleanup, pcp_discover, pcp_map, pcp_unmap, pcp_idle, pcp_interrupt},
    {upnp_init, upnp_cleanup, upnp_discover, upnp_map, upnp_unmap, upnp_idle, upnp_interrupt},
    {noprotocol_init, noprotocol_cleanup, noprotocol_discover, noprotocol_map, noprotocol_unmap,
     noprotocol_idle, noprotocol_interrupt}};

static int find_empty_mapping_index(client_t *client) {
	for (int i = 0; i < client->mappings_size; ++i)
		if (client->mappings[i].internal_port == 0)
			return i;

	return -1;
}

static void import_mapping(const plum_mapping_t *mapping, plum_mapping_callback_t callback,
                           client_mapping_t *cm) {
	memset(cm, 0, sizeof(*cm));
	cm->callback = callback;
	cm->protocol = mapping->protocol;
	cm->internal_port = mapping->internal_port;
	cm->user_ptr = mapping->user_ptr;

	if (*mapping->external_host == '\0' ||
	    addr_set(AF_UNSPEC, mapping->external_host, mapping->external_port, &cm->suggested_addr) <
	        0)
		if (mapping->external_port == 0 ||
		    addr_set(AF_INET, "0.0.0.0", mapping->external_port, &cm->suggested_addr) < 0)
			memset(&cm->suggested_addr, 0, sizeof(cm->suggested_addr));
}

static void export_mapping(const client_mapping_t *cm, plum_mapping_t *mapping) {
	memset(mapping, 0, sizeof(*mapping));
	mapping->protocol = cm->protocol;
	mapping->internal_port = cm->internal_port;
	mapping->user_ptr = cm->user_ptr;
	if (cm->external_addr.len > 0) {
		const struct sockaddr *sa = (const struct sockaddr *)&cm->external_addr.addr;
		mapping->external_port = addr_get_port(sa);
		if (dummytls_get_host(sa, mapping->external_host, PLUM_MAX_HOST_LEN) < 0)
			addr_get_host(sa, mapping->external_host, PLUM_MAX_HOST_LEN);
	}
}

thread_return_t THREAD_CALL client_thread_entry(void *arg) {
	client_run((client_t *)arg);
	return (thread_return_t)0;
}

client_t *client_create(void) {
	PLUM_LOG_DEBUG("Creating client");

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		PLUM_LOG_FATAL("WSAStartup failed");
		return NULL;
	}
#endif

	client_t *client = (client_t*)malloc(sizeof(client_t));
	if (!client) {
		PLUM_LOG_FATAL("Allocation failed for client");
		return NULL;
	}
	memset(client, 0, sizeof(*client));

	client->mappings = (client_mapping_t*)malloc(DEFAULT_MAPPINGS_SIZE * sizeof(client_mapping_t));
	if (!client->mappings) {
		PLUM_LOG_FATAL("Allocation failed for mappings");
		free(client);
		return NULL;
	}

	memset(client->mappings, 0, DEFAULT_MAPPINGS_SIZE * sizeof(client_mapping_t));
	client->mappings_size = DEFAULT_MAPPINGS_SIZE;

	mutex_init(&client->mappings_mutex, MUTEX_RECURSIVE); // so the user call the API from callbacks
	mutex_init(&client->protocol_mutex, 0);

	return client;
}

void client_destroy(client_t *client) {
	PLUM_LOG_DEBUG("Destroying client");

	if (client->is_started) {
		client_interrupt(client, true); // stop
		thread_join(client->thread, NULL);
	}

	mutex_destroy(&client->mappings_mutex);
	mutex_destroy(&client->protocol_mutex);

	free(client->mappings);
	free(client);

#ifdef _WIN32
	WSACleanup();
#endif

	PLUM_LOG_VERBOSE("Destroyed client");
}

int client_start(client_t *client) {
	mutex_lock(&client->protocol_mutex);
	if (client->is_started) {
		PLUM_LOG_DEBUG("Client is already started");
		mutex_unlock(&client->protocol_mutex);
		return 0;
	}

	int ret = thread_init(&client->thread, client_thread_entry, client);
	if (ret) {
		PLUM_LOG_FATAL("Thread creation failed, error=%d", ret);
		mutex_unlock(&client->protocol_mutex);
		return -1;
	}

	client->is_started = true;
	mutex_unlock(&client->protocol_mutex);
	return 0;
}

int client_add_mapping(client_t *client, const plum_mapping_t *mapping,
                       plum_mapping_callback_t callback) {
	if (!mapping)
		return -1;

	mutex_lock(&client->mappings_mutex);

	int i = find_empty_mapping_index(client);
	if (i < 0) {
		i = client->mappings_size;

		client_mapping_t *new_mappings =
		    (client_mapping_t*)realloc(client->mappings, client->mappings_size * 2 * sizeof(client_mapping_t));
		if (!new_mappings) {
			PLUM_LOG_ERROR("Reallocation failed for mappings");
			mutex_unlock(&client->mappings_mutex);
			return -1;
		}

		client->mappings = new_mappings;
		memset(client->mappings + client->mappings_size, 0,
		       client->mappings_size * sizeof(client_mapping_t));
		client->mappings_size *= 2;
	}

	client_mapping_t *cm = client->mappings + i;
	import_mapping(mapping, callback, cm);
	cm->state = PLUM_STATE_PENDING;
	cm->refresh_timestamp = 0;

	PLUM_LOG_INFO("Added mapping %d for internal port %hu (callback=%d)", i, cm->internal_port,
	              (int)(cm->callback != NULL));

	mutex_unlock(&client->mappings_mutex);

	client_interrupt(client, false);
	return i;
}

int client_get_mapping(client_t *client, int i, plum_state_t *state, plum_mapping_t *mapping) {
	mutex_lock(&client->mappings_mutex);

	if (i >= client->mappings_size || client->mappings[i].state == PLUM_STATE_DESTROYED) {
		mutex_unlock(&client->mappings_mutex);
		return -1;
	}

	client_mapping_t *cm = client->mappings + i;
	if (mapping)
		export_mapping(cm, mapping);

	if (state)
		*state = cm->state;

	mutex_unlock(&client->mappings_mutex);
	return 0;
}

int client_remove_mapping(client_t *client, int i) {
	mutex_lock(&client->mappings_mutex);

	if (i >= client->mappings_size || client->mappings[i].state == PLUM_STATE_DESTROYED) {
		mutex_unlock(&client->mappings_mutex);
		return -1;
	}

	client_mapping_t *cm = client->mappings + i;
	cm->state = PLUM_STATE_DESTROYING;

	PLUM_LOG_INFO("Removed mapping %d for internal port %hu", i, cm->internal_port);

	mutex_unlock(&client->mappings_mutex);

	client_interrupt(client, false);
	return 0;
}

static void trigger_mapping_callback(const client_mapping_t *cm, int i) {
	plum_mapping_t mapping;
	export_mapping(cm, &mapping);
	if (cm->callback)
		cm->callback(i, cm->state, &mapping);
}

static void update_mapping(client_mapping_t *cm, int i, plum_state_t state,
                           const addr_record_t *external) {
	if (cm->state == PLUM_STATE_DESTROYED || cm->state == PLUM_STATE_DESTROYING)
		return;

	bool changed = false;
	if (state == PLUM_STATE_SUCCESS && external) {
		changed = !addr_record_is_equal(&cm->external_addr, external, true);
		cm->external_addr = *external;
	} else {
		memset(&cm->external_addr, 0, sizeof(cm->external_addr));
	}

	if (state != cm->state || changed) {
		cm->state = state;

		if (state != PLUM_STATE_PENDING && state != PLUM_STATE_DESTROYING)
			trigger_mapping_callback(cm, i);
	}
}

static void destroy_mapping(client_mapping_t *cm, int i) {
	memset(&cm->external_addr, 0, sizeof(cm->external_addr));
	update_mapping(cm, i, PLUM_STATE_DESTROYED, NULL);
	free(cm->impl_record);
	memset(cm, 0, sizeof(*cm));
}

static bool has_destroying_mappings(client_t *client) {
	mutex_lock(&client->mappings_mutex);
	for (int i = 0; i < client->mappings_size; ++i) {
		client_mapping_t *cm = client->mappings + i;
		if (cm->state == PLUM_STATE_DESTROYING)
			return true;
	}
	mutex_unlock(&client->mappings_mutex);
	return false;
}

static void reset_protocol(client_t *client) {
	// protocol_mutex must be locked

	PLUM_LOG_VERBOSE("Resetting protocol state");

	if (client->protocol) {
		client->protocol->cleanup(&client->protocol_state);
		client->protocol = NULL;
	}

	// Also reset timestamps and records
	mutex_lock(&client->mappings_mutex);
	for (int i = 0; i < client->mappings_size; ++i) {
		client_mapping_t *cm = client->mappings + i;
		if (cm->state == PLUM_STATE_DESTROYED)
			continue;

		cm->refresh_timestamp = 0;
		free(cm->impl_record);
		cm->impl_record = NULL;
		memset(&cm->external_addr, 0, sizeof(cm->external_addr));
		if (cm->state == PLUM_STATE_DESTROYING)
			destroy_mapping(cm, i); // as good as destroyed now
		else
			update_mapping(cm, i, PLUM_STATE_PENDING, NULL);
	}
	mutex_unlock(&client->mappings_mutex);
}

void client_run(client_t *client) {
	PLUM_LOG_DEBUG("Starting client thread");
	mutex_lock(&client->protocol_mutex);

	addr_record_t old_local;
	memset(&old_local, 0, sizeof(old_local));
	int protocol_num = 0;
	while (true) {
		addr_record_t local;
		if (net_get_default_interface(AF_INET, &local) < 0) {
			PLUM_LOG_ERROR("Unable to get default interface address");
			break;
		}

		bool changed = old_local.len > 0 && !addr_record_is_equal(&old_local, &local, false);
		old_local = local;
		if (changed) {
			PLUM_LOG_INFO("Local address changed, restarting");
			reset_protocol(client);
			protocol_num = 0;
		}

		if (protocol_num != PROTOCOL_NOPROTOCOL &&
		    addr_is_public((const struct sockaddr *)&local)) {
			PLUM_LOG_DEBUG("Local address is public, skipping discovery");
			reset_protocol(client);
			protocol_num = PROTOCOL_NOPROTOCOL;
		}

		int err = PROTOCOL_ERR_SUCCESS;
		if (!client->protocol) {
			client->protocol = protocols + protocol_num;
			err = client->protocol->init(&client->protocol_state);
			if (err != PROTOCOL_ERR_SUCCESS) {
				client->protocol = NULL;
			}
		}

		if (err == PROTOCOL_ERR_SUCCESS) {
			mutex_unlock(&client->protocol_mutex);
			err = client_run_protocol(client, client->protocol, &client->protocol_state,
			                          CLIENT_RECHECK_PERIOD);
			mutex_lock(&client->protocol_mutex);
		}

		if (err == PROTOCOL_ERR_SUCCESS || err == PROTOCOL_ERR_INTERRUPTED) {
			if (err == PROTOCOL_ERR_INTERRUPTED)
				PLUM_LOG_DEBUG("Protocol was interrupted");

			if (atomic_load(&client->is_stopping)) {
				if (!has_destroying_mappings(client)) {
					PLUM_LOG_DEBUG("Client is stopping, exiting");
					break;
				}
				PLUM_LOG_DEBUG("Mappings are marked for destruction, continuing");
			}
			continue;
		}

		if (atomic_load(&client->is_stopping))
			break;

		reset_protocol(client);

		if (err == PROTOCOL_ERR_RESET) {
			PLUM_LOG_DEBUG("Protocol was reset");
			continue;
		}

		PLUM_LOG_DEBUG("Protocol failed");

		++protocol_num;
		if (protocol_num >= PROTOCOLS_COUNT) {
			PLUM_LOG_FATAL("Client failed, exiting");
			break;
		}
	}

	reset_protocol(client);

	PLUM_LOG_DEBUG("Exiting client thread");
	mutex_unlock(&client->protocol_mutex);
}

int client_run_protocol(client_t *client, const protocol_t *protocol,
                        protocol_state_t *protocol_state, timediff_t duration) {
	timestamp_t end_timestamp = current_timestamp() + duration;

	int err = protocol->discover(protocol_state, CLIENT_MAX_DISCOVER_TIMEOUT);
	if (err != PROTOCOL_ERR_SUCCESS)
		return err;

	while (current_timestamp() < end_timestamp) {
		timestamp_t next_timestamp = end_timestamp;
		int i = 0;
		while (true) {
			mutex_lock(&client->mappings_mutex);
			if (i >= client->mappings_size) {
				mutex_unlock(&client->mappings_mutex);
				break;
			}
			client_mapping_t *cm = client->mappings + i;
			if (cm->state == PLUM_STATE_DESTROYED) {
				mutex_unlock(&client->mappings_mutex);
				++i;
				continue;
			}
			client_mapping_t mapping = *cm;
			mutex_unlock(&client->mappings_mutex);

			PLUM_LOG_VERBOSE("Mapping %d for internal port %hu is alive", i, mapping.internal_port);

			if (mapping.state == PLUM_STATE_DESTROYING) {
				PLUM_LOG_INFO("Performing unmapping for internal port %hu", mapping.internal_port);

				err = protocol->unmap(protocol_state, &mapping, CLIENT_MAX_MAPPING_TIMEOUT);
				if (err != PROTOCOL_ERR_SUCCESS)
					return err;

				PLUM_LOG_INFO("Unmapped internal port %hu", mapping.internal_port);

				mutex_lock(&client->mappings_mutex);
				client_mapping_t *cm = client->mappings + i;
				destroy_mapping(cm, i);
				mutex_unlock(&client->mappings_mutex);

			} else if (current_timestamp() >= mapping.refresh_timestamp) {
				PLUM_LOG_INFO("Performing mapping for internal port %hu", mapping.internal_port);

				protocol_map_output_t output;
				err = protocol->map(protocol_state, &mapping, &output, CLIENT_MAX_MAPPING_TIMEOUT);
				if (err != PROTOCOL_ERR_SUCCESS)
					return err;

				if (PLUM_LOG_INFO_ENABLED) {
					if (output.state == PROTOCOL_MAP_STATE_SUCCESS) {
						char external_str[ADDR_MAX_STRING_LEN];
						addr_record_to_string(&output.external_addr, external_str,
						                      ADDR_MAX_STRING_LEN);
						PLUM_LOG_INFO(
						    "Mapping succeeded for internal port %hu, external address is %s",
						    mapping.internal_port, external_str);
					} else {
						PLUM_LOG_INFO("Mapping failed for internal port %hu",
						              mapping.internal_port);
					}
				}

				int refresh_delay = (int)(output.refresh_timestamp - current_timestamp());
				PLUM_LOG_DEBUG("Mapping will be refreshed in %dms", (int)refresh_delay);

				mutex_lock(&client->mappings_mutex);
				client_mapping_t *cm = client->mappings + i;
				free(cm->impl_record);
				cm->impl_record = output.impl_record;
				cm->refresh_timestamp = output.refresh_timestamp;

				if (cm->state != PLUM_STATE_DESTROYING) { // mapping might have been destroyed
					if (output.state == PROTOCOL_MAP_STATE_SUCCESS) {
						update_mapping(cm, i, PLUM_STATE_SUCCESS, &output.external_addr);
					} else {
						update_mapping(cm, i, PLUM_STATE_FAILURE, NULL);
					}
				}
				mutex_unlock(&client->mappings_mutex);
			}

			if (next_timestamp > mapping.refresh_timestamp)
				next_timestamp = mapping.refresh_timestamp;

			++i;
		}

		if (atomic_load(&client->is_stopping))
			break;

		timestamp_t now = current_timestamp();
		if (now < next_timestamp) {
			timediff_t diff = next_timestamp - now;
			if (diff > CLIENT_RECHECK_PERIOD)
				diff = CLIENT_RECHECK_PERIOD;

			err = protocol->idle(protocol_state, diff);
			if (err != PROTOCOL_ERR_SUCCESS && err != PROTOCOL_ERR_TIMEOUT)
				return err;
		}
	}

	return PROTOCOL_ERR_SUCCESS;
}

int client_interrupt(client_t *client, bool stop) {
	PLUM_LOG_DEBUG("Interrupting protocol");
	mutex_lock(&client->protocol_mutex);

	if (!client->is_started) {
		mutex_unlock(&client->protocol_mutex);
		return PROTOCOL_ERR_SUCCESS;
	}

	if (stop)
		atomic_store(&client->is_stopping, true);

	if (client->protocol) {
		int err = client->protocol->interrupt(&client->protocol_state, false);
		if (err < 0) {
			mutex_unlock(&client->protocol_mutex);
			return err;
		}
	}

	mutex_unlock(&client->protocol_mutex);
	return PROTOCOL_ERR_SUCCESS;
}
