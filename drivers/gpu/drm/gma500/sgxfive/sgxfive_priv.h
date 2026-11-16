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
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <drm/drm_mm.h>
#include <drm/gpu_scheduler.h>
#include <drm/sgxfive_drm.h>

#include "sgx_fw.h"
#include "sgx_mmu.h"
#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
#include "sgx_render.h"
#include "sgxfive_kpoke.h"
#include "sgxfive_owntask.h"
#endif

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
#define SGXFIVE_XBUFS	12

struct sgxfive_xtest {
	struct sgxfive_buf buf[SGXFIVE_XBUFS];
	u32 woff;
	u32 runs;
	struct sgx_mmu_pd *cpd;
	struct sgxfive_buf cbuf[2];
	bool alias_ro;		/* the EDM's buffers read-only in cpd */
	bool no_alias;		/* ...or not at all */
};

/*
 * The last render-context cleanup (debugfs render_cleanup): the TA and 3D
 * loaded-PB slots of TA3DCtl (dwords 46 and 47) before it and after each of
 * its RT, RC and PB steps, to see which step unloads the context's PB.
 */
#define SGXFIVE_TA3D_PB_TA	46
#define SGXFIVE_TA3D_PB_3D	47
#define SGXFIVE_CLEANUP_STEPS	4	/* before, after RT, RC, PB */

struct sgxfive_cleanup_trace {
	u32 rtds, ctx, pbdesc;		/* what was cleaned up */
	u32 flags;			/* the context's flags word at the start */
	int ret;
	unsigned int steps;		/* how many of slots[] were taken */
	u32 slots[SGXFIVE_CLEANUP_STEPS][2];
};

/* Which detector found a hang. */
enum sgxfive_hang {
	SGXFIVE_HANG_MANUAL,		/* a reset asked for (debugfs reset) */
	SGXFIVE_HANG_CLEANUP,		/* a render context cleanup unanswered */
	SGXFIVE_HANG_FAULT,		/* the BIF fault watchdog */
	SGXFIVE_HANG_TIMEOUT,		/* the drm_sched job timeout */
};

struct sgxfive_large;

/*
 * debugfs vm_pde_*: the one live large mapping and the regions freed ones
 * left, kept for reuse (a 4 MiB block may not be had again on a fragmented
 * machine) until the device goes.
 */
struct sgxfive_large_pool {
	struct mutex lock;
	struct sgxfive_large *live;
	struct list_head free;		/* head pages, page_private the order */
	u32 free_bytes;
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
	u32 core_id_sw, core_revision_sw, ddk_version;	/* read after sw_valid is set */
	bool ident_warned;		/* GETMISCINFO went unanswered, logged */

	/* Deferred release (sgxfive_defer), under lock. */
	u64 ccb_sent;			/* kernel-CCB commands submitted */
	struct list_head deferred;	/* in order of after */
	unsigned int ndeferred;
	struct delayed_work reaper;

	/* Kernel memory for render-node contexts (sgxfive_kmem_alloc). */
	struct drm_mm kmem;		/* offsets in the kernel data heap */
	unsigned int kmem_users;	/* live allocations: no reload */

