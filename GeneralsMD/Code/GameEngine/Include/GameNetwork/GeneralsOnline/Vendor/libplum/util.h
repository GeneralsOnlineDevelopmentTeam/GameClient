/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// String extraction utils
int string_extract(const char *str, const char *before, const char *after, char *buffer, size_t size, bool case_insensitive);
int header_extract(const char *str, const char *name, char *buffer, size_t size);
int xml_extract(const char *str, const char *tag, char *buffer, size_t size);
const char *xml_find_matching_child(const char *str, const char *tag, const char *child_tag, const char *child_value);

