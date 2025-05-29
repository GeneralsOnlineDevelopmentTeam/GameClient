/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "pcp.h"
#include "client.h"
#include "log.h"
#include "libplum/natpmp.h"
#include "net.h"
#include "lprandom.h"
#include "udp.h"

#include <stdlib.h>
#include <string.h>

int pcp_init(protocol_state_t *state) {
	PLUM_LOG_VERBOSE("Initializing PCP/NAT-PMP state");
	memset(state, 0, sizeof(*state));

	state->impl = malloc(sizeof(pcp_impl_t));
	if (!state->impl) {
		PLUM_LOG_ERROR("Allocation for PCP/NAT-PMP state failed");
		return PROTOCOL_ERR_INSUFF_RESOURCES;
	}

	pcp_impl_t *impl = (pcp_impl_t*)state->impl;
	memset(impl, 0, sizeof(*impl));
	impl->sock = INVALID_SOCKET;
	impl->mcast_sock = INVALID_SOCKET;
	impl->has_prev_server_time = false;
	impl->use_natpmp = false;
	impl->interrupt = PCP_INTERRUPT_NONE;

	udp_socket_config_t udp_config;
	memset(&udp_config, 0, sizeof(udp_config));
	udp_config.family = AF_INET;
	udp_config.port = 0;
	impl->sock = udp_create_socket(&udp_config);
	if (impl->sock == INVALID_SOCKET) {
		PLUM_LOG_ERROR("UDP socket creation failed");
		goto error;
	}

	udp_socket_config_t mcast_udp_config;
	memset(&mcast_udp_config, 0, sizeof(mcast_udp_config));
	mcast_udp_config.family = AF_INET;
	mcast_udp_config.port = PCP_CLIENT_PORT;
	mcast_udp_config.multicast_group = "224.0.0.1";
	mcast_udp_config.enable_reuseaddr = true;
	impl->mcast_sock = udp_create_socket(&mcast_udp_config);
	if (impl->mcast_sock == INVALID_SOCKET) {
		PLUM_LOG_ERROR("Multicast UDP socket creation on port %d failed", PCP_CLIENT_PORT);
		goto error;
	}

	return PROTOCOL_ERR_SUCCESS;

error:
	if (impl->sock != INVALID_SOCKET)
		closesocket(impl->sock);

	if (impl->mcast_sock != INVALID_SOCKET)
		closesocket(impl->mcast_sock);

	free(state->impl);
	state->impl = NULL;
	return PROTOCOL_ERR_INSUFF_RESOURCES;
}

int pcp_cleanup(protocol_state_t *state) {
	PLUM_LOG_VERBOSE("Cleaning up PCP/NAT-PMP state");

	pcp_impl_t *impl = (pcp_impl_t*)state->impl;
	closesocket(impl->sock);
	closesocket(impl->mcast_sock);

	free(state->impl);
	state->impl = NULL;
	return PROTOCOL_ERR_SUCCESS;
}

int pcp_discover(protocol_state_t *state, timediff_t duration) {
	pcp_impl_t *impl = (pcp_impl_t*)state->impl;
	timestamp_t end_timestamp = current_timestamp() + duration;

	// RFC 6886: To determine the external IPv4 address, or to request a port mapping, a NAT-PMP
	// client sends its request packet to port 5351 of its configured gateway address, and waits 250
	// ms for a response.  If no NAT-PMP response is received from the gateway after 250 ms, the
	// client retransmits its request and waits 500 ms.  The client SHOULD repeat this process with
	// the interval between attempts doubling each time.
	int probe_count = 0;
	timediff_t probe_duration = 250;
	do {
		timestamp_t probe_end_timestamp = current_timestamp() + probe_duration;
		if (probe_end_timestamp > end_timestamp)
			probe_end_timestamp = end_timestamp;

		int err;
		if (impl->use_natpmp) {
			PLUM_LOG_DEBUG("Probing NAT-PMP...");
			err = natpmp_impl_probe(impl, &state->gateway, probe_end_timestamp);
		} else {
			PLUM_LOG_DEBUG("Probing PCP...");
			err = pcp_impl_probe(impl, &state->gateway, probe_end_timestamp);
			if (err == PROTOCOL_ERR_NEED_DOWNGRADE) {
				PLUM_LOG_DEBUG("Falling back to NAT-PMP");
				impl->use_natpmp = true;
				continue;
			}
		}
		if (err == PROTOCOL_ERR_SUCCESS) {
			if (PLUM_LOG_INFO_ENABLED) {
				char gateway_str[ADDR_MAX_STRING_LEN];
				addr_record_to_string(&state->gateway, gateway_str, ADDR_MAX_STRING_LEN);
				if (impl->use_natpmp)
					PLUM_LOG_INFO("Success probing NAT-PMP, gateway address is %s", gateway_str);
				else
					PLUM_LOG_INFO("Success probing PCP, gateway address is %s", gateway_str);
			}
			return PROTOCOL_ERR_SUCCESS;
		}

		if (err != PROTOCOL_ERR_TIMEOUT)
			return err;

		probe_duration *= 2;
	} while (++probe_count < PCP_MAX_ATTEMPTS && current_timestamp() < end_timestamp);

	return PROTOCOL_ERR_TIMEOUT;
}

