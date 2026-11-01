// SPDX-License-Identifier: GPL-2.0-only
/*
 * gma500's side of the sgxfive GPU driver: describe the SGX545 of a Cedarview
 * device to it and give it its debugfs directory.
 */
#include <drm/drm_file.h>

#include "gma_sgx.h"
#include "psb_drv.h"
#include "sgxfive/sgxfive.h"

void gma_sgx_init(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct sgxfive_host host = {
		.drm = dev,
		.regs = dev_priv->sgx_reg,
	};

	if (!IS_CDV(dev) || dev_priv->sgx)
		return;
	dev_priv->sgx = sgxfive_create(&host);
}

void gma_sgx_fini(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	sgxfive_destroy(dev_priv->sgx);
	dev_priv->sgx = NULL;
}

void gma_sgx_debugfs_init(struct drm_minor *minor)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(minor->dev);

	if (dev_priv->sgx)
		sgxfive_debugfs_init(dev_priv->sgx, minor->debugfs_root);
}