	/* Render-node contexts, walked by the SW_EVENT interrupt. */
	spinlock_t ctx_lock;
	struct list_head ctx_list;
	struct drm_gpu_scheduler sched;	/* render-node jobs */
	u64 kicks;			/* jobs given to the firmware, under lock */
	u64 hang_oldest;		/* oldest job at the last timeout */
	struct delayed_work watchdog;	/* BIF fault poll while jobs run */
	u64 fault_suspect;		/* job blamed when a fault was last seen */
	u32 fault_reg, fault_pd;	/* that fault's BIF_FAULT, its directory */
	unsigned long coredump;		/* an error state is pending (bit 0) */
#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
	struct sgxfive_xtest xt;
	bool alias_ta3d_ro;		/* render VMs: TA3DCtl read-only */
	u32 alias_skip;			/* render VMs: firmware buffers (by id) left out */
	spinlock_t kpoke_lock;		/* pokes for the next jobs' commands */
	struct sgxfive_kpokes kpokes;
	u32 render_pb_pages;		/* PB of new render contexts */
	u32 render_final_pb_pages;	/* the next one's final kicks' PB, once */
	u32 render_rgn_arrays;		/* region-header arrays of new render contexts */
	u32 render_tail_room;		/* tail-pointer bytes of new render contexts */
	bool render_bg_layer;		/* renders: a one-layer background object */
	u32 render_bg_src;		/* ... its source, mapped in the render's VM */
	bool vm_low;			/* VM_MAP also takes the first MiB */
	u32 vm_pte_flags;		/* VM_MAP: PTE flags of the mapping at vm_pte_addr */
	u32 vm_pte_addr;		/* ... 0: off */
	u32 vm_pde_addr;		/* VM_MAP: the 4 MiB slot mapped large, 0: off */
	u32 vm_pde_size;		/* ... its PDE's size code */
	u32 vm_pde_layout;		/* ... and its table's layout */
	struct sgxfive_large_pool large;
	u32 render_ctx_flags;		/* new render contexts' extra flags */
	struct sgxfive_cleanup_trace cleanup;	/* the last one's, under lock */
	u32 timer_clock;		/* HostCtl's timer clock at boot, 0: default */
	bool hold_recovery;		/* detectors report hangs, do not reset */
	bool recovery_held;		/* one is held, under lock (logged once) */
	u64 held_suspect;		/* its job, which the next reset blames */
	enum sgxfive_hang held_why;	/* and the detector that found it */
	bool rtds_keep;			/* renders: no RT data set rewrite */
	bool render_share_pb;		/* the next render context shares a PB, once */
	bool alias_ctx_rw;		/* the next render context's page writable, once */
	bool render_keep;		/* the next render context closed is kept, once */
	struct sgxfive_keep kept;	/* the kept ones (debugfs render_keep) */
	bool kick_no_bump;		/* the next command leaves the kicker word, once */
	u32 event_kicks;		/* EVENT_KICK writes since the kicker word was zeroed */
	u32 peek_off;			/* debugfs peek's register */
	struct sgxfive_tdfill tdfill;	/* the next render's TA data page fill */
	struct sgxfive_hostirq hostirq;	/* host interrupts, kregnow's enables */
	struct sgxfive_own own;		/* our own firmware task: blob and arming */
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
	HC_INIT_STATUS = 0, HC_POWER_STATUS = 1, HC_CLEANUP_STATUS = 2,
	HC_HWR_SAMPLE_RATE = 5,
	HC_UKERNEL_TIMER_CLOCK = 6, HC_APM_SAMPLE_RATE = 7, HC_HOST_CLOCK = 13,
	HC_PERF_GROUP = 15,
	HC_DWORDS = 17,
};

/* Kernel CCB commands: SGXMKIF_CMD_*, the index into the kick table. */
#define SGX_CMD_TA		0
#define SGX_CMD_TRANSFER	1
#define SGX_CMD_POWER		3
#define SGX_CMD_CLEANUP		5
#define SGX_CMD_GETMISCINFO	6
#define SGX_CMD_PROCESS_QUEUES	7
#define SGX_CMD_SETHWPERFSTATUS	9

/* Cache control bits a command carries: SGXMKIF_CC_INVAL_*. */
#define SGX_CC_INVAL_BIF	0x3		/* page tables and directory */
#define SGX_CC_INVAL_DATA	0x8

/* The POWER command's request and the HostCtl power status bit it sets. */
#define POWERCMD_POWEROFF	1
#define POWERCMD_IDLE		2
#define POWMAN_IDLE_COMPLETE	BIT(2)
#define POWMAN_POWEROFF_COMPLETE BIT(3)

/*
 * GETMISCINFO's request and its reply in the misc-info buffer (BUF_MISC): the
 * request in dword 0, which the EDM sets READY when it has filled in the
 * rest, the reply's features after it, then the sizes of its structures.
 */
#define MISCINFO_READY			0x1
#define MISCINFO_GET_STRUCT_SIZES	0x2
#define MISCINFO_FEATURES		7
#define MISCINFO_STRUCT_SIZES		13
#define MISCINFO_DWORDS		(1 + MISCINFO_FEATURES + MISCINFO_STRUCT_SIZES)
/* The features' dwords: the core it runs on, the core and DDK it was built for. */
#define MISCINFO_CORE_REV		1
#define MISCINFO_CORE_ID		2
#define MISCINFO_DDK_VERSION		3
#define MISCINFO_DDK_BUILD		4
#define MISCINFO_CORE_ID_SW		5
#define MISCINFO_CORE_REV_SW		6
#define MISCINFO_BUILD_OPTIONS		7
/* The core the firmware is built for. */
#define SGXFIVE_CORE_ID_SW		545

