/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_NET_H
#define PLUM_NET_H

#include "addr.h"
#include "socket.h"

int net_get_default_interface(int family, addr_record_t *record);
int net_get_default_gateway(int family, addr_record_t *record);

#endif // PLUM_NET_H
