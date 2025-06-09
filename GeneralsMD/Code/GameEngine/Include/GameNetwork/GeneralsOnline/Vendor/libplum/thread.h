/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUM_THREAD_H
#define PLUM_THREAD_H

#include "timestamp.h"

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7
#endif
#ifndef __MSVCRT_VERSION__
#define __MSVCRT_VERSION__ 0x0601
#endif

#include <windows.h>

typedef CRITICAL_SECTION mutex_t;
typedef CONDITION_VARIABLE cond_t;
typedef HANDLE thread_t;
typedef DWORD thread_return_t;
#define THREAD_CALL __stdcall

#define MUTEX_PLAIN 0x0
#define MUTEX_RECURSIVE 0x0 // mutexes are recursive on Windows

#define mutex_init(m, flags) InitializeCriticalSection(m)
#define mutex_lock(m) EnterCriticalSection(m)
#define mutex_unlock(m) LeaveCriticalSection(m)
#define mutex_destroy(m) DeleteCriticalSection(m)

#define cond_init(c) InitializeConditionVariable(c)
#define cond_wait(c, m) (SleepConditionVariableCS(c, m, INFINITE) ? 0 : (int)GetLastError())
#define cond_timedwait(c, m, msecs)                                                                \
	(SleepConditionVariableCS(c, m, (DWORD)msecs) ? 0 : (int)GetLastError())
#define cond_broadcast(c) WakeAllConditionVariable(c)
#define cond_signal(c) WakeConditionVariable(c)
#define cond_destroy(c) (void)0

static inline void thread_join_impl(thread_t t, thread_return_t *res) {
	WaitForSingleObject(t, INFINITE);
	if (res)
		GetExitCodeThread(t, res);
	CloseHandle(t);
}

#define thread_init(t, func, arg)                                                                  \
	((*(t) = CreateThread(NULL, 0, func, arg, 0, NULL)) != NULL ? 0 : (int)GetLastError())
#define thread_join(t, res) thread_join_impl(t, res)

#else // POSIX

#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t cond_t;
typedef pthread_t thread_t;
typedef void *thread_return_t;
#define THREAD_CALL

#define MUTEX_PLAIN PTHREAD_MUTEX_NORMAL
#define MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE

static inline int mutex_init_impl(mutex_t *m, int flags) {
	pthread_mutexattr_t mutexattr;
	pthread_mutexattr_init(&mutexattr);
	pthread_mutexattr_settype(&mutexattr, flags);
	int ret = pthread_mutex_init(m, &mutexattr);
	pthread_mutexattr_destroy(&mutexattr);
	return ret;
}

#define mutex_init(m, flags) mutex_init_impl(m, flags)
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) (void)pthread_mutex_unlock(m)
#define mutex_destroy(m) (void)pthread_mutex_destroy(m)

static inline int cond_init_impl(cond_t *c) {
	pthread_condattr_t condattr;
	pthread_condattr_init(&condattr);
	// MacOS lacks pthread_condattr_setclock()...
#ifndef __APPLE__
	pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
#endif
	int ret = pthread_cond_init(c, &condattr);
	pthread_condattr_destroy(&condattr);
	return ret;
}

static inline int cond_timedwait_impl(cond_t *c, mutex_t *m, unsigned int msecs) {
#ifndef __APPLE__
	const clockid_t clockid = CLOCK_MONOTONIC;
#else
	const clockid_t clockid = CLOCK_REALTIME;
#endif
	struct timespec ts;
	if (clock_gettime(clockid, &ts))
		return -1;

	ts.tv_sec += msecs / 1000;
	ts.tv_nsec += (long)((msecs % 1000) * 1000000);
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000;
	}
	return pthread_cond_timedwait(c, m, &ts);
}

#define cond_init(c) cond_init_impl(c)
#define cond_wait(c, m) pthread_cond_wait(c, m)
#define cond_timedwait(c, m, msecs) cond_timedwait_impl(c, m, msecs)
#define cond_broadcast(c) pthread_cond_broadcast(c)
#define cond_signal(c) pthread_cond_signal(c)
#define cond_destroy(c) (void)pthread_cond_destroy(c)

#define thread_init(t, func, arg) pthread_create(t, NULL, func, arg)
#define thread_join(t, res) (void)pthread_join(t, res)

#endif // ifdef _WIN32

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)

#include <stdatomic.h>
#define atomic(T) _Atomic(T)
#define atomic_ptr(T) _Atomic(T *)

#else // no atomics

// Since we don't need compare-and-swap, just assume store and load are atomic
#define atomic(T) volatile T
#define atomic_ptr(T) T *volatile
#define atomic_store(a, v) (void)(*(a) = (v))
#define atomic_load(a) (*(a))

#endif // if atomics

#endif // PLUM_THREAD_H
