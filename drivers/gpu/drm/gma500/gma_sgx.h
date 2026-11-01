/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gma500's side of the sgxfive GPU driver (Cedarview only).
 */
#ifndef _GMA500_GMA_SGX_H_
#define _GMA500_GMA_SGX_H_

#include <linux/types.h>

struct drm_device;
struct drm_minor;

void gma_sgx_init(struct drm_device *dev);
void gma_sgx_fini(struct drm_device *dev);
void gma_sgx_debugfs_init(struct drm_minor *minor);
bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2);

#endif
