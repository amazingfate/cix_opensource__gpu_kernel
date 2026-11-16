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

#ifndef _MALI_KBASE_DRM_GEM_H_
#define _MALI_KBASE_DRM_GEM_H_

#include <linux/mutex.h>
#include <linux/types.h>

#include <drm/drm_gem.h>

struct dma_buf;
struct drm_device;
struct kbase_context;
struct kbase_device;
struct kbase_va_region;

/**
 * struct kbase_drm_device - Private data of the Mali render node DRM device
 * @kbdev:          The kbase device this DRM device belongs to
 * @import_ctx:     kbase context used for dma-buf imports, created lazily
 * @import_lock:    Protects @import_ctx against concurrent imports
 */
struct kbase_drm_device {
	struct kbase_device *kbdev;
	struct kbase_context *import_ctx;
	struct mutex import_lock;
	struct list_head gem_list;
};

/**
 * struct kbase_drm_gem_object - GEM object backed by an imported dma-buf
 * @base:     DRM GEM object
 * @kctx:     kbase context the buffer is mapped into
 * @reg:      kbase VA region created by the UMM import path, NULL when the
 *            import or the GPU mapping failed
 * @va_pages: Number of GPU virtual pages of @reg
 */
struct kbase_drm_gem_object {
	struct drm_gem_object base;
	struct kbase_drm_device *priv;
	struct kbase_context *kctx;
	struct kbase_va_region *reg;
	u64 va_pages;
	struct list_head link;
};

extern const struct drm_gem_object_funcs kbase_drm_gem_funcs;

#define to_kbase_drm_gem(obj) container_of(obj, struct kbase_drm_gem_object, base)

/**
 * kbase_drm_import_context_get() - Get the dma-buf import context
 * @priv: Render node private data
 *
 * Creates the kbase context used for dma-buf imports on first use. The caller
 * must hold @priv->import_lock.
 *
 * Return: the import context, or NULL on error.
 */
struct kbase_context *kbase_drm_import_context_get(struct kbase_drm_device *priv);

/**
 * kbase_drm_import_context_term() - Destroy the dma-buf import context
 * @priv: Render node private data
 *
 * Called when the DRM device is released, at which point no GEM object can
 * reference the context any more.
 */
void kbase_drm_import_context_term(struct kbase_drm_device *priv);

/**
 * kbase_drm_gem_term() - Release all imported buffers and the import context
 * @priv: Render node private data
 *
 * Called while the kbase device is still fully alive (before the rest of
 * kbase_device_term runs) and again when the DRM device is released. Buffers
 * whose file descriptors are still open keep their GEM object but lose the
 * kbase region; their free callback then only releases the wrapper.
 */
void kbase_drm_gem_term(struct kbase_drm_device *priv);

/**
 * kbase_drm_gem_prime_import() - Import a dma-buf as a GEM object
 * @dev:      The render node DRM device
 * @dma_buf:  dma-buf to import
 *
 * Implements drm_driver.gem_prime_import. The buffer is imported through the
 * kbase UMM path and mapped into the render node's import context.
 *
 * Return: the new GEM object, or an ERR_PTR on failure.
 */
struct drm_gem_object *kbase_drm_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf);

#endif /* _MALI_KBASE_DRM_GEM_H_ */
