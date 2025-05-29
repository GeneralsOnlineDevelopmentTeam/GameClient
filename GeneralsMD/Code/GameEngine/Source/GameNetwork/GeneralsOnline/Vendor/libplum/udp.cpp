/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "udp.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct addrinfo *find_family(struct addrinfo *ai_list, int family) {
	struct addrinfo *ai = ai_list;
	while (ai && ai->ai_family != family)
		ai = ai->ai_next;
	return ai;
}

socket_t udp_create_socket(const udp_socket_config_t *config) {
	socket_t sock = INVALID_SOCKET;

	if (config->family != AF_UNSPEC && config->family != AF_INET && config->family != AF_INET6) {
		PLUM_LOG_ERROR("Unknown address family for UDP socket");
		return INVALID_SOCKET;
	}

	// Obtain local Address
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = config->family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	char bind_service[8];
	if (snprintf(bind_service, 8, "%hu", config->port) < 0)
		return INVALID_SOCKET;

	struct addrinfo *ai_list = NULL;
	if (getaddrinfo(config->bind_address, bind_service, &hints, &ai_list) != 0) {
		PLUM_LOG_ERROR("getaddrinfo for binding address failed, errno=%d", sockerrno);
		return INVALID_SOCKET;
	}

	struct addrinfo *ai;
	if (config->family == AF_UNSPEC) {
		// Prefer IPv6
		if ((ai = find_family(ai_list, AF_INET6)) == NULL &&
		    (ai = find_family(ai_list, AF_INET)) == NULL) {
			PLUM_LOG_ERROR("getaddrinfo for binding address failed: no suitable "
			               "address family");
			freeaddrinfo(ai_list);
			if (sock != INVALID_SOCKET)
				closesocket(sock);

			return INVALID_SOCKET;
		}
	} else {
		ai = ai_list;
	}

	// Create socket
	sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (sock == INVALID_SOCKET) {
		PLUM_LOG_ERROR("UDP socket creation failed, errno=%d", sockerrno);
		freeaddrinfo(ai_list);
		if (sock != INVALID_SOCKET)
			closesocket(sock);

		return INVALID_SOCKET;
	}

	const sockopt_t enabled = 1;
	const sockopt_t disabled = 0;

	// Listen on both IPv6 and IPv4
	if (config->family == AF_UNSPEC && ai->ai_family == AF_INET6)
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&disabled, sizeof(disabled));

	if (config->enable_broadcast)
		setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)(&enabled), sizeof(enabled));

	if (config->enable_reuseaddr)
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled, sizeof(enabled));

	if (config->enable_dontfrag) {
		// Set DF flag
#ifndef NO_PMTUDISC
		const sockopt_t mtu_disc = IP_PMTUDISC_DO;
		setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, (const char *)&mtu_disc, sizeof(mtu_disc));
#ifdef IPV6_MTU_DISCOVER
		if (ai->ai_family == AF_INET6)
			setsockopt(sock, IPPROTO_IPV6, IPV6_MTU_DISCOVER, (const char *)&mtu_disc,
			           sizeof(mtu_disc));
#endif
#else
		// It seems Mac OS lacks a way to set the DF flag...
#ifdef IP_DONTFRAG
		setsockopt(sock, IPPROTO_IP, IP_DONTFRAG, (const char *)&enabled, sizeof(enabled));
#endif
#ifdef IPV6_DONTFRAG
		if (ai->ai_family == AF_INET6)
			setsockopt(sock, IPPROTO_IPV6, IPV6_DONTFRAG, (const char *)&enabled, sizeof(enabled));
#endif
#endif
	}

	if (config->multicast_group) {
		struct ip_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		if (inet_pton(AF_INET, config->multicast_group, &mreq.imr_multiaddr.s_addr) != 1) {
			PLUM_LOG_ERROR("Invalid multicast group");
			freeaddrinfo(ai_list);
			if (sock != INVALID_SOCKET)
				closesocket(sock);

			return INVALID_SOCKET;
		}

		if(ai->ai_addr->sa_family == AF_INET)
			mreq.imr_interface.s_addr = ((const struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
		else
			mreq.imr_interface.s_addr = htonl(INADDR_ANY);

		setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq));

		// TODO
		// struct ipv6_mreq mreq;
		// setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq))
	}

	ctl_t nbio = 1;
	if (ioctlsocket(sock, FIONBIO, &nbio)) {
		PLUM_LOG_ERROR("Setting non-blocking mode on UDP socket failed, errno=%d", sockerrno);
		freeaddrinfo(ai_list);
		if (sock != INVALID_SOCKET)
			closesocket(sock);

		return INVALID_SOCKET;
	}

	// Bind it
	if (bind(sock, ai->ai_addr, (socklen_t)ai->ai_addrlen)) {
		PLUM_LOG_ERROR("UDP socket binding failed, errno=%d", sockerrno);
		freeaddrinfo(ai_list);
		if (sock != INVALID_SOCKET)
			closesocket(sock);

		return INVALID_SOCKET;
	}

	freeaddrinfo(ai_list);
	return sock;
}

int udp_recvfrom(socket_t sock, char *buffer, size_t size, addr_record_t *src) {
	src->len = sizeof(src->addr);
	int len = recvfrom(sock, buffer, (socklen_t)size, 0, (struct sockaddr *)&src->addr, &src->len);
	if (len >= 0)
		addr_unmap_inet6_v4mapped((struct sockaddr *)&src->addr, &src->len);

	return len;
}

