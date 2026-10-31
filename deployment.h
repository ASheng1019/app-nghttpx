/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <nghttpx/unikraft.h>
#include <stdint.h>
int app_nghttpx_run(void);
int app_clock_anchor(uint64_t epoch);
int app_control_configure(const char *endpoint, unsigned drain_seconds);
int app_control_ready(struct ev_loop *, struct uk_nghttpx_control *, void *);
void app_control_close(struct ev_loop *, void *);
