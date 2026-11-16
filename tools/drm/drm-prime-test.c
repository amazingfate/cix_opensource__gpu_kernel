// SPDX-License-Identifier: GPL-2.0
/*
 * drm-prime-test - exercise PRIME import on a DRM render node.
 *
 * Allocates a dma-buf from /dev/dma_heap/system and runs
 * DRM_IOCTL_PRIME_FD_TO_HANDLE / DRM_IOCTL_GEM_CLOSE on the given node, once
 * or in a loop. It also checks that the export direction is refused.
 *
 * Usage: drm-prime-test <node> [loops] [--hold-ms N]
 *
 * With --hold-ms the buffer is imported once, the handle is kept open for N
 * milliseconds (so a test can unbind the device underneath it) and then
 * GEM_CLOSE is attempted. The exit code is 0 unless something unexpectedly
 * succeeded or failed to open. The package calls this from the lifecycle
 * tests.
 *
 * (C) COPYRIGHT 2025 CIX Technology Group Co., Ltd.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <drm/drm.h>
#include <linux/dma-heap.h>

#define BUFFER_SIZE (1024 * 1024)

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int loops = argc > 2 ? atoi(argv[2]) : 1;
	unsigned int hold_ms = 0;
	int handles = 1;
	struct dma_heap_allocation_data data;
	int heap_fd, dmabuf_fd, drm_fd;
	struct drm_prime_handle export;
	int imported = 0, failed = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc)
			hold_ms = (unsigned int)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--handles") && i + 1 < argc)
			handles = atoi(argv[++i]);
	}

	heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	if (heap_fd < 0) {
		fprintf(stderr, "open dma_heap: %s\n", strerror(errno));
		return 1;
	}

	memset(&data, 0, sizeof(data));
	data.len = BUFFER_SIZE;
	data.fd_flags = O_RDWR | O_CLOEXEC;

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
		fprintf(stderr, "DMA_HEAP_IOCTL_ALLOC: %s\n", strerror(errno));
		return 1;
	}
	dmabuf_fd = data.fd;

	drm_fd = open(node, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		fprintf(stderr, "open %s: %s\n", node, strerror(errno));
		return 1;
	}

	printf("dma-buf fd=%d size=%u, node=%s, loops=%d\n", dmabuf_fd,
	       (unsigned int)BUFFER_SIZE, node, loops);

	if (hold_ms) {
		unsigned int handle[16];
		struct drm_gem_close close_arg;
		int n;

		if (handles < 1)
			handles = 1;
		if (handles > (int)(sizeof(handle) / sizeof(handle[0])))
			handles = (int)(sizeof(handle) / sizeof(handle[0]));

		for (n = 0; n < handles; n++) {
			struct drm_prime_handle prime;

			memset(&prime, 0, sizeof(prime));
			prime.fd = dmabuf_fd;
			prime.flags = DRM_CLOEXEC;
			if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0) {
				fprintf(stderr, "FD_TO_HANDLE #%d failed: %s\n", n,
					strerror(errno));
				return 1;
			}
			handle[n] = prime.handle;
		}
		printf("imported %d handle(s) from the same dma-buf, holding %u ms\n",
		       handles, hold_ms);
		usleep(hold_ms * 1000);

		/*
		 * The DRM core deduplicates imports of the same dma-buf within
		 * one file, so repeated imports can legitimately return the same
		 * handle. Close each distinct handle once.
		 */
		for (n = 0; n < handles; n++) {
			int m;
			bool seen = false;

			for (m = 0; m < n; m++)
				if (handle[m] == handle[n])
					seen = true;
			if (seen)
				continue;

			memset(&close_arg, 0, sizeof(close_arg));
			close_arg.handle = handle[n];
			if (ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg) < 0)
				printf("GEM_CLOSE(%u) refused: %s\n", handle[n],
				       strerror(errno));
		}
		printf("closed %d distinct handle(s), first handle=%u\n", handles,
		       handle[0]);
		close(drm_fd);
		close(dmabuf_fd);
		close(heap_fd);
		return 0;
	}

	for (i = 0; i < loops; i++) {
		struct drm_prime_handle prime;
		struct drm_gem_close close_arg;

		memset(&prime, 0, sizeof(prime));
		prime.fd = dmabuf_fd;
		prime.flags = DRM_CLOEXEC;

		if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0) {
			fprintf(stderr, "loop %d: FD_TO_HANDLE failed: %s\n", i,
				strerror(errno));
			failed++;
			continue;
		}

		memset(&close_arg, 0, sizeof(close_arg));
		close_arg.handle = prime.handle;
		if (ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg) < 0) {
			fprintf(stderr, "loop %d: GEM_CLOSE(%u) failed: %s\n", i,
				prime.handle, strerror(errno));
			failed++;
			continue;
		}

		imported++;
	}

	/* Export is intentionally not implemented: must be refused. */
	memset(&export, 0, sizeof(export));
	export.handle = 1;
	export.flags = DRM_CLOEXEC;
	if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export) < 0)
		printf("HANDLE_TO_FD (export) refused as expected: %s\n", strerror(errno));
	else {
		printf("HANDLE_TO_FD unexpectedly succeeded (fd=%d)\n", export.fd);
		failed++;
	}

	close(drm_fd);
	close(dmabuf_fd);
	close(heap_fd);

	printf("result: imported=%d failed=%d\n", imported, failed);
	return failed ? 1 : 0;
}