int pcp_map(protocol_state_t *state, const client_mapping_t *mapping, protocol_map_output_t *output,
            timediff_t duration) {
	pcp_impl_t *impl = (pcp_impl_t*)state->impl;
	timestamp_t end_timestamp = current_timestamp() + duration;

	int map_count = 0;
	timediff_t map_duration = 250;
	do {
		timestamp_t map_end_timestamp = current_timestamp() + map_duration;
		if (map_end_timestamp > end_timestamp)
			map_end_timestamp = end_timestamp;

		int err;
		if (impl->use_natpmp) {
			// RFC 6886: The RECOMMENDED Port Mapping Lifetime is 7200 seconds (two hours).
			const uint32_t lifetime = 7200; // seconds

			PLUM_LOG_DEBUG("Mapping with NAT-PMP...");
			err = natpmp_impl_map(impl, mapping, output, lifetime, &state->gateway,
			                      map_end_timestamp);
		} else {
			// RFC 6887: The PCP client requests a certain lifetime, and the PCP server responds
			// with the assigned lifetime. The PCP server MAY grant a lifetime smaller or larger
			// than the requested lifetime. The PCP server SHOULD be configurable for permitted
			// minimum and maximum lifetime, and the minimum value SHOULD be 120 seconds. The
			// maximum value SHOULD be the remaining lifetime of the IP address assigned to the PCP
			// client if that information is available (e.g., from the DHCP server), or half the
			// lifetime of IP address assignments on that network if the remaining lifetime is not
			// available, or 24 hours. RFC 6886: The RECOMMENDED Port Mapping Lifetime is 7200
			// seconds (two hours).
			const uint32_t lifetime = 7200; // seconds

			PLUM_LOG_DEBUG("Mapping with PCP...");
			err = pcp_impl_map(impl, mapping, output, lifetime, &state->gateway, map_end_timestamp);
		}

		if (err == PROTOCOL_ERR_SUCCESS) {
			if (impl->use_natpmp)
				PLUM_LOG_DEBUG("Success mapping with NAT-PMP");
			else
				PLUM_LOG_DEBUG("Success mapping with PCP");

			return PROTOCOL_ERR_SUCCESS;
		}

		if (err != PROTOCOL_ERR_TIMEOUT)
			return err;

		map_duration *= 2;
	} while (++map_count < PCP_MAX_ATTEMPTS && current_timestamp() < end_timestamp);

	return PROTOCOL_ERR_TIMEOUT;
}

int pcp_unmap(protocol_state_t *state, const client_mapping_t *mapping, timediff_t duration) {
	pcp_impl_t *impl = (pcp_impl_t*)state->impl;
	timestamp_t end_timestamp = current_timestamp() + duration;

	const uint32_t lifetime = 0;  // We want to unmap
	protocol_map_output_t output; // dummy
	int map_count = 0;
	timediff_t map_duration = 250;
	do {
		timestamp_t map_end_timestamp = current_timestamp() + map_duration;
		if (map_end_timestamp > end_timestamp)
			map_end_timestamp = end_timestamp;

		int err;
		if (impl->use_natpmp) {
			PLUM_LOG_DEBUG("Unmapping with NAT-PMP...");
			err = natpmp_impl_map(impl, mapping, &output, lifetime, &state->gateway,
			                      map_end_timestamp);
		} else {
			PLUM_LOG_DEBUG("Unmapping with PCP...");
			err =
			    pcp_impl_map(impl, mapping, &output, lifetime, &state->gateway, map_end_timestamp);
		}

		if (err == PROTOCOL_ERR_SUCCESS) {
			if (impl->use_natpmp)
				PLUM_LOG_DEBUG("Success unmapping with NAT-PMP");
			else
				PLUM_LOG_DEBUG("Success unmapping with PCP");

			free(output.impl_record);
			return PROTOCOL_ERR_SUCCESS;
		}

		if (err != PROTOCOL_ERR_TIMEOUT)
			return err;

		map_duration *= 2;
	} while (++map_count < PCP_MAX_ATTEMPTS && current_timestamp() < end_timestamp);

	return PROTOCOL_ERR_TIMEOUT;
}

