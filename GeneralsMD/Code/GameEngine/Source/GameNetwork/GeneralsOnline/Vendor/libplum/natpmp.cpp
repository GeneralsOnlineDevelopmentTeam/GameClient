/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "libplum/natpmp.h"
#include "log.h"
#include "net.h"
#include "pcp.h"
#include "udp.h"

#include <stdlib.h>
#include <string.h>

int natpmp_impl_probe(pcp_impl_t *impl, addr_record_t *found_gateway, timestamp_t end_timestamp) {
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

	PLUM_LOG_DEBUG("Sending NAT-PMP announce request");
	struct natpmp_announce_request request;
	memset(&request, 0, sizeof(request));
	request.version = NATPMP_VERSION;
	request.opcode = NATPMP_OPCODE_ANNOUNCE;
	if (udp_sendto(impl->sock, (const char *)&request, sizeof(request), &gateway) < 0) {
		PLUM_LOG_ERROR("UDP send failed, errno=%d", sockerrno);
		return PROTOCOL_ERR_NETWORK_FAILED;
	}

	PLUM_LOG_DEBUG("Waiting for NAT-PMP announce response...");
	char buffer[PCP_MAX_PAYLOAD_LENGTH];
	addr_record_t src;
	int len;
	while ((len = pcp_natpmp_impl_wait_response(impl, buffer, &src, end_timestamp, false)) >= 0) {
		if (len < (int)sizeof(struct natpmp_announce_response)) {
			PLUM_LOG_WARN("Announce response of length %d is too short", len);
			continue;
		}
		const struct natpmp_announce_response *response =
		    (const struct natpmp_announce_response *)buffer;
		if (response->opcode != (NATPMP_OPCODE_ANNOUNCE | NATPMP_OPCODE_RESPONSE_BIT)) {
			PLUM_LOG_DEBUG("Unexpected response opcode, ignoring");
			continue;
		}

		uint32_t epoch_time = ntohl(response->epoch_time);
		int err = natpmp_impl_check_epoch_time(impl, epoch_time);
		if (err == PROTOCOL_ERR_RESET)
			return err;

		uint16_t result = ntohs(response->result);
		if (result != NATPMP_RESULT_SUCCESS) {
			PLUM_LOG_WARN("Got NAT-PMP error response, result=%d", (int)result);
			return PROTOCOL_ERR_PROTOCOL_FAILED;
		}

		addr_set_binary(AF_INET, response->external_addr, 0, &impl->external_addr);

		if (PLUM_LOG_DEBUG_ENABLED) {
			char external_str[ADDR_MAX_STRING_LEN];
			addr_record_to_string(&impl->external_addr, external_str, ADDR_MAX_STRING_LEN);
			PLUM_LOG_DEBUG("Success probing NAT-PMP compatible gateway, external address is %s",
			               external_str);
		}

		if (found_gateway)
			*found_gateway = gateway;

		return PROTOCOL_ERR_SUCCESS;
	}

	return len; // len < 0
}

int natpmp_impl_map(pcp_impl_t *impl, const client_mapping_t *mapping,
                    protocol_map_output_t *output, uint32_t lifetime, const addr_record_t *gateway,
                    timestamp_t end_timestamp) {
	memset(output, 0, sizeof(*output));

	if (impl->external_addr.len == 0) {
		PLUM_LOG_WARN("Attempted to map with NAT-PMP while external address is unknown");
		return PROTOCOL_ERR_RESET;
	}

	if (mapping->protocol != PLUM_IP_PROTOCOL_TCP && mapping->protocol != PLUM_IP_PROTOCOL_UDP)
		return PROTOCOL_ERR_UNSUPP_PROTOCOL;

	struct natpmp_map_request request;
	memset(&request, 0, sizeof(request));
	request.version = NATPMP_VERSION;
	request.opcode =
	    mapping->protocol == PLUM_IP_PROTOCOL_UDP ? NATPMP_OPCODE_MAP_UDP : NATPMP_OPCODE_MAP_TCP;
	request.internal_port = htons(mapping->internal_port);
	request.lifetime = htonl(lifetime);

	uint16_t external_port = mapping->external_addr.len > 0
	                             ? addr_get_port((const struct sockaddr *)&mapping->external_addr)
	                             : 0;
	if (external_port == 0)
		external_port = mapping->suggested_addr.len > 0
		                    ? addr_get_port((const struct sockaddr *)&mapping->suggested_addr)
		                    : 0;

	request.suggested_external_port = htons(external_port);

	PLUM_LOG_DEBUG("Sending map request");
	if (udp_sendto(impl->sock, (const char *)&request, sizeof(request), gateway) < 0) {
		PLUM_LOG_ERROR("UDP send failed, errno=%d", sockerrno);
		return -1;
	}

	PLUM_LOG_DEBUG("Waiting for map response...");
	char buffer[PCP_MAX_PAYLOAD_LENGTH];
	addr_record_t src;
	int len;
	while ((len = pcp_natpmp_impl_wait_response(impl, buffer, &src, end_timestamp, false)) >= 0) {
		if (len < (int)sizeof(struct natpmp_map_response)) {
			PLUM_LOG_WARN("Mapping response of length %d is too short", len);
			continue;
		}
		const struct natpmp_map_response *response = (const struct natpmp_map_response *)buffer;
		if (response->opcode != (request.opcode | PCP_OPCODE_RESPONSE_BIT)) {
			PLUM_LOG_DEBUG("Unexpected response opcode, ignoring");
			continue;
		}

		uint32_t epoch_time = ntohl(response->epoch_time);
		int err = natpmp_impl_check_epoch_time(impl, epoch_time);
		if (err == PROTOCOL_ERR_RESET)
			return err;

		uint16_t result = ntohs(response->result);
		if (result != NATPMP_RESULT_SUCCESS) {
			PLUM_LOG_WARN("Got NAT-PMP error response, result=%d", (int)response->result);
			return PROTOCOL_ERR_PROTOCOL_FAILED;
		}

		uint32_t response_lifetime = ntohl(response->lifetime);
		PLUM_LOG_VERBOSE("Server mapping lifetime is %us", (unsigned int)response_lifetime);

		if (lifetime > response_lifetime)
			lifetime = response_lifetime;

		output->state = PROTOCOL_MAP_STATE_SUCCESS;

		// RFC 6886: The client SHOULD begin trying to renew the mapping halfway to expiry time,
		// like DHCP.
		if (response_lifetime > 0) {
			timediff_t expiry_delay = (timediff_t)lifetime * 1000;
			timediff_t refresh_delay = expiry_delay / 2;
			PLUM_LOG_VERBOSE("Renewing mapping in %us", (unsigned int)(refresh_delay / 1000));
			output->refresh_timestamp = current_timestamp() + refresh_delay;
		} else {
			output->refresh_timestamp = 0;
		}

		uint16_t external_port = ntohs(response->external_port);
		output->external_addr = impl->external_addr;
		addr_set_port((struct sockaddr *)&output->external_addr, external_port);
		return PROTOCOL_ERR_SUCCESS;
	}

	return len; // len < 0
}

