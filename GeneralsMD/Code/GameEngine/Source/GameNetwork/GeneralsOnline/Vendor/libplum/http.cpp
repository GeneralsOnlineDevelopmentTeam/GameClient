/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "http.h"
#include "log.h"
#include "tcp.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define HTTP_MAX_RECORDS 4
#define HTTP_MAX_REDIRECTIONS 5

#define DEFAULT_BUFFER_SIZE 10 * 1024
#define MAX_BUFFER_SIZE 10 * 1024 * 1024

static int http_perform_rec(const http_request_t *request, http_response_t *response,
                 timestamp_t end_timestamp, int redirections) {
	const char *scheme = "http://";
	const size_t scheme_len = strlen(scheme);
    int ret = HTTP_ERR_UNKNOWN;

	const char *url = request->url;
	const size_t url_len = strlen(url);
	if (strncmp(url, scheme, scheme_len) != 0) {
		PLUM_LOG_WARN("HTTP request URL is invalid: %s", request->url);
		return ret;
	}

	const char *path = strchr(url + scheme_len, '/');
	if (!path)
		path = url + url_len;

	char host[HTTP_MAX_HOST_LEN];
	int host_len = (int)(path - (url + scheme_len));
	if (snprintf(host, HTTP_MAX_HOST_LEN, "%.*s", host_len, url + scheme_len) != host_len) {
		PLUM_LOG_WARN("Failed to retrieve HTTP host from URL");
		return ret;
	}

	const char *service;
	char *separator = strchr(host, ':');
	if (separator) {
		*separator = '\0';
		service = separator + 1;
	} else {
		service = "80";
	}

	addr_record_t records[HTTP_MAX_RECORDS];
	int n = addr_resolve(host, service, records, HTTP_MAX_RECORDS);
	if (n <= 0) {
		PLUM_LOG_WARN("Failed to resolve HTTP host %s:%s", host, service);
		return ret;
	}
	if (n > HTTP_MAX_RECORDS)
		n = HTTP_MAX_RECORDS;

	socket_t sock = INVALID_SOCKET;
	for (int i = 0; i < n; ++i) {
		addr_record_t *record = records + i;
		sock = tcp_connect_socket(record, end_timestamp);
		if (sock != INVALID_SOCKET)
			break;
	}

	if (sock == INVALID_SOCKET) {
		PLUM_LOG_WARN("Failed to connect to HTTP host %s:%s", host, service);
		return ret;
	}

	size_t size = DEFAULT_BUFFER_SIZE;
	char *buffer = (char*)malloc(size);
	if (!buffer) {
		PLUM_LOG_WARN("Failed to allocate HTTP buffer, size=%zu", size);
		free(buffer);
		closesocket(sock);
		return ret;
	}

	const char *method_str = request->method == HTTP_METHOD_POST ? "POST" : "GET";

	int len;
	if (request->body_size > 0)
		len = snprintf(buffer, size,
		               "%s %s HTTP/1.0\r\n"
		               "Host: %s\r\n"
		               "Connection: close\r\n"
		               "Content-Length: %zu\r\n"
		               "Content-Type: %s\r\n"
		               "%s\r\n",
		               method_str, *path != '\0' ? path : "/", host, request->body_size,
		               request->body_type, request->headers ? request->headers : "");
	else
		len = snprintf(buffer, size,
		               "%s %s HTTP/1.0\r\n"
		               "Host: %s\r\n"
		               "Connection: close\r\n"
		               "%s\r\n",
		               method_str, *path != '\0' ? path : "/", host,
		               request->headers ? request->headers : "");

	if (len < 0 || (size_t)len >= size) {
		PLUM_LOG_WARN("Failed to format HTTP request");
		free(buffer);
		closesocket(sock);
		return ret;
	}

	PLUM_LOG_VERBOSE("Sending HTTP request: %s%s", buffer,
	                 request->body_size > 0 ? request->body : "");

    int sent;
	if ((sent = tcp_send(sock, buffer, len, end_timestamp)) != len) {
        PLUM_LOG_WARN("Failed to send HTTP request");
        if (sent == TCP_ERR_TIMEOUT)
            ret = HTTP_ERR_TIMEOUT;
		free(buffer);
		closesocket(sock);
		return ret;
	}

	if (request->body_size > 0) {
		if ((sent = tcp_send(sock, request->body, request->body_size, end_timestamp)) !=
		    (int)request->body_size) {
            PLUM_LOG_WARN("Failed to send HTTP request body");
            if (sent == TCP_ERR_TIMEOUT)
                ret = HTTP_ERR_TIMEOUT;
			free(buffer);
			closesocket(sock);
			return ret;
		}
	}

	size_t total_len = 0;
	while ((len = tcp_recv(sock, buffer + total_len, size - total_len, end_timestamp)) > 0) {
		total_len += len;

		if (total_len == size) {
			if (size == MAX_BUFFER_SIZE) {
				PLUM_LOG_WARN("HTTP response is too big");
				free(buffer);
				closesocket(sock);
				return ret;
			}

			size_t new_size = size * 2;
			if (new_size > MAX_BUFFER_SIZE)
				new_size = MAX_BUFFER_SIZE;

			char *new_buffer = (char*)realloc(buffer, new_size);
			if (!new_buffer) {
				PLUM_LOG_WARN("Failed to reallocate HTTP buffer, size=%zu", new_size);
				free(buffer);
				closesocket(sock);
				return ret;
			}

			buffer = new_buffer;
			size = new_size;
		}
	}

	if (len < 0) {
        PLUM_LOG_WARN("Failed to receive HTTP response");
        if (len == TCP_ERR_TIMEOUT)
            ret = HTTP_ERR_TIMEOUT;
		free(buffer);
		closesocket(sock);
		return ret;
	}

	buffer[total_len] = '\0'; // null-terminate response
	PLUM_LOG_VERBOSE("Received HTTP response: %s", buffer);

	int code = 0;
	if (sscanf(buffer, "HTTP/%*s %d %*s\n%n", &code, &len) != 1 || code <= 0) {
		PLUM_LOG_WARN("Failed to parse HTTP response status");
		free(buffer);
		closesocket(sock);
		return ret;
	}

	PLUM_LOG_DEBUG("Got HTTP response code %d", code);

	char *headers_begin = buffer + len;
	char *headers_end = strstr(headers_begin, "\r\n\r\n");
	if (!headers_end) {
		PLUM_LOG_WARN("Failed to parse HTTP response headers");
		free(buffer);
		closesocket(sock);
		return ret;
	}
	headers_end += 2;
	*headers_end = '\0';

	// Handle redirections
	if (code >= 300 && code <= 399 && redirections < HTTP_MAX_REDIRECTIONS) {
		char new_url[HTTP_MAX_URL_LEN];
		char location[HTTP_MAX_URL_LEN];
		len = header_extract(headers_begin, "Location", location, HTTP_MAX_URL_LEN);
		if (len > 0) {
			if(location[0] == '/')
				len = snprintf(new_url, HTTP_MAX_URL_LEN, "%.*s%s", (int)(path - url), url, location);
			else
				strcpy(new_url, location);
		}
		if (len > 0 && len <= HTTP_MAX_URL_LEN) {
			PLUM_LOG_DEBUG("Got HTTP redirection to %s", new_url);
			http_request_t new_request = *request;
			new_request.url = new_url;
			free(buffer);
			return http_perform_rec(&new_request, response, end_timestamp, redirections + 1);
		}
	}

	size_t headers_size = headers_end - headers_begin;
	response->headers = (char*)malloc(headers_size + 1);
	if (!response->headers) {
		PLUM_LOG_WARN("Failed to allocate memory for HTTP headers, size=%zu", headers_size + 1);
		free(buffer);
		closesocket(sock);
		return ret;
	}
	memcpy(response->headers, headers_begin, headers_size);
	response->headers[headers_size] = '\0';

	char *body_begin = headers_end + 2;
	char *body_end = buffer + total_len;
	response->body_size = body_end - body_begin;
	memmove(buffer, body_begin, response->body_size);
	response->body = buffer;
	response->body[response->body_size] = '\0';

	PLUM_LOG_DEBUG("Got HTTP body of size %zu", response->body_size);

	closesocket(sock);
	return code;
}

int http_perform(const http_request_t *request, http_response_t *response,
                 timestamp_t end_timestamp) {
	return http_perform_rec(request, response, end_timestamp, 0);
}

void http_free(http_response_t *response) {
	if (!response)
		return;

	free(response->headers);
	free(response->body);

	response->headers = NULL;
	response->body = NULL;
	response->body_size = 0;
}

