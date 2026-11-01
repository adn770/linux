/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sgxfive internals: the device state the driver's files share.
 */
#ifndef _SGXFIVE_PRIV_H_
#define _SGXFIVE_PRIV_H_

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>

#include "sgx_fw.h"

struct drm_device;
struct firmware;
struct sgx_mmu_pd;

struct sgxfive_buf {
	struct page **pages;
	unsigned int npages;
	bool wc;		/* pages are set write-combined */
	void *va;
	u32 addr;		/* device virtual, once mapped */
	struct sgx_mmu_pd *pd;	/* the page directory it is mapped in */
	struct debugfs_blob_wrapper blob;
};

struct sgxfive {
	struct drm_device *dev;		/* for messages and debugfs */
	void __iomem *regs;		/* the SGX register block */
	u32 reset_pd;			/* the display side's PD, in list 0 */
	struct mutex lock;		/* serialises load and teardown */
	const struct firmware *blob;
	struct sgx_fw fw;
	struct sgx_fw_layout layout;
	struct sgx_mmu_pd *pd;		/* the EDM's page directory */
	struct sgxfive_buf buf[SGX_FW_MAX_BUFFERS];
	u32 heap_start;			/* first buffer's offset in each heap */
	bool loaded;
	bool live;			/* EDM booted: the IRQ events are ours */
	atomic_t sw_events;		/* EVENT_STATUS.SW_EVENT interrupts */
	atomic_t bif_faults;
};

/* What sgxfive_dump() shows: registers, HostCtl, TA3DCtl, CCB control. */
#define SGXFIVE_DUMP_NREGS	17
#define SGXFIVE_HC_DWORDS	17
#define SGXFIVE_TA3D_CTL_DWORDS	(0xec / 4)

struct sgxfive_devstate {
	bool loaded, live;
	int sw_events, bif_faults;
	struct { u32 reg, val; } regs[SGXFIVE_DUMP_NREGS];
	u32 hostctl[SGXFIVE_HC_DWORDS];
	u32 ta3dctl[SGXFIVE_TA3D_CTL_DWORDS];
	u32 ccbctl[2];
};

#endif
