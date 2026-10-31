/* SPDX-License-Identifier: BSD-3-Clause */
#include "deployment.h"
#include <arpa/inet.h>
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <uk/config.h>
#include <uk/pm.h>
#include <unistd.h>
#if CONFIG_APPNGHTTPX_STATS
#include <inttypes.h>
#include <uk/alloc.h>
#if CONFIG_APPNGHTTPX_FD_STATS
#include <fcntl.h>
#endif
#endif
static struct sockaddr_in endpoint;
#if CONFIG_APPNGHTTPX_SHUTDOWN
static unsigned drain_seconds;
#endif
static char token[65];
static int fd = -1;
static ev_io watcher;
#if CONFIG_APPNGHTTPX_SHUTDOWN
static ev_timer timer;
#endif
static struct uk_nghttpx_control *service;
#if CONFIG_APPNGHTTPX_SHUTDOWN
static void expired(struct ev_loop *loop, ev_timer *w, int events)
{
	(void)loop;
	(void)w;
	(void)events;
	puts("NATIVE_NGHTTPX DRAIN_DEADLINE immediate_vm_shutdown");
	fflush(NULL);
	uk_pm_shutdown(UK_PM_SHUTDOWN_OP_SYSHALT);
}
#endif
#if CONFIG_APPNGHTTPX_STATS
static void reply_stats(const struct sockaddr_in *peer, socklen_t length)
{
	struct uk_nghttpx_stats stat;
	long long used = -1, peak = -1, enomem = -1;
	int descriptors = -1, scan_limit = 0;
	char reply[512];
	int count;
	if (uk_nghttpx_get_stats(service, &stat))
		return;
#if CONFIG_LIBUKALLOC_IFSTATS_GLOBAL
	struct uk_alloc_stats allocator;
	uk_alloc_stats_get_global(&allocator);
	used = allocator.cur_mem_use;
	peak = allocator.max_mem_use;
	enomem = allocator.nb_enomem;
#endif
#if CONFIG_APPNGHTTPX_FD_STATS
	descriptors = 0;
	scan_limit = 4096;
	for (int i = 0; i < scan_limit; ++i)
		if (fcntl(i, F_GETFD) >= 0)
			++descriptors;
#endif
	count = snprintf(
	    reply, sizeof(reply),
	    "{\"heap_used\":%lld,\"heap_peak\":%lld,\"available\":%llu,"
	    "\"enomem\":%lld,"
	    "\"connections\":%" PRIu64 ",\"close_waits\":%" PRIu64
	    ",\"fds\":%d,\"fd_scan_limit\":%d}",
	    used, peak,
	    (unsigned long long)uk_alloc_availmem(uk_alloc_get_default()),
	    enomem, stat.connections, stat.close_waits, descriptors,
	    scan_limit);
	if (count > 0 && (size_t)count < sizeof(reply))
		sendto(fd, reply, count, 0, (const struct sockaddr *)peer,
		       length);
}
#endif
static void receive(struct ev_loop *loop, ev_io *w, int events)
{
	char message[128];
	struct sockaddr_in peer;
	socklen_t length = sizeof(peer);
	ssize_t count;
	(void)loop;
	(void)w;
	(void)events;
	count = recvfrom(fd, message, sizeof(message), 0,
			 (struct sockaddr *)&peer, &length);
#if CONFIG_APPNGHTTPX_STATS
	if (count == 70 && !memcmp(message, "STATS ", 6) &&
	    !memcmp(message + 6, token, 64)) {
		reply_stats(&peer, length);
		return;
	}
#endif
#if CONFIG_APPNGHTTPX_SHUTDOWN
	if (count == 64 && !memcmp(message, token, 64)) {
		puts("NATIVE_NGHTTPX STOP authenticated");
		ev_io_stop(loop, &watcher);
		ev_timer_start(loop, &timer);
		uk_nghttpx_shutdown(service);
	}
#endif
}
int app_control_configure(const char *text, unsigned seconds)
{
#if !CONFIG_APPNGHTTPX_SHUTDOWN && !CONFIG_APPNGHTTPX_STATS
	return -1;
#endif
	char ip[16], extra;
	unsigned port;
	FILE *f;
	int fields;
	/* %n/full-consumption avoids accepting trailing endpoint data. */
	int used = 0;
	if (sscanf(text, "%15[^,],%u%n", ip, &port, &used) != 2 || text[used] ||
	    !port || port > 65535)
		return -1;
	endpoint.sin_family = AF_INET;
	endpoint.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &endpoint.sin_addr) != 1 ||
	    endpoint.sin_addr.s_addr == INADDR_ANY)
		return -1;
	f = fopen("/native-control.token", "r");
	if (!f)
		return -1;
	fields = fscanf(f, "%64s %c", token, &extra);
	fclose(f);
	if (fields != 1 || strlen(token) != 64 ||
	    strspn(token, "0123456789abcdefABCDEF") != 64)
		return -1;
#if CONFIG_APPNGHTTPX_SHUTDOWN
	drain_seconds = seconds;
#else
	(void)seconds;
#endif
	return 0;
}
int app_control_ready(struct ev_loop *loop, struct uk_nghttpx_control *handle,
		      void *user)
{
	(void)user;
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&endpoint, sizeof(endpoint))) {
		close(fd);
		fd = -1;
		return -1;
	}
	service = handle;
	ev_io_init(&watcher, receive, fd, EV_READ);
#if CONFIG_APPNGHTTPX_SHUTDOWN
	ev_timer_init(&timer, expired, drain_seconds, 0);
#endif
	ev_io_start(loop, &watcher);
	printf("NATIVE_NGHTTPX CONTROL port=%u\n", ntohs(endpoint.sin_port));
	return 0;
}
void app_control_close(struct ev_loop *loop, void *user)
{
	(void)user;
#if CONFIG_APPNGHTTPX_SHUTDOWN
	ev_timer_stop(loop, &timer);
#endif
	if (fd >= 0) {
		ev_io_stop(loop, &watcher);
		close(fd);
		fd = -1;
	}
	service = NULL;
}
