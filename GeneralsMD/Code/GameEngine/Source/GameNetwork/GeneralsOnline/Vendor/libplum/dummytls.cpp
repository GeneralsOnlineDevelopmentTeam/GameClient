/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dummytls.h"
#include "addr.h"
#include "http.h"
#include "log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HTTP_TIMEOUT 10 * 1000               // ms
#define CERTS_CACHE_TIME 24 * 60 * 60 * 1000 // ms, 1 day

typedef struct {
	const char *cert;
	const char *chain;
	const char *fullchain;
	const char *privkey;
	timestamp_t timestamp;
} dummytls_certs_t;

typedef struct {
	char domain[DUMMYTLS_MAX_DOMAIN_LEN];
	dummytls_certs_t certs;
	mutex_t mutex;
} dummytls_state_t;

static dummytls_state_t state;

static int resolve_host(const struct sockaddr *sa, const char *domain, char *buffer, size_t size) {
	char host[ADDR_MAX_STRING_LEN];
	if (addr_get_host(sa, host, ADDR_MAX_STRING_LEN) < 0) {
		mutex_unlock(&state.mutex);
		return -1;
	}

	char *p = host;
	while (*p != '\0') {
		if (*p == '.' || *p == ':')
			*p = '-';

		++p;
	}

	int len = snprintf(buffer, size, "%s.%s", host, domain);
	if (len < 0 || (size_t)len >= size)
		return -1;

	addr_record_t record;
	const char *dummy_service = "443";
	if (addr_resolve(buffer, dummy_service, &record, 1) < 0)
		return -1;

	if (!addr_is_equal((const struct sockaddr *)&record.addr, sa, false))
		return -1;

	return len;
}

static const char *fetch_file(const char *domain, const char *filename) {
	char url[DUMMYTLS_MAX_URL_LEN];
	int len = snprintf(url, DUMMYTLS_MAX_URL_LEN, "http://%s/%s", domain, filename);
	if (len < 0 || len >= DUMMYTLS_MAX_URL_LEN)
		return NULL;

	timestamp_t end_timestamp = current_timestamp() + HTTP_TIMEOUT;

	http_request_t request;
	memset(&request, 0, sizeof(request));
	request.method = HTTP_METHOD_GET;
	request.url = url;
	request.headers = "";

	http_response_t response;
	int ret = http_perform(&request, &response, end_timestamp);
	if (ret < 0) {
		PLUM_LOG_WARN("Failed to send HTTP request to DummyTLS service");
		return NULL;
	}

	if (ret != 200) {
		PLUM_LOG_WARN("HTTP request to DummyTLS service failed, code=%d", ret);
		http_free(&response);
		return NULL;
	}

	char *content = (char*)malloc(response.body_size + 1);
	if (!content) {
		PLUM_LOG_WARN("Failed to allocate memory for file, size=%zu", response.body_size);
		http_free(&response);
		return NULL;
	}

	memcpy(content, response.body, response.body_size);
	content[response.body_size] = '\0';
	http_free(&response);
	return content;
}

static void clear_certs(dummytls_certs_t *certs) {
	free((char *)certs->cert);
	free((char *)certs->chain);
	free((char *)certs->fullchain);
	free((char *)certs->privkey);
	memset(certs, 0, sizeof(*certs));
}

static void renew_certs(dummytls_certs_t *certs) {
	timestamp_t now = current_timestamp();
	if (state.certs.timestamp == 0 || now >= state.certs.timestamp + CERTS_CACHE_TIME) {
		PLUM_LOG_DEBUG("Fetching DummyTLS certificates");
		clear_certs(certs);
		state.certs.timestamp = now;
	}

	if (!certs->cert)
		certs->cert = fetch_file(state.domain, "cert.pem");

	if (!certs->chain)
		certs->chain = fetch_file(state.domain, "chain.pem");

	if (!certs->fullchain)
		certs->fullchain = fetch_file(state.domain, "fullchain.pem");

	if (!certs->privkey)
		certs->privkey = fetch_file(state.domain, "privkey.pem");
}

void dummytls_init() {
	memset(&state, 0, sizeof(state));
	mutex_init(&state.mutex, 0);
}

void dummytls_cleanup() {
	clear_certs(&state.certs);
	mutex_destroy(&state.mutex);
}

int dummytls_set_domain(const char *domain) {
	mutex_lock(&state.mutex);

	clear_certs(&state.certs);

	if (domain) {
		PLUM_LOG_DEBUG("Using DummyTLS domain: %s", domain);
		snprintf(state.domain, DUMMYTLS_MAX_DOMAIN_LEN, "%s", domain);
	} else {
		*state.domain = '\0';
	}

	mutex_unlock(&state.mutex);
	return 0;
}

int dummytls_get_host(const struct sockaddr *sa, char *buffer, size_t size) {
	mutex_lock(&state.mutex);

	if (*state.domain == '\0') {
		mutex_unlock(&state.mutex);
		return -1;
	}

	int len = resolve_host(sa, state.domain, buffer, size);
	if (len < 0) {
		PLUM_LOG_ERROR("Failed to resolve host with DummyTLS service");
		mutex_unlock(&state.mutex);
		return -1;
	}

	PLUM_LOG_DEBUG("Successfully resolved DummyTLS host: %s", buffer);

	mutex_unlock(&state.mutex);
	return len;
}

int dummytls_get_cert(plum_dummytls_cert_type_t type, char *buffer, size_t size) {
	mutex_lock(&state.mutex);

	if (*state.domain == '\0') {
		mutex_unlock(&state.mutex);
		return -1;
	}

	const char *cert;
	switch (type) {
	case PLUM_DUMMYTLS_PEM_CERT:
		cert = state.certs.cert;
		break;
	case PLUM_DUMMYTLS_PEM_CHAIN:
		cert = state.certs.chain;
		break;
	case PLUM_DUMMYTLS_PEM_FULLCHAIN:
		cert = state.certs.fullchain;
		break;
	case PLUM_DUMMYTLS_PEM_PRIVKEY:
		cert = state.certs.privkey;
		break;
	default:
		return -1;
	}

	if (!cert) {
		PLUM_LOG_ERROR("Failed to retrieve DummyTLS cert file");
		mutex_unlock(&state.mutex);
		return -1;
	}

	int len = snprintf(buffer, size, "%s", cert);

	mutex_unlock(&state.mutex);
	return len;
}

int dummytls_renew_certs(void) {
	mutex_lock(&state.mutex);

	if (*state.domain == '\0') {
		mutex_unlock(&state.mutex);
		return -1;
	}

	renew_certs(&state.certs);

	mutex_unlock(&state.mutex);
	return 0;
}