/* The CLEANUP command's object types, and the HostCtl status bit it sets. */
#define CLEANUP_RT		1	/* an RT data set */
#define CLEANUP_RC		2	/* a HW render context */
#define CLEANUP_PB		5	/* a PB descriptor */
#define CLEANUP_COMPLETE	BIT(0)	/* in HostCtl's cleanup status */

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
		   struct sgxfive_buf *b, u32 addr, u32 size, u32 flags);
int sgxfive_command(struct sgxfive *sgx, unsigned int cmd, const u32 *data,
		    unsigned int ndata);
/* Zero the kicker word before a kick sequence (C7); the debug group's count of kicks with it. */
void sgxfive_kicker_zero(struct sgxfive *sgx);
int sgxfive_wait_mem(const u32 *w, u32 mask, u32 want);
void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p);
void sgxfive_devstate(struct sgxfive *sgx, struct sgxfive_devstate *d);
void sgxfive_devstate_print(struct drm_printer *p, const struct sgxfive_devstate *d);
int sgxfive_power(struct sgxfive *sgx, u32 powercmd, u32 done);
bool sgxfive_ident_core_ok(const struct sgxfive *sgx, const u32 *mi);
unsigned int sgxfive_ident_result(struct sgxfive *sgx, int ret, const u32 *mi);
void sgxfive_ident_clear(struct sgxfive *sgx);
int sgxfive_ident_ask(struct sgxfive *sgx, s64 *us);
void sgxfive_gpu_info(struct sgxfive *sgx, struct drm_sgxfive_dev_query_gpu_info *g);
u32 sgxfive_ddk_version(struct sgxfive *sgx);

struct sgxfive_vm;
struct sgxfive_client;

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
	struct xarray ctxs;		/* handle -> struct sgxfive_ctx */
	struct sgxfive_client *client;	/* its fault budget */
};

struct sgxfive_vm *sgxfive_vm_get(struct sgxfive_file *f, u32 handle);
void sgxfive_vm_put(struct sgxfive_vm *vm);
u32 sgxfive_vm_pd_phys(const struct sgxfive_vm *vm);
u32 sgxfive_vm_lookup(struct sgxfive_vm *vm, u32 addr);

/* The client heap layout and its query (sgxfive_drv.c). */
extern const struct drm_sgxfive_heap sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_COUNT];
int sgxfive_query_heaps(struct drm_sgxfive_dev_query_args *a);

bool sgxfive_is_bo(struct drm_gem_object *obj);
void sgxfive_need_bif_inval(struct sgxfive *sgx);
int sgxfive_kmem_alloc(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b, u32 size);
void sgxfive_kmem_free(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b);
int sgxfive_ensure_live(struct sgxfive *sgx);
void sgxfive_fences_irq(struct sgxfive *sgx);
int sgxfive_kick_transfer(struct sgxfive *sgx, u32 hw_ctx_addr);
int sgxfive_sched_init(struct sgxfive *sgx);
void sgxfive_sched_fini(struct sgxfive *sgx);

u64 sgxfive_oldest(struct sgxfive *sgx);
int sgxfive_recover(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why);
int __sgxfive_recover(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why,
		      bool *dropped);
int sgxfive_hang_error(enum sgxfive_hang why);
void sgxfive_watchdog_arm(struct sgxfive *sgx);

u32 sgxfive_job_build(u8 *cmd, const struct drm_sgxfive_transfer_cmd *regs,
		      bool dummy, u32 status_addr, u32 value);
int sgxfive_job_check(const struct drm_sgxfive_transfer_cmd *regs);
struct sgxfive_ctx;
void sgxfive_submit_xfer_pokes(struct sgxfive_ctx *ctx, u8 *cmd, bool dummy);
int sgxfive_ctx_check_args(const struct drm_sgxfive_create_context_args *a);

struct sgx_mmu_pd;
struct sgx_render_addrs;
struct sgx_render_tadata;
int sgxfive_render_check(const struct drm_sgxfive_render_cmd *c, u32 max_w,
			 u32 max_h, bool ms);
int sgxfive_render_check_bg(bool layer, u32 src, u32 pte);
void sgxfive_render_addrs(struct sgx_render_addrs *a, u32 mem, u32 pd_phys,
			  unsigned int slot, const struct sgx_render_tadata *td);
u32 sgxfive_render_build(u8 *img, const struct sgx_render_addrs *a,
			 const struct drm_sgxfive_render_cmd *c, u32 seqno, bool first,
			 bool last);