int natpmp_impl_process_mcast_response(pcp_impl_t *impl, const char *buffer, int len) {
	if (len < (int)sizeof(struct natpmp_announce_response)) {
		PLUM_LOG_WARN("Datagram of length %d is too short, ignoring", len);
		return PROTOCOL_ERR_PROTOCOL_FAILED;
	}

	const struct natpmp_announce_response *response =
	    (const struct natpmp_announce_response *)buffer;
	if (response->opcode != (PCP_OPCODE_ANNOUNCE | PCP_OPCODE_RESPONSE_BIT)) {
		PLUM_LOG_DEBUG("Unexpected opcode received multicast socket, ignoring");
		return PROTOCOL_ERR_SUCCESS;
	}

	uint32_t epoch_time = ntohl(response->epoch_time);
	int err = natpmp_impl_check_epoch_time(impl, epoch_time);
	if (err == PROTOCOL_ERR_RESET)
		return err;

	uint16_t result = ntohs(response->result);
	if (result == NATPMP_RESULT_SUCCESS) {
		addr_set_binary(AF_INET, response->external_addr, 0, &impl->external_addr);

		if (PLUM_LOG_DEBUG_ENABLED) {
			char external_str[ADDR_MAX_STRING_LEN];
			addr_record_to_string(&impl->external_addr, external_str, ADDR_MAX_STRING_LEN);
			PLUM_LOG_DEBUG("Got announce from NAT-PMP compatible gateway, external address is %s",
			               external_str);
		}
	}

	return PROTOCOL_ERR_SUCCESS;
}

int natpmp_impl_check_epoch_time(pcp_impl_t *impl, uint32_t curr_server_time) {
	PLUM_LOG_VERBOSE("NAT-PMP server epoch time is %u", (unsigned int)curr_server_time);
	uint32_t curr_client_time = (uint32_t)(current_timestamp() / 1000); // seconds

	if (!impl->has_prev_server_time) {
		impl->has_prev_server_time = true;
		impl->prev_client_time = curr_client_time;
		impl->prev_server_time = curr_server_time;
		return PROTOCOL_ERR_SUCCESS;
	}

	// RFC 6886: Every packet sent by the NAT gateway includes a Seconds Since Start of Epoch
	// (SSSoE) field. [...] Whenever a client receives any packet from the NAT gateway, either
	// unsolicited or in response to a client request, the client computes its own conservative
	// estimate of the expected SSSoE value by taking the SSSoE value in the last packet it received
	// from the gateway and adding 7/8 (87.5%) of the time elapsed according to the client's local
	// clock since that packet was received.
	uint32_t elapsed = curr_client_time - impl->prev_client_time;
	uint32_t estimated_server_time = curr_server_time + elapsed - elapsed / 8;

	impl->prev_client_time = curr_client_time;
	impl->prev_server_time = curr_server_time;

	// If the SSSoE in the newly received packet is less than the client's conservative estimate by
	// more than 2 seconds, then the client concludes that the NAT gateway has undergone a reboot or
	// other loss of port mapping state, and the client MUST immediately renew all its active port
	// mapping leases
	if (curr_server_time + 2 < estimated_server_time) {
		PLUM_LOG_INFO("NAT-PMP reset detected");
		return PROTOCOL_ERR_RESET;
	}

	return PROTOCOL_ERR_SUCCESS;
}
