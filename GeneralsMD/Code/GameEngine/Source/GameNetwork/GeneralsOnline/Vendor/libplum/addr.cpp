/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "addr.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

socklen_t addr_get_len(const struct sockaddr *sa) {
	switch (sa->sa_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	default:
		PLUM_LOG_WARN("Unknown address family %hu", sa->sa_family);
		return 0;
	}
}

int addr_get_host(const struct sockaddr *sa, char *buffer, size_t size) {
	switch (sa->sa_family) {
	case AF_INET: {
		char tmp[INET_ADDRSTRLEN];
		if (!inet_ntop(AF_INET, &((struct sockaddr_in *)sa)->sin_addr, tmp, INET_ADDRSTRLEN)) {
			PLUM_LOG_ERROR("inet_ntop failed, errno=%d", sockerrno);
			goto error;
		}
		return snprintf(buffer, size, "%s", tmp);
	}
	case AF_INET6: {
		char tmp[INET6_ADDRSTRLEN];
		if (!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)sa)->sin6_addr, tmp, INET6_ADDRSTRLEN)) {
			PLUM_LOG_ERROR("inet_ntop failed, errno=%d", sockerrno);
			goto error;
		}
		return snprintf(buffer, size, "%s", tmp);
	}
	default:
		PLUM_LOG_WARN("Unknown address family %hu", sa->sa_family);
		goto error;
	}

error:
	// Make sure we still write a valid null-terminated string
	snprintf(buffer, size, "?");
	return -1;
}

uint16_t addr_get_port(const struct sockaddr *sa) {
	switch (sa->sa_family) {
	case AF_INET:
		return ntohs(((struct sockaddr_in *)sa)->sin_port);
	case AF_INET6:
		return ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
	default:
		PLUM_LOG_WARN("Unknown address family %hu", sa->sa_family);
		return 0;
	}
}

int addr_set_port(struct sockaddr *sa, uint16_t port) {
	switch (sa->sa_family) {
	case AF_INET:
		((struct sockaddr_in *)sa)->sin_port = htons(port);
		return 0;
	case AF_INET6:
		((struct sockaddr_in6 *)sa)->sin6_port = htons(port);
		return 0;
	default:
		PLUM_LOG_WARN("Unknown address family %hu", sa->sa_family);
		return -1;
	}
}

bool addr_is_any(const struct sockaddr *sa) {
	switch (sa->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
		const uint8_t *b = (const uint8_t *)&sin->sin_addr;
		for (int i = 0; i < 4; ++i)
			if (b[i] != 0)
				return false;

		return true;
	}
	case AF_INET6: {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			const uint8_t *b = (const uint8_t *)&sin6->sin6_addr + 12;
			for (int i = 0; i < 4; ++i)
				if (b[i] != 0)
					return false;
		} else {
			const uint8_t *b = (const uint8_t *)&sin6->sin6_addr;
			for (int i = 0; i < 16; ++i)
				if (b[i] != 0)
					return false;
		}
		return true;
	}
	default:
		return false;
	}
}

bool addr_is_local(const struct sockaddr *sa) {
	switch (sa->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
		const uint8_t *b = (const uint8_t *)&sin->sin_addr;
		if (b[0] == 127) // loopback
			return true;
		if (b[0] == 169 && b[1] == 254) // link-local
			return true;
		return false;
	}
	case AF_INET6: {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
		if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) {
			return true;
		}
		if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
			return true;
		}
		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			const uint8_t *b = (const uint8_t *)&sin6->sin6_addr + 12;
			if (b[0] == 127) // loopback
				return true;
			if (b[0] == 169 && b[1] == 254) // link-local
				return true;
			return false;
		}
		return false;
	}
	default:
		return false;
	}
}

