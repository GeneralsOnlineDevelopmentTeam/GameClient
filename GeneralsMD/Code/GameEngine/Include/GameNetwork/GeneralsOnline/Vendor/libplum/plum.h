/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_H
#define PLUM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../NextGenMP_defines.h"

#ifndef PLUM_STATIC // dynamic library
#  ifdef _WIN32
#    ifdef PLUM_EXPORTS
#      define PLUM_EXPORT __declspec(dllexport) // building the library
#    else
#      define PLUM_EXPORT __declspec(dllimport) // using the library
#    endif
#  else // not WIN32
#    if defined(__has_attribute)
#      if __has_attribute(visibility)
#        define PLUM_EXPORT __attribute__((visibility("default")))
#      endif
#    endif
#  endif
#endif
#ifndef PLUM_EXPORT
#  define PLUM_EXPORT
#endif

#define PLUM_ERR_SUCCESS 0
#define PLUM_ERR_INVALID -1   // invalid argument
#define PLUM_ERR_FAILED -2    // runtime error
#define PLUM_ERR_NOT_AVAIL -3 // element not available

typedef enum {
	PLUM_LOG_LEVEL_VERBOSE = 0,
	PLUM_LOG_LEVEL_DEBUG = 1,
	PLUM_LOG_LEVEL_INFO = 2,
	PLUM_LOG_LEVEL_WARN = 3,
	PLUM_LOG_LEVEL_ERROR = 4,
	PLUM_LOG_LEVEL_FATAL = 5,
	PLUM_LOG_LEVEL_NONE = 6
} plum_log_level_t;

typedef void (*plum_log_callback_t)(plum_log_level_t level, const char *message);

typedef struct {
	plum_log_level_t log_level;
	plum_log_callback_t log_callback; // NULL means stdout
	const char *dummytls_domain;      // NULL means disabled
} plum_config_t;

PLUM_EXPORT int plum_init(const plum_config_t *config);
PLUM_EXPORT int plum_cleanup(void);

typedef enum {
	PLUM_IP_PROTOCOL_TCP = 0,
	PLUM_IP_PROTOCOL_UDP = 1
} plum_ip_protocol_t;

typedef enum {
	PLUM_STATE_DESTROYED = 0,
	PLUM_STATE_PENDING = 1,
	PLUM_STATE_SUCCESS = 2,
	PLUM_STATE_FAILURE = 3,
	PLUM_STATE_DESTROYING = 4
} plum_state_t;

#define PLUM_MAX_HOST_LEN 256
#define PLUM_MAX_ADDRESS_LEN 64

typedef struct {
	plum_ip_protocol_t protocol;
	uint16_t internal_port;
	uint16_t external_port;
	char external_host[PLUM_MAX_HOST_LEN];
	void *user_ptr;
} plum_mapping_t;

// Callback will be called on SUCCESS and FAILURE
typedef void (*plum_mapping_callback_t)(int id, plum_state_t state, const plum_mapping_t *mapping);

PLUM_EXPORT int plum_create_mapping(const plum_mapping_t *mapping,
                                    plum_mapping_callback_t callback);
PLUM_EXPORT int plum_query_mapping(int id, plum_state_t *state, plum_mapping_t *mapping);
PLUM_EXPORT int plum_destroy_mapping(int id);

PLUM_EXPORT int plum_get_local_address(char *buffer, size_t size);

typedef enum {
	PLUM_DUMMYTLS_PEM_CERT = 0,
	PLUM_DUMMYTLS_PEM_CHAIN = 1,
	PLUM_DUMMYTLS_PEM_FULLCHAIN = 2,
	PLUM_DUMMYTLS_PEM_PRIVKEY = 3
} plum_dummytls_cert_type_t;

PLUM_EXPORT int plum_get_dummytls_certificate(plum_dummytls_cert_type_t type, char *buffer, size_t size);
PLUM_EXPORT int plum_get_dummytls_host(const char *address, char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif
