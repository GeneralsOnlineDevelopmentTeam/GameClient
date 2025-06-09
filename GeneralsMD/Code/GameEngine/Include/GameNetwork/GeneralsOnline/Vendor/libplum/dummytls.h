/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_DUMMYTLS_H
#define PLUM_DUMMYTLS_H

#include "plum.h"
#include "socket.h"
#include "thread.h"
#include "timestamp.h"

#define DUMMYTLS_MAX_DOMAIN_LEN 256
#define DUMMYTLS_MAX_URL_LEN (DUMMYTLS_MAX_DOMAIN_LEN + 32)

void dummytls_init(void);
void dummytls_cleanup(void);

int dummytls_set_domain(const char *domain);
int dummytls_get_host(const struct sockaddr *sa, char *buffer, size_t size);
int dummytls_get_cert(plum_dummytls_cert_type_t type, char *buffer, size_t size);
int dummytls_renew_certs(void);

#endif
