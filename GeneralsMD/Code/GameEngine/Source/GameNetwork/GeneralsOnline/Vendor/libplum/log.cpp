/**
 * Copyright (c) 2020-2021 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "log.h"
#include "thread.h" // for mutexes and atomics

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define BUFFER_SIZE 4096

static const char *log_level_names[] = {"VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

static const char *log_level_colors[] = {
    "\x1B[90m",        // grey
    "\x1B[96m",        // cyan
    "\x1B[39m",        // default foreground
    "\x1B[93m",        // yellow
    "\x1B[91m",        // red
    "\x1B[97m\x1B[41m" // white on red
};

static atomic(plum_log_level_t) log_level = PLUM_LOG_LEVEL_NONE;
static plum_log_callback_t log_callback = NULL;
static mutex_t log_mutex;

static bool use_color(void) {
#ifdef _WIN32
	return false;
#else
	return isatty(fileno(stdout)) != 0;
#endif
}

static int get_localtime(const time_t *t, struct tm *buf) {
#ifdef _WIN32
	// Windows does not have POSIX localtime_r...
	return localtime_s(buf, t) == 0 ? 0 : -1;
#else // POSIX
	return localtime_r(t, buf) != NULL ? 0 : -1;
#endif
}

void plum_log_init() {
	mutex_init(&log_mutex, 0);
}

void plum_log_cleanup() {
	mutex_destroy(&log_mutex);
}

void plum_set_log_level(plum_log_level_t level) {
	atomic_store(&log_level, level);
}

void plum_set_log_handler(plum_log_callback_t callback) {
	mutex_lock(&log_mutex);
	log_callback = callback;
	mutex_unlock(&log_mutex);
}

bool plum_log_is_enabled(plum_log_level_t level) {
	return level != PLUM_LOG_LEVEL_NONE && level >= atomic_load(&log_level);
}

void plum_log_write(plum_log_level_t level, const char *file, int line, const char *fmt, ...) {
	if (!plum_log_is_enabled(level))
		return;

	mutex_lock(&log_mutex);

#if !RELEASE
	const char *filename = file + strlen(file);
	while (filename != file && *filename != '/' && *filename != '\\')
		--filename;
	if (filename != file)
		++filename;
#else
	(void)file;
	(void)line;
#endif

	if (log_callback) {
		char message[BUFFER_SIZE];
		int len = 0;
#if !RELEASE
		len = snprintf(message, BUFFER_SIZE, "%s:%d: ", filename, line);
		if (len < 0)
			goto __exit;
#endif
		if (len < BUFFER_SIZE) {
			va_list args;
			va_start(args, fmt);
			vsnprintf(message + len, BUFFER_SIZE - len, fmt, args);
			va_end(args);
		}

		log_callback(level, message);

	} else {
		time_t t = time(NULL);
		struct tm lt;
		char buffer[16];
		if (get_localtime(&t, &lt) != 0 || strftime(buffer, 16, "%H:%M:%S", &lt) == 0)
			buffer[0] = '\0';

		if (use_color())
			fprintf(stdout, "%s", log_level_colors[level]);

		fprintf(stdout, "%s %-7s ", buffer, log_level_names[level]);

#if !RELEASE
		fprintf(stdout, "%s:%d: ", filename, line);
#endif

		va_list args;
		va_start(args, fmt);
		vfprintf(stdout, fmt, args);
		va_end(args);

		if (use_color())
			fprintf(stdout, "%s", "\x1B[0m\x1B[0K");

		fprintf(stdout, "\n");
		fflush(stdout);
	}

#if !RELEASE
	// Place the goto label under #ifdef so that this code doesn't
	// trigger `-Wunused-label` warning in release builds.
__exit:
#endif
	mutex_unlock(&log_mutex);
}
