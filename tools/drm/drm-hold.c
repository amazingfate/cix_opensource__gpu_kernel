// SPDX-License-Identifier: GPL-2.0
/*
 * drm-hold - hold DRM render node file descriptors open, then exercise them.
 *
 * Used by the lifecycle tests: keep fds open across a device unbind (or a
 * suspend/resume), then check that an ioctl on the already-open fd is refused
 * with ENODEV instead of reaching freed driver state.
 *
 * Usage:
 *   drm-hold <node> [hold_ms] [--fork-children N] [--fork-close]
 *
 *   --fork-children N  fork N children that inherit the fd and close it after
 *                      hold_ms/2
 *   --fork-close       fork one child that closes the fd immediately while the
 *                      parent keeps using it
 *
 * Exit status is 0 when every operation reported a result (an ioctl error is
 * a valid result, it is printed), 1 on an unexpected failure (open, fork).
 *
 * (C) COPYRIGHT 2025 CIX Technology Group Co., Ltd.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm/drm.h>

static int probe_ioctl(int fd)
{
	struct drm_get_cap cap;
	int ret;

	memset(&cap, 0, sizeof(cap));
	cap.capability = DRM_CAP_SYNCOBJ;

	ret = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
	if (ret < 0)
		printf("drm-hold: pid=%d ioctl=errno:%d(%s)\n", getpid(), errno,
		       strerror(errno));
	else
		printf("drm-hold: pid=%d ioctl=ok value=0x%llx\n", getpid(),
		       (unsigned long long)cap.value);

	return ret;
}

int main(int argc, char **argv)
{
	unsigned int hold_ms = 0;
	int fork_children = 0;
	int fork_close = 0;
	const char *node;
	int fd;
	int i;
	int status = 0;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <node> [hold_ms] [--fork-children N] [--fork-close]\n",
			argv[0]);
		return 2;
	}

	node = argv[1];

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--fork-children") && i + 1 < argc) {
			fork_children = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--fork-close")) {
			fork_close = 1;
		} else {
			hold_ms = (unsigned int)atoi(argv[i]);
		}
	}

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "drm-hold: open %s: %s\n", node, strerror(errno));
		return 1;
	}

	printf("drm-hold: pid=%d opened %s\n", getpid(), node);

	if (fork_close) {
		pid_t child = fork();

		if (child < 0) {
			perror("fork");
			return 1;
		}
		if (child == 0) {
			close(fd);
			printf("drm-hold: child pid=%d closed inherited fd\n", getpid());
			_exit(0);
		}
		waitpid(child, NULL, 0);
	}

	if (fork_children) {
		for (i = 0; i < fork_children; i++) {
			pid_t child = fork();

			if (child < 0) {
				perror("fork");
				return 1;
			}
			if (child == 0) {
				usleep((hold_ms / 2) * 1000);
				probe_ioctl(fd);
				close(fd);
				printf("drm-hold: child pid=%d closed\n", getpid());
				_exit(0);
			}
		}
	}

	if (hold_ms)
		usleep(hold_ms * 1000);

	probe_ioctl(fd);

	for (i = 0; i < fork_children; i++)
		wait(&status);

	if (close(fd) < 0) {
		fprintf(stderr, "drm-hold: close: %s\n", strerror(errno));
		return 1;
	}

	printf("drm-hold: pid=%d closed\n", getpid());
	return 0;
}