int pcp_idle(protocol_state_t *state, timediff_t duration) {
	pcp_impl_t *impl = (pcp_impl_t*)state->impl;
	timestamp_t end_timestamp = current_timestamp() + duration;

	char buffer[PCP_MAX_PAYLOAD_LENGTH];
	addr_record_t src;
	int len;
	while ((len = pcp_natpmp_impl_wait_response(impl, buffer, &src, end_timestamp, true)) >= 0) {
		PLUM_LOG_DEBUG("Unexpected datagram, ignoring");
	}

	return len; // len < 0
}

int pcp_interrupt(protocol_state_t *state, bool hard) {
	pcp_impl_t *impl = (pcp_impl_t*)state->impl;

	PLUM_LOG_VERBOSE("Interrupting PCP/NAT-PMP operation");
	atomic_store(&impl->interrupt, hard ? PCP_INTERRUPT_HARD : PCP_INTERRUPT_SOFT);

	char dummy = 0; // Some C libraries might error out on NULL pointers
	if (udp_sendto_self(impl->sock, &dummy, 0) < 0) {
		if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK) {
			PLUM_LOG_WARN(
			    "Failed to interrupt PCP/NAT-PMP operation by triggering socket, errno=%d",
			    sockerrno);
			return PROTOCOL_ERR_UNKNOWN;
		}
	}

	return PROTOCOL_ERR_SUCCESS;
}

static int write_pcp_header(struct pcp_request_header *header, uint8_t opcode, uint32_t lifetime) {
	memset(header, 0, sizeof(*header));
	header->version = PCP_VERSION;
	header->opcode = opcode;
	header->lifetime = lifetime;

	addr_record_t local;
	if (net_get_default_interface(AF_INET, &local)) {
		PLUM_LOG_ERROR("Unable to get default interface address");
		return PROTOCOL_ERR_UNKNOWN;
	}

	// RFC 6887: When the address field holds an IPv4 address, an IPv4-mapped IPv6 address
	// [RFC4291] is used.
	addr_map_inet6_v4mapped(&local.addr, &local.len);
	struct sockaddr_in6 *local_sin6 = (struct sockaddr_in6 *)&local.addr;
	memcpy(header->client_address, &local_sin6->sin6_addr, 16);
	return 0;
}