int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst) {
#ifndef __linux__
	addr_record_t tmp = *dst;
	addr_record_t name;
	name.len = sizeof(name.addr);
	if (getsockname(sock, (struct sockaddr *)&name.addr, &name.len) == 0) {
		if (name.addr.ss_family == AF_INET6)
			addr_map_inet6_v4mapped(&tmp.addr, &tmp.len);
	} else {
		PLUM_LOG_WARN("getsockname failed, errno=%d", sockerrno);
	}
	return sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&tmp.addr, tmp.len);
#else
	return sendto(sock, data, size, 0, (const struct sockaddr *)&dst->addr, dst->len);
#endif
}

int udp_sendto_self(socket_t sock, const char *data, size_t size) {
	addr_record_t local;
	if (udp_get_local_addr(sock, AF_UNSPEC, &local) < 0)
		return -1;

	int ret;
#ifndef __linux__
	// We know local has the same address family as sock here
	ret = sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&local.addr, local.len);
#else
	ret = sendto(sock, data, size, 0, (const struct sockaddr *)&local.addr, local.len);
#endif
	if (ret >= 0 || local.addr.ss_family != AF_INET6)
		return ret;

	// Fallback as IPv6 may be disabled on the loopback interface
	if (udp_get_local_addr(sock, AF_INET, &local) < 0)
		return -1;

#ifndef __linux__
	addr_map_inet6_v4mapped(&local.addr, &local.len);
	return sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&local.addr, local.len);
#else
	return sendto(sock, data, size, 0, (const struct sockaddr *)&local.addr, local.len);
#endif
}

int udp_set_diffserv(socket_t sock, int ds) {
#ifdef _WIN32
	// IP_TOS has been intentionally broken on Windows in favor of a convoluted proprietary
	// mechanism called qWave. Thank you Microsoft!
	// TODO: Investigate if DSCP can be still set directly without administrator flow configuration.
	(void)sock;
	(void)ds;
	PLUM_LOG_INFO("IP Differentiated Services are not supported on Windows");
	return -1;
#else
	addr_record_t name;
	name.len = sizeof(name.addr);
	if (getsockname(sock, (struct sockaddr *)&name.addr, &name.len) < 0) {
		PLUM_LOG_WARN("getsockname failed, errno=%d", sockerrno);
		return -1;
	}

	switch (name.addr.ss_family) {
	case AF_INET:
#ifdef IP_TOS
		if (setsockopt(sock, IPPROTO_IP, IP_TOS, &ds, sizeof(ds)) < 0) {
			PLUM_LOG_WARN("Setting IP ToS failed, errno=%d", sockerrno);
			return -1;
		}
		return 0;
#else
		PLUM_LOG_INFO("Setting IP ToS is not supported");
		return -1;
#endif

	case AF_INET6:
#ifdef IPV6_TCLASS
		if (setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, &ds, sizeof(ds)) < 0) {
			PLUM_LOG_WARN("Setting IPv6 traffic class failed, errno=%d", sockerrno);
			return -1;
		}
		return 0;
#else
		PLUM_LOG_INFO("Setting IPv6 traffic class is not supported");
		return -1;
#endif

	default:
		return -1;
	}
#endif
}

uint16_t udp_get_port(socket_t sock) {
	addr_record_t record;
	if (udp_get_bound_addr(sock, &record) < 0)
		return 0;
	return addr_get_port((struct sockaddr *)&record.addr);
}

int udp_get_bound_addr(socket_t sock, addr_record_t *record) {
	record->len = sizeof(record->addr);
	if (getsockname(sock, (struct sockaddr *)&record->addr, &record->len)) {
		PLUM_LOG_WARN("getsockname failed, errno=%d", sockerrno);
		return -1;
	}
	return 0;
}

int udp_get_local_addr(socket_t sock, int family_hint, addr_record_t *record) {
	if (udp_get_bound_addr(sock, record) < 0)
		return -1;

	// If the socket is bound to a particular address, return it
	if (!addr_is_any((struct sockaddr *)&record->addr)) {
		if (record->addr.ss_family == AF_INET && family_hint == AF_INET6)
			addr_map_inet6_v4mapped(&record->addr, &record->len);

		return 0;
	}

	if (record->addr.ss_family == AF_INET6 && family_hint == AF_INET) {
		// Generate an IPv4 instead (socket is listening to any IPv4 or IPv6)

		uint16_t port = addr_get_port((struct sockaddr *)&record->addr);
		if (port == 0)
			return -1;

		struct sockaddr_in *sin = (struct sockaddr_in *)&record->addr;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		record->len = sizeof(*sin);
	}

	switch (record->addr.ss_family) {
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)&record->addr;
		const uint8_t localhost[4] = {127, 0, 0, 1};
		memcpy(&sin->sin_addr, localhost, 4);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&record->addr;
		uint8_t *b = (uint8_t *)&sin6->sin6_addr;
		memset(b, 0, 15);
		b[15] = 0x01; // localhost
		break;
	}
	default:
		// Ignore
		break;
	}

	if (record->addr.ss_family == AF_INET && family_hint == AF_INET6)
		addr_map_inet6_v4mapped(&record->addr, &record->len);

	return 0;
}
