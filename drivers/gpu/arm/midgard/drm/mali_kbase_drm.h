/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * (C) COPYRIGHT 2025 CIX Technology Group Co., Ltd. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

#ifndef _MALI_KBASE_DRM_H_
#define _MALI_KBASE_DRM_H_

#include <linux/kconfig.h>
#include <linux/types.h>

struct kbase_device;

#if IS_ENABLED(CONFIG_MALI_KBASE_DRM_RENDER)
/**
 * kbase_drm_render_init() - Register the Mali GPU with the DRM core
 * @kbdev: Pointer to the kbase device
 *
 * Registers a DRM device, owned by the Mali GPU platform device, that exposes
 * a standard render node (/dev/dri/renderD*). No GPU submission, KMS or GEM
 * private ioctls are provided by this device.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int kbase_drm_render_init(struct kbase_device *kbdev);

/**
 * kbase_drm_render_term() - Unregister the Mali GPU DRM device
 * @kbdev: Pointer to the kbase device
 *
 * Unregisters the render node registered by kbase_drm_render_init() and drops
 * the driver reference on the DRM device. The DRM device itself is kept alive
 * until the last file descriptor opened on it has been closed.
 */
void kbase_drm_render_term(struct kbase_device *kbdev);
#else
static inline int kbase_drm_render_init(struct kbase_device *kbdev)
{
	(void)kbdev;
	return 0;
}

static inline void kbase_drm_render_term(struct kbase_device *kbdev)
{
	(void)kbdev;
}
#endif /* IS_ENABLED(CONFIG_MALI_KBASE_DRM_RENDER) */

#endif /* _MALI_KBASE_DRM_H_ */