int pcp_impl_probe(pcp_impl_t *impl, addr_record_t *found_gateway, timestamp_t end_timestamp) {
	addr_record_t gateway;
	if (net_get_default_gateway(AF_INET, &gateway)) {
		PLUM_LOG_ERROR("Unable to get the default gateway address");
		return PROTOCOL_ERR_NETWORK_FAILED;
	}

	addr_set_port((struct sockaddr *)&gateway.addr, PCP_SERVER_PORT);

	if (found_gateway && found_gateway->len &&
	    !addr_record_is_equal(&gateway, found_gateway, false)) {
		PLUM_LOG_DEBUG("Default gateway changed");
		return PROTOCOL_ERR_RESET;
	}

	if (PLUM_LOG_DEBUG_ENABLED) {
		char gateway_str[ADDR_MAX_STRING_LEN];
		addr_record_to_string(&gateway, gateway_str, ADDR_MAX_STRING_LEN);
		PLUM_LOG_DEBUG("Probing gateway at %s", gateway_str);
	}

	char buffer[PCP_MAX_PAYLOAD_LENGTH];
	if (write_pcp_header((struct pcp_request_header *)buffer, PCP_OPCODE_ANNOUNCE, 0)) {
		PLUM_LOG_ERROR("Unable to write PCP header");
		return PROTOCOL_ERR_UNKNOWN;
	}

	PLUM_LOG_DEBUG("Sending PCP announce request");
	if (udp_sendto(impl->sock, buffer, sizeof(struct pcp_request_header), &gateway) < 0) {
		PLUM_LOG_ERROR("UDP send failed, errno=%d", sockerrno);
		return PROTOCOL_ERR_NETWORK_FAILED;
	}

	PLUM_LOG_DEBUG("Waiting for PCP announce response...");
	addr_record_t src;
	int len;
	while ((len = pcp_natpmp_impl_wait_response(impl, buffer, &src, end_timestamp, false)) >= 0) {
		if (len < (int)sizeof(struct pcp_common_header)) {
			PLUM_LOG_WARN("Datagram of length %d is too short, ignoring", len);
			continue;
		}
		const struct pcp_common_header *common_header = (const struct pcp_common_header *)buffer;
		if (common_header->opcode != (PCP_OPCODE_ANNOUNCE | PCP_OPCODE_RESPONSE_BIT)) {
			PLUM_LOG_DEBUG("Unexpected response opcode, ignoring");
			continue;
		}

		uint8_t result = common_header->result;
		uint8_t version = common_header->version;
		if (result != PCP_RESULT_SUCCESS) {
			if (result == PCP_RESULT_UNSUPP_VERSION) {
				if (version == 0) {
					// RFC 6887 9. Version Negotiation:
					// If the version number in the UNSUPP_VERSION response is zero then that means
					// this is a NAT-PMP server [RFC6886], and a client MAY choose to communicate
					// with it using the older NAT-PMP protocol
					PLUM_LOG_DEBUG("PCP is not supported, server is NAT-PMP only");
					return PROTOCOL_ERR_NEED_DOWNGRADE;
				} else {
					PLUM_LOG_WARN("PCP server version is unsupported, version=%u",
					              (unsigned int)version);
					return PROTOCOL_ERR_UNSUPP_VERSION;
				}
			} else {
				PLUM_LOG_WARN("Got PCP error response, result=%u", (unsigned int)result);
				return PROTOCOL_ERR_PROTOCOL_FAILED;
			}
		}

		if (len < (int)sizeof(struct pcp_response_header)) {
			PLUM_LOG_WARN("Announce response of length %d is too short", len);
			continue;
		}

		const struct pcp_response_header *header = (const struct pcp_response_header *)buffer;
		uint32_t epoch_time = ntohl(header->epoch_time);
		int err = pcp_impl_check_epoch_time(impl, epoch_time);
		if (err == PROTOCOL_ERR_RESET)
			return err;

		PLUM_LOG_DEBUG("Success probing PCP compatible gateway");
		if (found_gateway)
			*found_gateway = gateway;

		return PROTOCOL_ERR_SUCCESS;
	}

	return len; // len < 0
}

