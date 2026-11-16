// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
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

/**
 * DOC: Mali kbase DRM GEM objects
 *
 * GEM objects on the render node are wrappers around a dma-buf imported through
 * the kbase UMM path. There is no native allocation, export or mmap support:
 * the object only exists so that standard user space can turn an external
 * dma-buf into a GEM handle and back.
 */

#include <linux/kernel.h>
#include <linux/slab.h>

#include <drm/drm_device.h>
#include <drm/drm_gem.h>

#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_mem.h>
#include <context/mali_kbase_context.h>
#include <device/mali_kbase_device.h>

#include "mali_kbase_drm_gem.h"

static void kbase_drm_gem_free(struct drm_gem_object *gem_obj)
{
	struct kbase_drm_gem_object *obj = to_kbase_drm_gem(gem_obj);
	struct kbase_drm_device *priv = obj->priv;

	mutex_lock(&priv->import_lock);
	if (obj->reg) {
		kbase_gpu_vm_lock_with_pmode_sync(obj->kctx);
		kbase_mem_free_region(obj->kctx, obj->reg);
		kbase_gpu_vm_unlock_with_pmode_sync(obj->kctx);
		obj->reg = NULL;
	}
	list_del(&obj->link);
	mutex_unlock(&priv->import_lock);

	obj->kctx = NULL;
	kfree(obj);
}

const struct drm_gem_object_funcs kbase_drm_gem_funcs = {
	.free = kbase_drm_gem_free,
};

struct kbase_context *kbase_drm_import_context_get(struct kbase_drm_device *priv)
{
	lockdep_assert_held(&priv->import_lock);

	if (!priv->import_ctx) {
		/*
		 * Context creation requires the CSF firmware and the scheduler
		 * kthread. On /dev/maliX this is done on the first open; the
		 * render node has no such open path, so do it here.
		 */
		int err = kbase_device_firmware_init_once(priv->kbdev);

		if (err)
			return NULL;

		/*
		 * A 64-bit context has no CUSTOM_VA zone until user space
		 * requests a JIT region, and the SAME_VA path would hand back a
		 * cookie that only an mmap() can resolve. The render node maps
		 * imports immediately, so use a compatibility-mode context,
		 * which owns a CUSTOM_VA zone from creation time. No user
		 * pointers are involved, so the compat flag is harmless here.
		 *
		 * The api_version only gates MEM_QUERY flags, which an internal
		 * import-only context never queries, so 0 is safe.
		 */
		priv->import_ctx = kbase_create_context(priv->kbdev, true, 0, 0, NULL);
	}

	return priv->import_ctx;
}

void kbase_drm_gem_term(struct kbase_drm_device *priv)
{
	struct kbase_drm_gem_object *obj;

	if (!priv)
		return;

	mutex_lock(&priv->import_lock);

	list_for_each_entry(obj, &priv->gem_list, link) {
		if (!obj->reg)
			continue;

		kbase_gpu_vm_lock_with_pmode_sync(obj->kctx);
		kbase_mem_free_region(obj->kctx, obj->reg);
		kbase_gpu_vm_unlock_with_pmode_sync(obj->kctx);
		obj->reg = NULL;
	}

	kbase_drm_import_context_term(priv);
	mutex_unlock(&priv->import_lock);
}

void kbase_drm_import_context_term(struct kbase_drm_device *priv)
{
	if (!priv->import_ctx)
		return;

	kbase_destroy_context(priv->import_ctx);
	priv->import_ctx = NULL;
}