int sgxfive_kick_render(struct sgxfive *sgx, u32 hw_ctx_addr);
/* A render context's CLEANUP commands, in order (sgxfive_render_cleanup_steps). */
#define SGXFIVE_CLEANUP_CMDS	3

struct sgxfive_cleanup_step {
	u32 type;			/* CLEANUP_RT, CLEANUP_RC or CLEANUP_PB */
	u32 addr;
};

unsigned int sgxfive_render_cleanup_steps(u32 rtds, u32 ctx, u32 pbdesc,
					  struct sgxfive_cleanup_step *step);
int sgxfive_render_cleanup(struct sgxfive *sgx, u32 rtds, u32 ctx, u32 pbdesc,
			   u32 flags);
int sgxfive_alias_firmware(struct sgxfive *sgx, struct sgx_mmu_pd *pd, bool map);
int sgxfive_alias_flags(unsigned int id, bool ta3d_ro, u32 skip);

int sgxfive_vm_check_range(const struct drm_sgxfive_heap *heaps, unsigned int n,
			   u64 addr, u64 size);
int sgxfive_vm_check_low(u64 addr, u64 size);

/* The PTE flags debugfs vm_pte_flags may add: WRITEONLY, CACHECONSISTENT, EDMPROTECT. */
#define SGXFIVE_VM_PTE_KNOB	(SGX_MMU_WO | SGX_MMU_CACHED | SGX_MMU_EDMPROT)
u32 sgxfive_vm_pte_flags(bool ro, u32 addr, u32 knob_addr, u32 knob_flags);
int sgxfive_vm_large_wanted(u64 addr, u64 size, u32 knob_addr, unsigned int code,
			    unsigned int layout);
int sgxfive_vm_render_attach(struct sgxfive_vm *vm, unsigned int *slot);
void sgxfive_vm_render_detach(struct sgxfive_vm *vm, unsigned int slot);
int sgxfive_vm_map_pages(struct sgxfive_vm *vm, u32 addr, struct page **pages,
			 unsigned int n, u32 flags);
void sgxfive_vm_unmap_pages(struct sgxfive_vm *vm, u32 addr, unsigned int n);
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
bool sgxfive_debug_reset(struct sgxfive *sgx);
void sgxfive_debug_disarm(struct sgxfive *sgx);
int sgxfive_debug_own_apply(struct sgxfive *sgx, u8 *const bufs[]);
void sgxfive_debug_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2);
int sgxfive_alias_skip_set(void *data, u64 val);
int sgxfive_vm_pte_flags_set(void *data, u64 val);
int sgxfive_vm_pte_addr_set(void *data, u64 val);
int sgxfive_vm_pde_addr_set(void *data, u64 val);
int sgxfive_vm_pde_size_set(void *data, u64 val);
int sgxfive_vm_pde_layout_set(void *data, u64 val);
void sgxfive_vm_large_init(struct sgxfive *sgx);
void sgxfive_vm_large_fini(struct sgxfive *sgx);
struct seq_file;
int sgxfive_vm_large_show(struct seq_file *m, struct sgxfive *sgx);
int sgxfive_final_pb_set(void *data, u64 val);

static inline bool sgxfive_alias_ta3d_ro(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->alias_ta3d_ro);
}

/*
 * The firmware buffers a new render VM's alias leaves out, by id (debugfs
 * alias_skip).
 */
static inline u32 sgxfive_alias_skip(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->alias_skip);
}

/* Whether the next render context maps its page writable: taken once (debugfs alias_ctx_rw). */
static inline bool sgxfive_take_ctx_rw(struct sgxfive *sgx)
{
	return sgxfive_oneshot_take(&sgx->alias_ctx_rw);
}

/* Whether the render context closed next is kept: taken once (debugfs render_keep). */
static inline bool sgxfive_take_render_keep(struct sgxfive *sgx)
{
	return sgxfive_oneshot_take(&sgx->render_keep);
}

/*
 * The PB whose thresholds the next render context's final kicks after a geometry kick take:
 * taken once (debugfs render_final_pb_pages), 0 for none.
 */
static inline u32 sgxfive_take_final_pb(struct sgxfive *sgx)
{
	return xchg(&sgx->render_final_pb_pages, 0);
}

/* Whether the next render context runs on a sibling's PB: taken once (debugfs render_share_pb). */
static inline bool sgxfive_take_share_pb(struct sgxfive *sgx)
{
	return sgxfive_oneshot_take(&sgx->render_share_pb);
}