int pcp_impl_map(pcp_impl_t *impl, const client_mapping_t *mapping, protocol_map_output_t *output,
                 uint32_t lifetime, const addr_record_t *gateway, timestamp_t end_timestamp) {
	memset(output, 0, sizeof(*output));

	char buffer[PCP_MAX_PAYLOAD_LENGTH];
	if (write_pcp_header((struct pcp_request_header *)buffer, PCP_OPCODE_MAP, lifetime)) {
		PLUM_LOG_ERROR("Unable to write PCP header");
		return PROTOCOL_ERR_UNKNOWN;
	}

	char nonce[PCP_MAP_NONCE_SIZE];
	if (mapping->impl_record) {
		PLUM_LOG_VERBOSE("Using saved nonce for PCP map request");
		memcpy(nonce, mapping->impl_record, PCP_MAP_NONCE_SIZE);
	} else {
		PLUM_LOG_VERBOSE("Generating new nonce for PCP map request");
		plum_random(nonce, PCP_MAP_NONCE_SIZE);
	}

	pcp_protocol_t protocol;
	switch (mapping->protocol) {
	case PLUM_IP_PROTOCOL_TCP:
		protocol = PCP_PROTOCOL_TCP;
		break;
	case PLUM_IP_PROTOCOL_UDP:
		protocol = PCP_PROTOCOL_UDP;
		break;
	default:
		protocol = PCP_PROTOCOL_ALL;
		break;
	}

	struct pcp_map_request *map =
	    (struct pcp_map_request *)(buffer + sizeof(struct pcp_request_header));
	memset(map, 0, sizeof(*map));
	memcpy(map->nonce, nonce, PCP_MAP_NONCE_SIZE);
	map->protocol = protocol;
	map->internal_port = htons(mapping->internal_port);

	addr_record_t external =
	    mapping->external_addr.len > 0 ? mapping->external_addr : mapping->suggested_addr;

	// RFC 6887: If the PCP client does not know the external address, or does not have a
	// preference, it MUST use the address-family-specific all-zeros address
	if (external.len == 0 || lifetime == 0)
		addr_set(AF_INET, "0.0.0.0", 0, &external);

	// RFC 6887: When the address field holds an IPv4 address, an IPv4-mapped IPv6 address [RFC4291]
	// is used.
	addr_map_inet6_v4mapped(&external.addr, &external.len);
	struct sockaddr_in6 *external_sin6 = (struct sockaddr_in6 *)&external.addr;
	map->suggested_external_port = external_sin6->sin6_port; // network byte-order
	memcpy(map->suggested_external_addr, &external_sin6->sin6_addr, 16);

	PLUM_LOG_DEBUG("Sending PCP map request");
	if (udp_sendto(impl->sock, buffer,
	               sizeof(struct pcp_request_header) + sizeof(struct pcp_map_request),
	               gateway) < 0) {
		PLUM_LOG_ERROR("UDP send failed, errno=%d", sockerrno);
		return PROTOCOL_ERR_NETWORK_FAILED;
	}

	PLUM_LOG_DEBUG("Waiting for PCP map response...");
	addr_record_t src;
	int len;
	while ((len = pcp_natpmp_impl_wait_response(impl, buffer, &src, end_timestamp, false)) >= 0) {
		if (len < (int)sizeof(struct pcp_response_header)) {
			PLUM_LOG_WARN("Mapping response of length %d is too short", len);
			continue;
		}
		const struct pcp_response_header *header = (const struct pcp_response_header *)buffer;
		if (header->opcode != (PCP_OPCODE_MAP | PCP_OPCODE_RESPONSE_BIT)) {
			PLUM_LOG_DEBUG("Unexpected response opcode, ignoring");
			continue;
		}

		if (len < (int)(sizeof(struct pcp_response_header) + sizeof(struct pcp_map_response))) {
			PLUM_LOG_WARN("Mapping success response of length=%d is too short", len);
			continue;
		}

		const struct pcp_map_response *map =
		    (const struct pcp_map_response *)(buffer + sizeof(struct pcp_response_header));
		if (memcmp(map->nonce, nonce, PCP_MAP_NONCE_SIZE) != 0) {
			PLUM_LOG_DEBUG("Unexpected nonce in map response, ignoring");
			continue;
		}

		uint32_t epoch_time = ntohl(header->epoch_time);
		int err = pcp_impl_check_epoch_time(impl, epoch_time);
		if (err == PROTOCOL_ERR_RESET)
			return err;

		if (header->result != PCP_RESULT_SUCCESS) {
			PLUM_LOG_WARN("Got PCP error response, result=%d", (int)header->result);
			return PROTOCOL_ERR_PROTOCOL_FAILED;
		}

		uint32_t response_lifetime = ntohl(header->lifetime);
		PLUM_LOG_VERBOSE("Server mapping lifetime is %us", (unsigned int)response_lifetime);

		if (lifetime > response_lifetime)
			lifetime = response_lifetime;

		output->state = PROTOCOL_MAP_STATE_SUCCESS;

		// RFC 6887: The PCP client SHOULD renew the mapping before its expiry time; otherwise, it
		// will be removed by the PCP server. To reduce the risk of inadvertent synchronization of
		// renewal requests, a random jitter component should be included.
		if (response_lifetime > 0) {
			timediff_t expiry_delay = (timediff_t)lifetime * 1000;
			timediff_t refresh_delay = expiry_delay / 2 + plum_rand32() % (expiry_delay / 4);
			PLUM_LOG_VERBOSE("Renewing mapping in %us", (unsigned int)(refresh_delay / 1000));
			output->refresh_timestamp = current_timestamp() + refresh_delay;
		} else {
			output->refresh_timestamp = 0;
		}

		struct sockaddr_in6 *external_sin6 = (struct sockaddr_in6 *)&impl->external_addr.addr;
		memset(external_sin6, 0, sizeof(*external_sin6));
		impl->external_addr.len = sizeof(*external_sin6);
		external_sin6->sin6_family = AF_INET6;
		external_sin6->sin6_port = map->external_port; // network byte-order
		memcpy(&external_sin6->sin6_addr, map->external_addr, 16);
		addr_unmap_inet6_v4mapped((struct sockaddr *)&impl->external_addr.addr,
		                          &impl->external_addr.len);
		output->external_addr = impl->external_addr;

		if (lifetime != 0) {
			output->impl_record = malloc(PCP_MAP_NONCE_SIZE);
			if (!output->impl_record) {
				PLUM_LOG_ERROR("Allocation for nonce record failed");
				return PROTOCOL_ERR_INSUFF_RESOURCES;
			}
			memcpy(output->impl_record, nonce, PCP_MAP_NONCE_SIZE);
		}

		return PROTOCOL_ERR_SUCCESS;
	}

	return len; // len < 0
}

