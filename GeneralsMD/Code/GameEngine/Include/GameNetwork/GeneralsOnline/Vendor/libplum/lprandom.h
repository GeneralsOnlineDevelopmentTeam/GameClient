/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_RANDOM_H
#define PLUM_RANDOM_H

#include <stdint.h>
#include <stdlib.h>

void plum_random_init();
void plum_random_cleanup();

void plum_random(void *buf, size_t size);

uint32_t plum_rand32(void);
uint64_t plum_rand64(void);

#endif // PLUM_RANDOM_H
