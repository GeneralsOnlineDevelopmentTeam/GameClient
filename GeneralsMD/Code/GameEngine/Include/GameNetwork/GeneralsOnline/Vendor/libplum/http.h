/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "timestamp.h"

#include <stdlib.h>

// Trivial implementation of an HTTP 1.0 client

#define HTTP_MAX_HOST_LEN 256
#define HTTP_MAX_URL_LEN 1024

#define HTTP_ERR_UNKNOWN -1
#define HTTP_ERR_TIMEOUT -2

typedef enum http_method {
	HTTP_METHOD_GET,
	HTTP_METHOD_POST,
} http_method_t;

typedef struct http_request {
	http_method_t method;
	const char *url;
	const char *headers;
	const char *body;
	const char *body_type;
	size_t body_size;
} http_request_t;

typedef struct http_response {
	char *headers;
	char *body;
	size_t body_size;
} http_response_t;

int http_perform(const http_request_t *request, http_response_t *response, timestamp_t end_timestamp);
void http_free(http_response_t *response);

