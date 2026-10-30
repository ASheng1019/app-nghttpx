# SPDX-License-Identifier: BSD-3-Clause
UK_ROOT ?= $(CURDIR)/../unikraft
LIBS_ROOT ?= $(CURDIR)/../libs
LIBS := musl lwip nghttp3 ngtcp2 nghttp2 c-ares libev zlib openssl libcxx libcxxabi libunwind nghttpx
LIB_PATHS := $(foreach lib,$(LIBS),$(abspath $(LIBS_ROOT)/lib-$(lib)))
empty :=
space := $(empty) $(empty)
L := $(subst $(space),:,$(strip $(LIB_PATHS)))
.DEFAULT_GOAL := all
.PHONY: all defconfig menuconfig olddefconfig clean properclean fetch prepare
all defconfig menuconfig olddefconfig clean properclean fetch prepare:
	$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(L) UK_DEFCONFIG=$(CURDIR)/defconfig $@
