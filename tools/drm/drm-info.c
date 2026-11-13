// SPDX-License-Identifier: GPL-2.0
/*
 * drm-info - print the identity and the DRM core capabilities of a DRM node.
 *
 * Deliberately does not link against libdrm so that it can be built and run on
 * a target rootfs that has no -dev packages installed. The output is stable
 * enough to be consumed by tools/drm/01-current-behavior.sh.
 *
 * (C) COPYRIGHT 2025 CIX Technology Group Co., Ltd.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include <drm/drm.h>

static const char *errstr(int err)
{
	static char buf[128];

	if (err < 0)
		err = -err;
	snprintf(buf, sizeof(buf), "%s (%d)", strerror(err), err);
	return buf;
}

static void print_parent(const char *node)
{
	char link[256];
	char path[300];
	const char *base;
	ssize_t len;

	base = strrchr(node, '/');
	base = base ? base + 1 : node;

	snprintf(path, sizeof(path), "/sys/class/drm/%s/device", base);
	len = readlink(path, link, sizeof(link) - 1);
	if (len < 0) {
		printf("  parent: <none> (%s)\n", errstr(errno));
		return;
	}
	link[len] = '\0';
	printf("  parent: %s\n", link);
}

static void print_version(int fd)
{
	struct drm_version ver;
	char name[128] = { 0 };
	char date[128] = { 0 };
	char desc[256] = { 0 };
	int ret;

	memset(&ver, 0, sizeof(ver));

	/* First call only asks for the string lengths. */
	ret = ioctl(fd, DRM_IOCTL_VERSION, &ver);
	if (ret < 0) {
		printf("  version: <failed: %s>\n", errstr(errno));
		return;
	}

	ver.name = name;
	ver.name_len = sizeof(name) - 1;
	ver.date = date;
	ver.date_len = sizeof(date) - 1;
	ver.desc = desc;
	ver.desc_len = sizeof(desc) - 1;

	ret = ioctl(fd, DRM_IOCTL_VERSION, &ver);
	if (ret < 0) {
		printf("  version: <failed: %s>\n", errstr(errno));
		return;
	}

	/* The kernel truncates to the buffer size and does not NUL terminate. */
	name[sizeof(name) - 1] = '\0';
	date[sizeof(date) - 1] = '\0';
	desc[sizeof(desc) - 1] = '\0';

	printf("  driver: %s\n", name);
	printf("  desc: %s\n", desc);
	printf("  date: %s\n", date);
	printf("  version: %d.%d.%d\n", ver.version_major, ver.version_minor,
	       ver.version_patchlevel);
}

static void print_cap(int fd, const char *label, uint64_t cap)
{
	struct drm_get_cap get_cap;
	int ret;

	memset(&get_cap, 0, sizeof(get_cap));
	get_cap.capability = cap;

	ret = ioctl(fd, DRM_IOCTL_GET_CAP, &get_cap);
	if (ret < 0) {
		printf("  cap_%s: <failed: %s>\n", label, errstr(errno));
		return;
	}

	printf("  cap_%s: 0x%llx\n", label, (unsigned long long)get_cap.value);
}

/*
 * Probe PRIME by actually issuing the ioctls. DRM_IOCTL_GET_CAP(DRM_CAP_PRIME)
 * is useless from Linux 6.6 on: the core returns IMPORT|EXPORT for every
 * driver, including the ones without GEM. Only the real ioctl tells the truth.
 */
static void print_prime_probe(int fd)
{
	struct drm_prime_handle prime;
	int ret;

	memset(&prime, 0, sizeof(prime));
	prime.handle = 1;
	prime.flags = DRM_CLOEXEC;
	prime.fd = -1;

	ret = ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ret < 0)
		printf("  prime_handle_to_fd: rejected: %s\n", errstr(errno));
	else
		printf("  prime_handle_to_fd: accepted (fd=%d)\n", prime.fd);

	memset(&prime, 0, sizeof(prime));
	/* -1 is never a valid dma-buf fd: this only tests the entry point. */
	prime.fd = -1;

	ret = ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
	if (ret < 0)
		printf("  prime_fd_to_handle: rejected: %s\n", errstr(errno));
	else
		printf("  prime_fd_to_handle: accepted (handle=%u)\n", prime.handle);
}

static int probe_node(const char *node)
{
	struct stat st;
	int fd;
	int ret = 0;

	if (stat(node, &st) < 0) {
		fprintf(stderr, "%s: %s\n", node, strerror(errno));
		return 1;
	}

	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s: not a character device\n", node);
		return 1;
	}

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "%s: open: %s\n", node, strerror(errno));
		return 1;
	}

	printf("node: %s\n", node);
	printf("  major=%u minor=%u\n", major(st.st_rdev), minor(st.st_rdev));
	print_parent(node);
	print_version(fd);
	print_cap(fd, "prime", DRM_CAP_PRIME);
	print_cap(fd, "syncobj", DRM_CAP_SYNCOBJ);
	print_cap(fd, "syncobj_timeline", DRM_CAP_SYNCOBJ_TIMELINE);
	print_prime_probe(fd);

	close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	int errors = 0;
	int i;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s /dev/dri/renderD128 [/dev/dri/card0 ...]\n",
			argv[0]);
		return 2;
	}

	for (i = 1; i < argc; i++)
		errors += probe_node(argv[i]);

	return errors ? 1 : 0;
}