/* Whether the next command leaves the kicker word as it is: taken once (debugfs kick_no_bump). */
static inline bool sgxfive_take_kick_no_bump(struct sgxfive *sgx)
{
	return sgxfive_oneshot_take(&sgx->kick_no_bump);
}

/* A command that took it was not sent: the next one takes it. */
static inline void sgxfive_kick_no_bump_again(struct sgxfive *sgx)
{
	WRITE_ONCE(sgx->kick_no_bump, true);
}

/* One more EVENT_KICK write, and the count's restart with the kicker word (debugfs kicker). */
static inline void sgxfive_note_kick(struct sgxfive *sgx)
{
	WRITE_ONCE(sgx->event_kicks, sgx->event_kicks + 1);
}

static inline void sgxfive_kicks_zero(struct sgxfive *sgx)
{
	WRITE_ONCE(sgx->event_kicks, 0);
}

/* 0 or out of range: the default (sgx_render_pb_pages()). */
static inline u32 sgxfive_render_pb_pages(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->render_pb_pages);
}

/*
 * debugfs render_rgn_arrays: 1, 2 or 4 forced; 0, the default, and any other
 * value: 0, automatic (sgx_render_rgn_arrays()).
 */
static inline u32 sgxfive_render_rgn_arrays(struct sgxfive *sgx)
{
	u32 n = READ_ONCE(sgx->render_rgn_arrays);

	return sgx_render_rgn_arrays_ok(n) ? n : 0;
}

/*
 * debugfs render_tail_room: the setting as written; sgx_render_tail_room()
 * takes a whole number of pages, not 0, and any other value as automatic.
 */
static inline u32 sgxfive_render_tail_room(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->render_tail_room);
}

/*
 * Whether a render submitted now has a one-layer background object (debugfs
 * render_bg_layer), and at *src its source (debugfs render_bg_src, 0: none).
 */
static inline bool sgxfive_render_bg_layer(struct sgxfive *sgx, u32 *src)
{
	*src = READ_ONCE(sgx->render_bg_src);
	return READ_ONCE(sgx->render_bg_layer);
}

static inline bool sgxfive_vm_low(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->vm_low);
}

/* debugfs vm_pte_flags, and at *addr vm_pte_addr: the mapping they apply to (0: none). */
static inline u32 sgxfive_vm_pte_knob(struct sgxfive *sgx, u32 *addr)
{
	*addr = READ_ONCE(sgx->vm_pte_addr);
	return READ_ONCE(sgx->vm_pte_flags);
}

/* debugfs vm_pde_addr (0: off), and its size code and layout. */
static inline u32 sgxfive_vm_pde_knob(struct sgxfive *sgx, unsigned int *code,
				      unsigned int *layout)
{
	*code = READ_ONCE(sgx->vm_pde_size);
	*layout = READ_ONCE(sgx->vm_pde_layout);
	return READ_ONCE(sgx->vm_pde_addr);
}

static inline u32 sgxfive_render_ctx_flags(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->render_ctx_flags);
}

void sgxfive_cleanup_start(struct sgxfive_cleanup_trace *t, u32 rtds, u32 ctx,
			   u32 pbdesc, u32 flags);
void sgxfive_cleanup_note(struct sgxfive_cleanup_trace *t, const u32 *ta3dctl);
void sgxfive_cleanup_print(struct drm_printer *p, const struct sgxfive_cleanup_trace *t);

/* Under the device lock, around a render-context cleanup's steps. */
static inline void sgxfive_cleanup_trace_start(struct sgxfive *sgx, u32 rtds,
					       u32 ctx, u32 pbdesc, u32 flags)
{
	sgxfive_cleanup_start(&sgx->cleanup, rtds, ctx, pbdesc, flags);
}

static inline void sgxfive_cleanup_trace_note(struct sgxfive *sgx)
{
	sgxfive_cleanup_note(&sgx->cleanup, sgxfive_words(sgx, BUF_TA3D_CTL));
}

static inline void sgxfive_cleanup_trace_end(struct sgxfive *sgx, int ret)
{
	sgx->cleanup.ret = ret;
}

/* HostCtl's timer clock for the next boot: the debugfs value, if set. */
static inline u32 sgxfive_timer_clock(struct sgxfive *sgx, u32 dflt)
{
	u32 v = READ_ONCE(sgx->timer_clock);

	return v ? v : dflt;
}

