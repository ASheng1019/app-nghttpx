/* SPDX-License-Identifier: BSD-3-Clause */
#include "deployment.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uk/config.h>
#if CONFIG_APPNGHTTPX_IPV6
#include <uk/lwip-ipv6.h>
#endif
static int number(const char *s, unsigned long long low,
		  unsigned long long high, unsigned long long *out)
{
	char *end;
	unsigned long long value;
	if (!*s || strspn(s, "0123456789") != strlen(s))
		return -1;
	errno = 0;
	value = strtoull(s, &end, 10);
	if (errno || *end || value < low || value > high)
		return -1;
	*out = value;
	return 0;
}
#if CONFIG_APPNGHTTPX_BOOT_EPOCH
static int anchor_clock(void)
{
	FILE *f = fopen("/native-epoch.txt", "r");
	char text[32], extra;
	unsigned long long epoch;
	int count;
	if (!f)
		return -1;
	count = fscanf(f, "%31s %c", text, &extra);
	fclose(f);
	if (count != 1 || number(text, 1704067200, 4102444800ULL, &epoch) ||
	    app_clock_anchor(epoch))
		return -1;
	printf("NATIVE_NGHTTPX TIME trusted_boot_epoch=%llu\n", epoch);
	return 0;
}
#endif
int app_nghttpx_run(void)
{
	struct uk_nghttpx_options options = {.max_udp_payload = 1200};
	char control[64] = "10.0.2.15,4434";
	unsigned drain = 15;
	char iface[16] = "", address[48] = "";
	unsigned ipv6_flags = 0, ipv6_timeout = 6000;
	FILE *f;
	char *line = NULL;
	size_t capacity = 0;
	/* Track duplicate keys; reject rather than silently overriding
	 * deployment. */
	unsigned seen = 0;
	int failed = 0;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	f = fopen("/native-runtime.conf", "r");
	if (!f && errno != ENOENT)
		goto fail;
	while (f && getline(&line, &capacity, f) >= 0) {
		char *key = line + strspn(line, " \t\r\n"), *value, *end;
		unsigned bit;
		unsigned long long parsed;
		if (!*key || *key == '#')
			continue;
		end = key + strlen(key);
		while (end > key && strchr(" \t\r\n", end[-1]))
			--end;
		*end = 0;
		value = strchr(key, '=');
		if (!value || strlen(key) > 4096) {
			failed = 1;
			break;
		}
		*value++ = 0;
		if (!strcmp(key, "pmtud") || !strcmp(key, "ecn")) {
			bit = !strcmp(key, "pmtud") ? 1 : 2;
			if (strcmp(value, "yes") && strcmp(value, "no")) {
				failed = 1;
				break;
			}
			if (bit == 1)
				options.pmtud = !strcmp(value, "yes");
			else
				options.ecn = !strcmp(value, "yes");
		} else if (!strcmp(key, "max-udp-payload")) {
			bit = 4;
			if (number(value, 1200, 1452, &parsed)) {
				failed = 1;
				break;
			}
			options.max_udp_payload = parsed;
#if CONFIG_APPNGHTTPX_CONTROL
		} else if (!strcmp(key, "control")) {
			bit = 8;
			if (strlen(value) >= sizeof(control)) {
				failed = 1;
				break;
			}
			strcpy(control, value);
#endif
#if CONFIG_APPNGHTTPX_SHUTDOWN
		} else if (!strcmp(key, "drain-seconds")) {
			bit = 16;
			if (number(value, 1, 300, &parsed)) {
				failed = 1;
				break;
			}
			drain = parsed;
#endif
#if CONFIG_APPNGHTTPX_IPV6
		} else if (!strcmp(key, "ipv6-interface")) {
			bit = 32;
			if (!*value || strlen(value) >= sizeof(iface)) {
				failed = 1;
				break;
			}
			strcpy(iface, value);
		} else if (!strcmp(key, "ipv6-dad-timeout-ms")) {
			bit = 512;
			if (number(value, 1, 60000, &parsed)) {
				failed = 1;
				break;
			}
			ipv6_timeout = parsed;
		} else if (!strcmp(key, "ipv6-address")) {
			bit = 64;
			if (!*value || strlen(value) >= sizeof(address)) {
				failed = 1;
				break;
			}
			strcpy(address, value);
		} else if (!strcmp(key, "ipv6-slaac") ||
			   !strcmp(key, "ipv6-multicast-rx")) {
			bit = !strcmp(key, "ipv6-slaac") ? 128 : 256;
			if (strcmp(value, "yes") && strcmp(value, "no")) {
				failed = 1;
				break;
			}
			if (!strcmp(value, "yes"))
				ipv6_flags |= bit == 128
						  ? UK_LWIP_IP6_SLAAC
						  : UK_LWIP_IP6_RX_MULTICAST;
#endif
		} else {
			printf("NATIVE_NGHTTPX RUNTIME rejected key=%s\n", key);
			failed = 1;
			break;
		}
		if (seen & bit) {
			failed = 1;
			break;
		}
		seen |= bit;
	}
	if (f) {
		failed |= ferror(f);
		fclose(f);
	}
	free(line);
	if (failed)
		goto fail;
#if CONFIG_APPNGHTTPX_BOOT_EPOCH
	if (anchor_clock())
		goto fail;
#endif
#if CONFIG_APPNGHTTPX_IPV6
	if ((seen & (64 | 128 | 256 | 512)) && !*iface)
		goto fail;
	if (*iface) {
		int rc = uk_lwip_ip6_configure(iface, *address ? address : NULL,
					       ipv6_flags, ipv6_timeout);
		if (rc) {
			printf("NATIVE_NGHTTPX IPV6 failed status=%d\n", rc);
			goto fail;
		}
	}
#endif
#if CONFIG_APPNGHTTPX_CONTROL
	if (app_control_configure(control, drain))
		goto fail;
	options.on_ready = app_control_ready;
	options.on_close = app_control_close;
#else
	(void)control;
	(void)drain;
#endif
#if !CONFIG_APPNGHTTPX_IPV6
	(void)iface;
	(void)address;
	(void)ipv6_flags;
	(void)ipv6_timeout;
#endif
	return uk_nghttpx_run(&options);
fail:
	puts("NATIVE_NGHTTPX FAIL deployment configuration");
	return 1;
}