bool addr_is_private(const struct sockaddr *sa) {
	switch (sa->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
		const uint8_t *b = (const uint8_t *)&sin->sin_addr;
		// RFC 1918 Private Addresses
		if (b[0] == 10) // 10.0.0.0/8
			return true;
		if (b[0] == 172 && (b[1] & 0xF0) == 16) // 172.16.0.0/12
			return true;
		if (b[0] == 192 && b[1] == 168) // 192.168.0.0/16
			return true;
		return false;
	}
	case AF_INET6: {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
		const uint8_t *b = (const uint8_t *)&sin6->sin6_addr;
		// RFC 4193 Unique Local Addresses
		if((b[0] & 0xFE) == 0xFC) // fc00::/7
			return true;
		return false;
	}
	default:
		return false;
	}
}

bool addr_is_public(const struct sockaddr *sa) {
	return !addr_is_private(sa) && !addr_is_local(sa);
}

bool addr_is_temp_inet6(const struct sockaddr *sa) {
	if (sa->sa_family != AF_INET6)
		return false;
	if (addr_is_local(sa))
		return false;
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
	const uint8_t *b = (const uint8_t *)&sin6->sin6_addr;
	return (b[8] & 0x02) ? false : true;
}

bool addr_unmap_inet6_v4mapped(struct sockaddr *sa, socklen_t *len) {
	if (sa->sa_family != AF_INET6)
		return false;

	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
	if (!IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
		return false;

	struct sockaddr_in6 copy = *sin6;
	sin6 = &copy;

	struct sockaddr_in *sin = (struct sockaddr_in *)sa;
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = sin6->sin6_port;
	memcpy(&sin->sin_addr, ((const uint8_t *)&sin6->sin6_addr) + 12, 4);
	*len = sizeof(*sin);
	return true;
}

bool addr_map_inet6_v4mapped(struct sockaddr_storage *ss, socklen_t *len) {
	if (ss->ss_family != AF_INET)
		return false;

	const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
	struct sockaddr_in copy = *sin;
	sin = &copy;

	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;
	memset(sin6, 0, sizeof(*sin6));
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = sin->sin_port;
	uint8_t *b = (uint8_t *)&sin6->sin6_addr;
	memset(b, 0, 10);
	memset(b + 10, 0xFF, 2);
	memcpy(b + 12, (const uint8_t *)&sin->sin_addr, 4);
	*len = sizeof(*sin6);
	return true;
}

bool addr_is_equal(const struct sockaddr *a, const struct sockaddr *b, bool compare_ports) {
	if (a->sa_family != b->sa_family)
		return false;

	switch (a->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *ain = (const struct sockaddr_in *)a;
		const struct sockaddr_in *bin = (const struct sockaddr_in *)b;
		if (memcmp(&ain->sin_addr, &bin->sin_addr, 4) != 0)
			return false;
		if (compare_ports && ain->sin_port != bin->sin_port)
			return false;
		break;
	}
	case AF_INET6: {
		const struct sockaddr_in6 *ain6 = (const struct sockaddr_in6 *)a;
		const struct sockaddr_in6 *bin6 = (const struct sockaddr_in6 *)b;
		if (memcmp(&ain6->sin6_addr, &bin6->sin6_addr, 16) != 0)
			return false;
		if (compare_ports && ain6->sin6_port != bin6->sin6_port)
			return false;
		break;
	}
	default:
		return false;
	}

	return true;
}

int addr_to_string(const struct sockaddr *sa, char *buffer, size_t size) {
	socklen_t salen = addr_get_len(sa);
	if (salen == 0)
		goto error;

	char host[ADDR_MAX_NUMERICHOST_LEN];
	char service[ADDR_MAX_NUMERICSERV_LEN];
	if (getnameinfo(sa, salen, host, ADDR_MAX_NUMERICHOST_LEN, service, ADDR_MAX_NUMERICSERV_LEN,
	                NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM)) {
		PLUM_LOG_ERROR("getnameinfo failed, errno=%d", sockerrno);
		goto error;
	}

	int len;
	if (strcmp(service, "0") == 0)
		len = snprintf(buffer, size, "%s", host);
	else
		len = snprintf(buffer, size, "%s:%s", host, service);

	if (len < 0 || (size_t)len >= size)
		goto error;

	return len;

error:
	// Make sure we still write a valid null-terminated string
	snprintf(buffer, size, "?");
	return -1;
}

