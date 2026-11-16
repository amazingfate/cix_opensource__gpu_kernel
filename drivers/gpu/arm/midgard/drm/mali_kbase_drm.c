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
 * DOC: Mali kbase DRM render node
 *
 * This file registers the Mali GPU with the DRM core as a render-node-only
 * DRM device. It lets generic user space discover the GPU through the
 * standard /sys/class/drm and /dev/dri interfaces instead of relying on a
 * vendor specific device.
 *
 * The DRM device deliberately advertises DRIVER_RENDER only: no KMS, no GEM
 * and no private GPU submission ioctls. The /dev/mali0 user/kernel ABI is left
 * untouched; all memory and job submission still goes through the kbase
 * ioctls. GEM/PRIME support is added separately once the device model is
 * proven.
 */

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_print.h>

#include <mali_kbase.h>
#include <mali_kbase_defs.h>

#include "mali_kbase_drm.h"
#include "mali_kbase_drm_gem.h"

/*
 * The render node exposes the DRM core ioctls (VERSION, GET_CAP, ...) and the
 * GEM/PRIME import ioctls. Export, dumb buffers and mmap are not implemented,
 * so the DRM core rejects them.
 */
static const struct file_operations kbase_drm_fops = {
	.owner = THIS_MODULE,
#ifdef FOP_UNSIGNED_OFFSET
	/*
	 * The DRM core refuses to open a device whose fops do not allow
	 * unsigned file offsets (drm_open_helper() returns -EINVAL). The flag
	 * does not exist in older kernels, where the check is not made.
	 */
	.fop_flags = FOP_UNSIGNED_OFFSET,
#endif
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
};

/*
 * Called when the last reference to the DRM device is dropped, which can
 * happen after the platform device has been removed if file descriptors were
 * still open. The kbase device reference taken at registration time is
 * released here, so the kbase device memory is only freed once nobody can
 * reach it through the render node any more.
 */
static void kbase_drm_release(struct drm_device *drm)
{
	struct kbase_drm_device *priv = drm->dev_private;

	drm->dev_private = NULL;

	if (!priv)
		return;

	/*
	 * Normally a no-op here: the import context and every region were
	 * already released by kbase_drm_render_term() while the kbase device
	 * was still alive. It also covers the path where release happens
	 * without a preceding unplug.
	 */
	kbase_drm_gem_term(priv);

	mutex_destroy(&priv->import_lock);
	kbase_device_put(priv->kbdev);
	kfree(priv);
}

static const struct drm_driver kbase_drm_driver = {
	.driver_features = DRIVER_RENDER | DRIVER_GEM,
	.name = "mali-kbase",
	.desc = "ARM Mali GPU (kbase)",
#if (KERNEL_VERSION(6, 18, 0) > LINUX_VERSION_CODE)
	/* struct drm_driver lost its date field before 6.18. */
	.date = "20251113",
#endif
	.major = 1,
	.minor = 0,
	.fops = &kbase_drm_fops,
	.release = kbase_drm_release,
	.gem_prime_import = kbase_drm_gem_prime_import,
};

int kbase_drm_render_init(struct kbase_device *kbdev)
{
	struct kbase_drm_device *priv;
	struct drm_device *drm;
	int err;

	if (WARN_ON(kbdev->drm))
		return -EEXIST;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->kbdev = kbdev;
	mutex_init(&priv->import_lock);
	INIT_LIST_HEAD(&priv->gem_list);

	/*
	 * drm_dev_alloc() takes a reference on the parent device, so the
	 * platform device outlives the DRM device even if the two are torn down
	 * at different times.
	 */
	drm = drm_dev_alloc(&kbase_drm_driver, kbdev->dev);
	if (IS_ERR(drm)) {
		err = PTR_ERR(drm);
		mutex_destroy(&priv->import_lock);
		kfree(priv);
		return err;
	}

	/*
	 * The DRM device can outlive the platform device while file
	 * descriptors are open, so it owns a reference on the kbase device.
	 * kbase_drm_release() drops it when the DRM device is finally freed.
	 */
	drm->dev_private = priv;
	kbase_device_get(kbdev);

	err = drm_dev_register(drm, 0);
	if (err) {
		/* Drops the kbase device reference through kbase_drm_release(). */
		drm_dev_put(drm);
		return err;
	}

	kbdev->drm = drm;

	dev_info(kbdev->dev, "DRM render node %s registered\n",
		 dev_name(drm->render->kdev));

	return 0;
}

void kbase_drm_render_term(struct kbase_device *kbdev)
{
	struct drm_device *drm = kbdev->drm;

	if (!drm)
		return;

	kbdev->drm = NULL;

	/*
	 * Unplug rather than plain unregister: file descriptors opened before
	 * the device is torn down keep working on the DRM object (which is
	 * reference counted) but any further DRM ioctl returns -ENODEV, so an
	 * fd can never reach the kbase device after kbase_device_term(). The
	 * kbase device reference is dropped by kbase_drm_release() when the
	 * last DRM reference goes away.
	 */
	drm_dev_unplug(drm);

	/*
	 * Tear the imported buffers down while kbase is still fully alive:
	 * leaving the import context in place would make the later stages of
	 * kbase_device_term() run with a live context (timeline, memory and
	 * misc teardown all warn about that).
	 */
	kbase_drm_gem_term(drm->dev_private);

	drm_dev_put(drm);
}
