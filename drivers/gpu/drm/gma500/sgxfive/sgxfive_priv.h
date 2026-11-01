/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sgxfive internals: the device state the driver's files share, the device
 * code (sgxfive.c) and the render-node interface (sgxfive_drv.c and the
 * files it uses).
 */
#ifndef _SGXFIVE_PRIV_H_
#define _SGXFIVE_PRIV_H_

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <drm/sgxfive_drm.h>

#include "sgx_fw.h"

struct drm_device;
struct drm_file;
struct drm_gem_object;
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

/*
 * The debug group's transfer-queue test area (sgxfive_debug.c): buffers in
 * the EDM's address space, the client CCB's write offset and the number of
 * transfers queued since the load; a client page directory with buffers of
 * its own, and how it maps the EDM's buffers.
 */
#define SGXFIVE_XBUFS	10

struct sgxfive_xtest {
	struct sgxfive_buf buf[SGXFIVE_XBUFS];
	u32 woff;
	u32 runs;
	struct sgx_mmu_pd *cpd;
	struct sgxfive_buf cbuf[2];
	bool alias_ro;		/* the EDM's buffers read-only in cpd */
	bool no_alias;		/* ...or not at all */
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
	wait_queue_head_t event_wq;	/* woken on each SW_EVENT */
	atomic_t bif_faults;

	/* What DEV_QUERY reports. */
	u32 core_id, core_revision;	/* read at probe */
	u32 host_bridge_rev;
	bool sw_valid;			/* the firmware's own values, below */
	u32 core_id_sw, core_revision_sw, ddk_version;

	/* Deferred release (sgxfive_defer), under lock. */
	u64 ccb_sent;			/* kernel-CCB commands submitted */
	struct list_head deferred;	/* in order of after */
	unsigned int ndeferred;
	struct delayed_work reaper;
#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
	struct sgxfive_xtest xt;
#endif
};

/* What sgxfive_dump() shows: registers, HostCtl, TA3DCtl, CCB control. */
#define SGXFIVE_DUMP_NREGS	17
#define SGXFIVE_HC_DWORDS	17
#define SGXFIVE_TA3D_CTL_DWORDS	(0xec / 4)

struct sgxfive_devstate {
	bool loaded, live;
	int sw_events, bif_faults;
	u64 ccb_sent;
	unsigned int ndeferred;
	struct { u32 reg, val; } regs[SGXFIVE_DUMP_NREGS];
	u32 hostctl[SGXFIVE_HC_DWORDS];
	u32 ta3dctl[SGXFIVE_TA3D_CTL_DWORDS];
	u32 ccbctl[2];
};

/*
 * The GPU address layout's heaps, by sgxapi_km.h heap id (the bases are in
 * sgxfive.c).
 */
enum {
	HEAP_KERNEL_CODE = 2,
	HEAP_KERNEL_DATA = 3,
	HEAP_PIXELSHADER = 4,
	HEAP_VERTEXSHADER = 5,
	HEAP_PDSPIXEL = 6,
	HEAP_TEXTURE = 14,	/* SGX_TEXTURE_HEAP_ID */
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
#define SGX_CMD_TRANSFER	1
#define SGX_CMD_POWER		3
#define SGX_CMD_GETMISCINFO	6
#define SGX_CMD_PROCESS_QUEUES	7

/* Cache control bits a command carries: SGXMKIF_CC_INVAL_*. */
#define SGX_CC_INVAL_BIF	0x3		/* page tables and directory */
#define SGX_CC_INVAL_DATA	0x8

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

void sgxfive_unmap(struct sgxfive *sgx, struct sgxfive_buf *b);
int sgxfive_map_pd(struct sgxfive *sgx, struct sgx_mmu_pd *pd,
		   struct sgxfive_buf *b, u32 addr, u32 size);
int sgxfive_command(struct sgxfive *sgx, unsigned int cmd, const u32 *data,
		    unsigned int ndata);
int sgxfive_wait_mem(const u32 *w, u32 mask, u32 want);
void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p);
int sgxfive_power(struct sgxfive *sgx, u32 powercmd, u32 done);

struct sgxfive_vm;

/*
 * Memory the GPU may still reach through stale BIF state: page directories,
 * page tables and pinned BO pages. Freed by free() once kernel-CCB command
 * number after, which carries the BIF invalidation, has been processed.
 */
struct sgxfive_deferred {
	struct list_head link;
	u64 after;
	void (*free)(struct sgxfive_deferred *d);
};

u64 sgxfive_ccb_done(u64 sent, u32 woff, u32 roff);
unsigned int sgxfive_reap(struct list_head *list, u64 done, struct list_head *due);
void sgxfive_free_all(struct list_head *due);
void sgxfive_defer(struct sgxfive *sgx, struct sgxfive_deferred *d);

/* Per-open-file state of the render node. */
struct sgxfive_file {
	struct xarray vms;		/* handle -> struct sgxfive_vm */
};

struct sgxfive_vm *sgxfive_vm_get(struct sgxfive_file *f, u32 handle);
void sgxfive_vm_put(struct sgxfive_vm *vm);
u32 sgxfive_vm_pd_phys(const struct sgxfive_vm *vm);

/* The client heap layout and its query (sgxfive_drv.c). */
extern const struct drm_sgxfive_heap sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_COUNT];
int sgxfive_query_heaps(struct drm_sgxfive_dev_query_args *a);

bool sgxfive_is_bo(struct drm_gem_object *obj);
void sgxfive_need_bif_inval(struct sgxfive *sgx);

int sgxfive_vm_check_range(const struct drm_sgxfive_heap *heaps, unsigned int n,
			   u64 addr, u64 size);
int sgxfive_ioctl_create_vm(struct sgxfive *sgx, void *data,
			    struct drm_file *file);
int sgxfive_ioctl_destroy_vm(struct sgxfive *sgx, void *data,
			     struct drm_file *file);
int sgxfive_ioctl_vm_map(struct sgxfive *sgx, void *data, struct drm_file *file);
int sgxfive_ioctl_vm_unmap(struct sgxfive *sgx, void *data,
			   struct drm_file *file);

/* The debug group's files beyond regs, and its test area (sgxfive_debug.c). */
#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
void sgxfive_debug_init(struct sgxfive *sgx, struct dentry *dir);
int sgxfive_debug_load(struct sgxfive *sgx);
void sgxfive_debug_unload(struct sgxfive *sgx);
#else
static inline int sgxfive_debug_load(struct sgxfive *sgx)
{
	return 0;
}

static inline void sgxfive_debug_unload(struct sgxfive *sgx)
{
}
#endif

#endif