int addr_set(int family, const char *host, uint16_t port, addr_record_t *record) {
	switch (family) {
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)&record->addr;
		memset(sin, 0, sizeof(*sin));
		record->len = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		if (inet_pton(AF_INET, host, &sin->sin_addr) != 1) {
			PLUM_LOG_WARN("Invalid IPv4 address: %s", host);
			return -1;
		}
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&record->addr;
		memset(sin6, 0, sizeof(*sin6));
		record->len = sizeof(*sin6);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(port);
		if (inet_pton(AF_INET6, host, &sin6->sin6_addr) != 1) {
			PLUM_LOG_WARN("Invalid IPv6 address: %s", host);
			return -1;
		}
		break;
	}
	case AF_UNSPEC: {
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
		char service[8];
		if (snprintf(service, 8, "%hu", port) < 0)
			return -1;

		struct addrinfo *ai_list = NULL;
		if (getaddrinfo(host, service, &hints, &ai_list)) {
			PLUM_LOG_WARN("Invalid address: %s", host);
			return -1;
		}

		struct addrinfo *ai;
		for (ai = ai_list; ai; ai = ai->ai_next)
			if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6)
				break;

		if (!ai) {
			PLUM_LOG_WARN("Not an IPv4 or IPv6 address: %s", host);
			freeaddrinfo(ai_list);
			return -1;
		}

		memcpy(&record->addr, ai->ai_addr, ai->ai_addrlen);
		record->len = (socklen_t)ai->ai_addrlen;
		freeaddrinfo(ai_list);
		break;
	}
	default:
		PLUM_LOG_WARN("Unknown address family %d", family);
		return -1;
	}

	return 0;
}

int addr_set_binary(int family, const void *addr, uint16_t port, addr_record_t *record) {
	switch (family) {
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)&record->addr;
		memset(sin, 0, sizeof(*sin));
		record->len = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		memcpy(&sin->sin_addr, addr, 4);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&record->addr;
		memset(sin6, 0, sizeof(*sin6));
		record->len = sizeof(*sin6);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(port);
		memcpy(&sin6->sin6_addr, addr, 16);
		break;
	}
	case AF_UNSPEC: {
		PLUM_LOG_ERROR("addr_set_binary called with unspecified address family");
		return -1;
	}
	default:
		PLUM_LOG_WARN("Unknown address family %d", family);
		return -1;
	}

	return 0;
}

int addr_resolve(const char *hostname, const char *service, addr_record_t *records, size_t count) {
	addr_record_t *end = records + count;

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_ADDRCONFIG;
	struct addrinfo *ai_list = NULL;
	if (getaddrinfo(hostname, service, &hints, &ai_list)) {
		PLUM_LOG_WARN("Address resolution failed for %s:%s", hostname, service);
		return -1;
	}

	int ret = 0;
	for (struct addrinfo *ai = ai_list; ai; ai = ai->ai_next) {
		if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
			++ret;
			if (records != end) {
				memcpy(&records->addr, ai->ai_addr, ai->ai_addrlen);
				records->len = (socklen_t)ai->ai_addrlen;
				++records;
			}
		}
	}

	freeaddrinfo(ai_list);
	return ret;
}

bool addr_record_is_equal(const addr_record_t *a, const addr_record_t *b, bool compare_ports) {
	return addr_is_equal((const struct sockaddr *)&a->addr, (const struct sockaddr *)&b->addr,
	                     compare_ports);
}

int addr_record_to_string(const addr_record_t *record, char *buffer, size_t size) {
	return addr_to_string((const struct sockaddr *)&record->addr, buffer, size);
}