int pcp_impl_process_mcast_response(pcp_impl_t *impl, const char *buffer, int len) {
	if (len < (int)sizeof(struct pcp_response_header)) {
		PLUM_LOG_WARN("Datagram of length %d is too short, ignoring", len);
		return PROTOCOL_ERR_PROTOCOL_FAILED;
	}

	const struct pcp_response_header *header = (const struct pcp_response_header *)buffer;
	if (header->opcode != (PCP_OPCODE_ANNOUNCE | PCP_OPCODE_RESPONSE_BIT)) {
		PLUM_LOG_DEBUG("Unexpected opcode received multicast socket, ignoring");
		return PROTOCOL_ERR_SUCCESS;
	}

	uint32_t epoch_time = ntohl(header->epoch_time);
	int err = pcp_impl_check_epoch_time(impl, epoch_time);
	if (err == PROTOCOL_ERR_RESET)
		return err;

	return PROTOCOL_ERR_SUCCESS;
}

int pcp_impl_check_epoch_time(pcp_impl_t *impl, uint32_t curr_server_time) {
	PLUM_LOG_VERBOSE("PCP server epoch time is %u", (unsigned int)curr_server_time);
	uint32_t curr_client_time = (uint32_t)(current_timestamp() / 1000); // seconds

	// RFC 6887: Whenever a client receives a PCP response, the client validates the received Epoch
	// Time value according to the procedure below, using integer arithmetic:
	bool is_valid;

	// ... If this is the first PCP response the client has received from this PCP server, the Epoch
	// Time value is treated as necessarily valid
	if (!impl->has_prev_server_time) {
		is_valid = true;
	}
	// ... If the current PCP server Epoch time (curr_server_time) is less than the previously
	// received PCP server Epoch time (prev_server_time) by more than one second, then the
	// client treats the Epoch time as obviously invalid (time should not go backwards).
	else if (curr_server_time < impl->prev_server_time - 1) {
		is_valid = false;
	}
	// If the server Epoch time passes this check, then further validation checks are performed:
	else {
		// ... The client computes the difference between its current local time (curr_client_time)
		// and the time the previous PCP response was received from this PCP server
		// (prev_client_time)
		uint32_t client_delta = curr_client_time - impl->prev_client_time;

		// ... The client computes the difference between the current PCP server Epoch time
		// (curr_server_time) and the previously received Epoch time (prev_server_time)
		uint32_t server_delta = curr_server_time - impl->prev_server_time;

		// ... If client_delta+2 < server_delta - server_delta/16 or server_delta+2 < client_delta -
		// client_delta/16, then the client treats the Epoch Time value as invalid, else the client
		// treats the Epoch Time value as valid.
		is_valid = !(client_delta + 2 < server_delta - server_delta / 16 ||
		             server_delta + 2 < client_delta - client_delta / 16);
	}

	// ... The client records the current time values for use in its next comparison
	impl->prev_client_time = curr_client_time;
	impl->prev_server_time = curr_server_time;

	if (!is_valid) {
		PLUM_LOG_INFO("PCP reset detected");
		return PROTOCOL_ERR_RESET;
	}

	return PROTOCOL_ERR_SUCCESS;
}

