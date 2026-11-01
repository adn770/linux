/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sgxfive internals: the device state the driver's files share.
 */
#ifndef _SGXFIVE_PRIV_H_
#define _SGXFIVE_PRIV_H_

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/mutex.h>

#include "sgx_fw.h"

struct drm_device;
struct drm_printer;
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
	u32 cache_control;		/* SGXMKIF_CC_* for the next command */
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

/*
 * Buffer ids and HostCtl dwords, as resgx545's doc/specs/fw_format.md and
 * doc/specs/hw_spec.md give them.
 */
enum {
	BUF_CCB = 6, BUF_CCB_CTL = 7, BUF_KICKER = 8, BUF_TA3D_CTL = 9,
	BUF_HOST_CTL = 10, BUF_MISC = 11,
};

enum {
	HC_INIT_STATUS = 0, HC_POWER_STATUS = 1, HC_HWR_SAMPLE_RATE = 5,
	HC_UKERNEL_TIMER_CLOCK = 6, HC_APM_SAMPLE_RATE = 7, HC_HOST_CLOCK = 13,
	HC_DWORDS = 17,
};

/* Kernel CCB commands: SGXMKIF_CMD_*, the index into the kick table. */
#define SGX_CMD_POWER		3
#define SGX_CMD_GETMISCINFO	6

/* The POWER command's request and the HostCtl power status bit it sets. */
#define POWERCMD_POWEROFF	1
#define POWERCMD_IDLE		2
#define POWMAN_IDLE_COMPLETE	BIT(2)
#define POWMAN_POWEROFF_COMPLETE BIT(3)

static inline u32 *sgxfive_words(struct sgxfive *sgx, unsigned int id)
{
	return sgx->buf[id].va;
}

static inline u32 sgxfive_rd(struct sgxfive *sgx, u32 reg)
{
	return ioread32(sgx->regs + reg);
}

int sgxfive_command(struct sgxfive *sgx, unsigned int cmd, const u32 *data,
		    unsigned int ndata);
int sgxfive_wait_mem(const u32 *w, u32 mask, u32 want);
void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p);
int sgxfive_power(struct sgxfive *sgx, u32 powercmd, u32 done);

/* The debug group's files beyond regs (sgxfive_debug.c). */
void sgxfive_debug_init(struct sgxfive *sgx, struct dentry *dir);

#endif
