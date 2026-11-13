# SPDX-License-Identifier: GPL-2.0
#
# Userspace helpers for the Mali kbase DRM work. Kept dependency free (no
# libdrm) so that they can be cross compiled and dropped on a target rootfs.

CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

all: drm-info

drm-info: drm-info.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	$(RM) drm-info

.PHONY: all clean