bool sgxfive_recovery_hold(struct sgxfive *sgx, u64 *suspect, enum sgxfive_hang *why);

/* Whether a recovery is held: the detectors have reported its hang already. */
static inline bool sgxfive_recovery_held(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->recovery_held);
}

/* Whether a render leaves its RT data set as it is (debugfs rtds_keep). */
static inline bool sgxfive_rtds_keep(struct sgxfive *sgx)
{
	return READ_ONCE(sgx->rtds_keep);
}
#else
static inline int sgxfive_debug_load(struct sgxfive *sgx)
{
	return 0;
}

static inline void sgxfive_debug_unload(struct sgxfive *sgx)
{
}

static inline bool sgxfive_debug_reset(struct sgxfive *sgx)
{
	return false;
}

static inline int sgxfive_debug_own_apply(struct sgxfive *sgx, u8 *const bufs[])
{
	return 0;
}

static inline void sgxfive_debug_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2)
{
}

static inline bool sgxfive_alias_ta3d_ro(struct sgxfive *sgx)
{
	return false;
}

static inline u32 sgxfive_alias_skip(struct sgxfive *sgx)
{
	return 0;
}

static inline void sgxfive_kpoke_job(struct sgxfive *sgx, u8 *img, bool render)
{
}

static inline void sgxfive_kpoke_run(struct sgxfive *sgx, u32 *const words[])
{
}

static inline void sgxfive_kpoke_xfer(struct sgxfive *sgx, u32 *hw)
{
}

static inline u32 sgxfive_render_pb_pages(struct sgxfive *sgx)
{
	return 0;
}

static inline u32 sgxfive_render_rgn_arrays(struct sgxfive *sgx)
{
	return 0;
}

static inline bool sgxfive_render_bg_layer(struct sgxfive *sgx, u32 *src)
{
	*src = 0;
	return false;
}

static inline u32 sgxfive_render_tail_room(struct sgxfive *sgx)
{
	return 0;
}

static inline bool sgxfive_take_ctx_rw(struct sgxfive *sgx)
{
	return false;
}

static inline bool sgxfive_take_render_keep(struct sgxfive *sgx)
{
	return false;
}

static inline u32 sgxfive_take_final_pb(struct sgxfive *sgx)
{
	return 0;
}

static inline bool sgxfive_take_share_pb(struct sgxfive *sgx)
{
	return false;
}

static inline bool sgxfive_take_kick_no_bump(struct sgxfive *sgx)
{
	return false;
}

static inline void sgxfive_kick_no_bump_again(struct sgxfive *sgx)
{
}

static inline void sgxfive_note_kick(struct sgxfive *sgx)
{
}

static inline void sgxfive_kicks_zero(struct sgxfive *sgx)
{
}

static inline u32 sgxfive_ctx_page_flags(bool rw)
{
	return SGX_MMU_RO;
}

static inline bool sgxfive_vm_low(struct sgxfive *sgx)
{
	return false;
}

static inline u32 sgxfive_vm_pte_knob(struct sgxfive *sgx, u32 *addr)
{
	*addr = 0;
	return 0;
}

static inline u32 sgxfive_vm_pde_knob(struct sgxfive *sgx, unsigned int *code,
				      unsigned int *layout)
{
	*code = 0;
	*layout = 0;
	return 0;
}

static inline void sgxfive_vm_large_init(struct sgxfive *sgx)
{
}

static inline void sgxfive_vm_large_fini(struct sgxfive *sgx)
{
}

static inline u32 sgxfive_render_ctx_flags(struct sgxfive *sgx)
{
	return 0;
}

static inline void sgxfive_cleanup_trace_start(struct sgxfive *sgx, u32 rtds,
					       u32 ctx, u32 pbdesc, u32 flags)
{
}

static inline void sgxfive_cleanup_trace_note(struct sgxfive *sgx)
{
}

static inline void sgxfive_cleanup_trace_end(struct sgxfive *sgx, int ret)
{
}

static inline u32 sgxfive_timer_clock(struct sgxfive *sgx, u32 dflt)
{
	return dflt;
}

static inline bool sgxfive_recovery_hold(struct sgxfive *sgx, u64 *suspect,
					 enum sgxfive_hang *why)
{
	return false;
}

static inline bool sgxfive_recovery_held(struct sgxfive *sgx)
{
	return false;
}

static inline bool sgxfive_rtds_keep(struct sgxfive *sgx)
{
	return false;
}
#endif

#endif