static int process_mcast_response(pcp_impl_t *impl, const char *buffer, int len) {
	if (len < (int)sizeof(struct pcp_common_header)) {
		PLUM_LOG_WARN("Datagram of length %d is too short, ignoring", len);
		return PROTOCOL_ERR_PROTOCOL_FAILED;
	}
	const struct pcp_common_header *common_header = (const struct pcp_common_header *)buffer;
	uint8_t opcode = common_header->opcode;
	if (opcode != (PCP_OPCODE_ANNOUNCE | PCP_OPCODE_RESPONSE_BIT)) {
		PLUM_LOG_DEBUG("Unexpected response opcode, ignoring");
		return PROTOCOL_ERR_PROTOCOL_FAILED;
	}

	uint8_t version = common_header->version;
	if (!impl->use_natpmp && version == PCP_VERSION)
		return pcp_impl_process_mcast_response(impl, buffer, len);
	if (impl->use_natpmp && version == NATPMP_VERSION)
		return natpmp_impl_process_mcast_response(impl, buffer, len);
	else
		return PROTOCOL_ERR_UNSUPP_VERSION;
}

int pcp_natpmp_impl_wait_response(pcp_impl_t *impl, char *buffer, addr_record_t *src,
                                  timestamp_t end_timestamp, bool interruptible) {
	bool is_reset = false;
	timediff_t timediff;
	while ((timediff = end_timestamp - current_timestamp()) > 0) {

		pcp_interrupt_t interrupt = atomic_load(&impl->interrupt);
		if (interrupt == PCP_INTERRUPT_HARD || (interrupt == PCP_INTERRUPT_SOFT && interruptible)) {
			PLUM_LOG_VERBOSE("PCP/NAT-PMP interrupted");
			atomic_store(&impl->interrupt, PCP_INTERRUPT_NONE);
			return PROTOCOL_ERR_INTERRUPTED;
		}

		struct pollfd pfd[2];
		pfd[0].fd = impl->mcast_sock;
		pfd[0].events = POLLIN;
		pfd[1].fd = impl->sock;
		pfd[1].events = POLLIN;

		PLUM_LOG_VERBOSE("Entering poll for %d ms", (int)timediff);
		int ret = poll(pfd, 2, (int)timediff);
		if (ret < 0) {
			if (sockerrno == SEINTR || sockerrno == SEAGAIN) {
				PLUM_LOG_VERBOSE("poll interrupted");
				continue;
			} else {
				PLUM_LOG_ERROR("poll failed, errno=%d", sockerrno);
				return PROTOCOL_ERR_UNKNOWN;
			}
		}

		PLUM_LOG_VERBOSE("Exiting poll");

		if (ret == 0) // timeout
			break;

		for (int i = 0; i < 2; ++i) {
			if (pfd[i].revents & POLLNVAL || pfd[i].revents & POLLERR) {
				PLUM_LOG_ERROR("Error when polling socket");
				return PROTOCOL_ERR_UNKNOWN;
			}

			if (pfd[i].revents & POLLIN) {
				int len = udp_recvfrom(pfd[i].fd, buffer, PCP_MAX_PAYLOAD_LENGTH, src);
				if (len < 0) {
					if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK)
						continue;

					PLUM_LOG_WARN("UDP recvfrom failed, errno=%d", sockerrno);
					if (sockerrno == SECONNRESET) // ICMP Port Unreachable
						return PROTOCOL_ERR_UNSUPP_PROTOCOL;
					else
						return PROTOCOL_ERR_NETWORK_FAILED;
				}

				if (i == 0) {                                            // mcast_sock
					int err = process_mcast_response(impl, buffer, len); // handles PCP and NAT-PMP
					if (err == PROTOCOL_ERR_RESET) {
						// RFC 6887: [The client] MUST wait a random amount of time between 0 and 5
						// seconds
						unsigned int wait_duration = plum_rand32() % 5000;
						PLUM_LOG_VERBOSE("PCP/NAT-PMP reset, waiting %ums...", wait_duration);
						end_timestamp = current_timestamp() + wait_duration;
						is_reset = true;
					}
				} else {           // sock
					if (len > 0 && // 0-length datagrams are used to interrupt, ignore them
					    !is_reset)
						return len;
				}
			}
		}
	}

	if (is_reset)
		return PROTOCOL_ERR_RESET;

	return PROTOCOL_ERR_TIMEOUT;
}
