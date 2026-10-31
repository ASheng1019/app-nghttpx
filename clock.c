/* SPDX-License-Identifier: BSD-3-Clause */
/* Explicit deployment adapter, not a platform clock discipline. */
#include "deployment.h"
#include <errno.h>
#include <sys/time.h>
#include <time.h>
static int64_t offset;
static int anchored;
int __real_clock_gettime(clockid_t, struct timespec *);
int app_clock_anchor(uint64_t epoch)
{
	struct timespec mono;
	if (__real_clock_gettime(CLOCK_MONOTONIC, &mono))
		return -1;
	offset = (int64_t)epoch * 1000000000 -
		 (int64_t)mono.tv_sec * 1000000000 - mono.tv_nsec;
	anchored = 1;
	return 0;
}
int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
	int64_t now;
	if (!anchored || (id != CLOCK_REALTIME && id != CLOCK_REALTIME_COARSE))
		return __real_clock_gettime(id, ts);
	if (!ts) {
		errno = EFAULT;
		return -1;
	}
	if (__real_clock_gettime(CLOCK_MONOTONIC, ts))
		return -1;
	now = (int64_t)ts->tv_sec * 1000000000 + ts->tv_nsec + offset;
	ts->tv_sec = now / 1000000000;
	ts->tv_nsec = now % 1000000000;
	return 0;
}
int __wrap_gettimeofday(struct timeval *tv, void *tz)
{
	struct timespec ts;
	(void)tz;
	if (__wrap_clock_gettime(CLOCK_REALTIME, &ts))
		return -1;
	if (tv) {
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / 1000;
	}
	return 0;
}
time_t __wrap_time(time_t *out)
{
	struct timespec ts;
	if (__wrap_clock_gettime(CLOCK_REALTIME, &ts))
		return -1;
	if (out)
		*out = ts.tv_sec;
	return ts.tv_sec;
}
