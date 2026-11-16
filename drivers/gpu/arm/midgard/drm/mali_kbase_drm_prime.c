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
 * DOC: Mali kbase DRM PRIME support
 *
 * Only import is supported: an external dma-buf is wrapped into a GEM object
 * whose backing region is created by the kbase UMM import path. Export,
 * mmap and native allocation are deliberately not implemented.
 */

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <drm/drm_device.h>
#include <drm/drm_gem.h>

#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_mem.h>
#include <context/mali_kbase_context.h>

#include "mali_kbase_drm_gem.h"

struct drm_gem_object *kbase_drm_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct kbase_drm_device *priv = dev->dev_private;
	struct kbase_drm_gem_object *obj;
	struct kbase_va_region *reg;
	base_mem_alloc_flags flags = 0;
	u64 va_pages = 0;
	int err;

	if (!priv)
		return ERR_PTR(-ENODEV);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	err = drm_gem_object_init(dev, &obj->base, dma_buf->size);
	if (err)
		goto err_free_obj;

	obj->priv = priv;
	obj->base.funcs = &kbase_drm_gem_funcs;

	mutex_lock(&priv->import_lock);

	obj->kctx = kbase_drm_import_context_get(priv);
	if (!obj->kctx) {
		mutex_unlock(&priv->import_lock);
		dev_err(dev->dev, "failed to create the dma-buf import context\n");
		err = -ENOMEM;
		goto err_release_obj;
	}

	/*
	 * kbase_mem_import_dma_buf() consumes the reference handed to it, so
	 * take our own: the dma_buf passed by the DRM core is only borrowed.
	 */
	get_dma_buf(dma_buf);
	reg = kbase_mem_import_dma_buf(obj->kctx, dma_buf, &va_pages, &flags, 0, true);
	if (!reg) {
		mutex_unlock(&priv->import_lock);
		dev_err(dev->dev, "failed to import the dma-buf through the UMM path\n");
		err = -ENODEV;
		goto err_release_obj;
	}

	obj->reg = reg;
	obj->va_pages = va_pages;

	/* Map the imported region into the render node's GPU address space. */
	kbase_gpu_vm_lock_with_pmode_sync(obj->kctx);
	err = kbase_gpu_mmap(obj->kctx, reg, 0, va_pages, 1, CALLER_MMU_SYNC);
	kbase_gpu_vm_unlock_with_pmode_sync(obj->kctx);
	if (err) {
		dev_err(dev->dev, "failed to map the imported dma-buf: %d\n", err);
		kbase_gpu_vm_lock_with_pmode_sync(obj->kctx);
		kbase_free_alloced_region(obj->reg);
		kbase_gpu_vm_unlock_with_pmode_sync(obj->kctx);
		obj->reg = NULL;
		mutex_unlock(&priv->import_lock);
		goto err_release_obj;
	}

	list_add_tail(&obj->link, &priv->gem_list);
	mutex_unlock(&priv->import_lock);

	return &obj->base;

err_release_obj:
	drm_gem_object_release(&obj->base);
err_free_obj:
	kfree(obj);
	return ERR_PTR(err);
}
