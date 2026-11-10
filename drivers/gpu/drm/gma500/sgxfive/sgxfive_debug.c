// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The SGX545 debug group's checks on the running firmware
 * (CONFIG_DRM_GMA500_SGXFIVE_DEBUG): each debugfs file sends the EDM work
 * through its kernel CCB and checks what comes back, printing one line per
 * value. They exist to test the firmware interface on the hardware; the
 * driver itself needs none of them.
 *
 * The transfer checks run on a test area mapped next to the firmware's
 * buffers at load: one client CCB and HW transfer context, the targets of
 * their status and update writes. Transfers are serialised by the device
 * lock, one in flight at a time.
 */
#include <crypto/sha2.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/lockdep.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <drm/drm_print.h>

#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"
#include "sgx_mmu.h"
#include "sgx_render.h"
#include "sgx_seq.h"
#include "sgx_xfer.h"
#include "sgxfive_owntask.h"

/*
 * The test area, in KERNEL_DATA above the firmware's buffers so the EDM's
 * own page directory maps it: a 64 KiB client CCB (plus room for a command
 * at its end), its control block, one HW transfer context and a scratch
 * page: U (update target), F (status target) and, from SCR_SYNC_READ, the
 * read- and write-complete counters of a destination sync. For a render
 * there are also a page of PDS code at the PDS heap base, a page of USE code
 * at the pixel-shader code base, the background object, a destination of
 * two pages plus a guard page and, in the texture heap, a copy's source.
 */
enum { SCR_U = 0, SCR_F = 1, SCR_SYNC_READ = 8, SCR_SYNC_WRITE = 9 };
enum {
	XB_CCB, XB_CCB_CTL, XB_CTX, XB_SCRATCH, XB_PDS, XB_USE, XB_BG, XB_DST,
	XB_SRC, XB_OWNCODE, XB_OWNRES, XB_COUNT
};

/* By heap and offset, so the area follows the layout. */
static const struct {
	u8 heap;
	u32 off, size;
	u32 flags;			/* the EDM's page flags: SGX_MMU_RO, or 0 */
} sgxfive_xbuf[XB_COUNT] = {
	[XB_CCB] = { HEAP_KERNEL_DATA, 0x100000,
		     SGX_XFER_CCB_SIZE + SGX_XFER_CMD_SIZE },
	[XB_CCB_CTL] = { HEAP_KERNEL_DATA, 0x120000, 8 },
	[XB_CTX] = { HEAP_KERNEL_DATA, 0x121000, SGX_XFER_CTX_SIZE },
	[XB_SCRATCH] = { HEAP_KERNEL_DATA, 0x122000, 0x40 },
	[XB_PDS] = { HEAP_PDSPIXEL, 0, 0x1000 },	/* the PDS code base */
	[XB_USE] = { HEAP_PIXELSHADER, 0, 0x1000 },	/* the USE code base */
	[XB_BG] = { HEAP_KERNEL_DATA, 0x130000, 4 * SGX_XFER_BGOBJ1_DWORDS },
	[XB_DST] = { HEAP_KERNEL_DATA, 0x200000, 0x3000 },
	[XB_SRC] = { HEAP_TEXTURE, 0x300000, 0x2000 },
	/* our own firmware task (sgxfive_owntask.h): its code and its results page */
	/*
	 * The code is read-only for the EDM (the CPU alias stays writable): a store of the task
	 * into its own page could write a word the verifier never saw. What the task can store
	 * into otherwise is the results page only, by the verifier's base check (sgxfive_owntask.c).
	 */
	[XB_OWNCODE] = { HEAP_KERNEL_CODE, SGX_OWN_CODE_OFF, SGX_OWN_CODE_SIZE, SGX_MMU_RO },
	[XB_OWNRES] = { HEAP_KERNEL_DATA, SGX_OWN_RES_OFF, SGX_OWN_RES_SIZE, 0 },
};

static_assert(XB_COUNT <= SGXFIVE_XBUFS);

/*
 * What a tester armed for the firmware that ran does not outlive it: a boot,
 * a firmware reload or a recovery reset starts with nothing armed.
 * The own task's arming (own_arm) is not among them: it is set before a load and has to
 * live through the load's disarm; a recovery reset clears it itself (sgxfive_debug_reset).
 */
void sgxfive_debug_disarm(struct sgxfive *sgx)
{
	sgxfive_hostirq_reset(&sgx->hostirq);
	WRITE_ONCE(sgx->alias_ctx_rw, false);
	WRITE_ONCE(sgx->render_share_pb, false);
	WRITE_ONCE(sgx->render_keep, false);
	WRITE_ONCE(sgx->kick_no_bump, false);
	spin_lock(&sgx->kpoke_lock);
	sgx->tdfill.armed = false;
	sgx->kpokes.n = 0;
	spin_unlock(&sgx->kpoke_lock);
}

int sgxfive_debug_load(struct sgxfive *sgx)
{
	unsigned int i;
	int ret;

	sgxfive_debug_disarm(sgx);
	for (i = 0; i < XB_COUNT; i++) {
		ret = sgxfive_map_pd(sgx, sgx->pd, &sgx->xt.buf[i],
				     sgx->layout.heap_base[sgxfive_xbuf[i].heap] +
				     sgxfive_xbuf[i].off, sgxfive_xbuf[i].size,
				     sgxfive_xbuf[i].flags);
		if (ret)
			return ret;
	}
	return 0;
}

void sgxfive_debug_unload(struct sgxfive *sgx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sgx->xt.cbuf); i++)
		sgxfive_unmap(sgx, &sgx->xt.cbuf[i]);
	if (sgx->xt.cpd)
		sgx_mmu_pd_destroy(sgx->xt.cpd);	/* also drops its aliases */
	sgx->xt.cpd = NULL;
	for (i = 0; i < XB_COUNT; i++)
		sgxfive_unmap(sgx, &sgx->xt.buf[i]);
	sgx->xt.woff = 0;
	sgx->xt.runs = 0;	/* the next transfer initialises the context */
}

static void *xb_va(struct sgxfive *sgx, unsigned int i)
{
	return sgx->xt.buf[i].va;
}

static u32 xb_addr(struct sgxfive *sgx, unsigned int i)
{
	return sgx->xt.buf[i].addr;
}

static void sgxfive_check(struct drm_printer *p, const char *what, u32 got,
			  u32 want, int *bad)
{
	drm_printf(p, "%-16s %#010x %s\n", what, got,
		   got == want ? "ok" : "MISMATCH");
	if (got != want) {
		drm_printf(p, "%-16s expected %#010x\n", "", want);
		(*bad)++;
	}
}

static const char *const sgxfive_feature_names[MISCINFO_FEATURES] = {
	"core rev", "core id", "ddk version", "ddk build", "core id sw",
	"core rev sw", "build options",
};

/*
 * GETMISCINFO with GET_STRUCT_SIZES, as the boot asks it (sgxfive_ident_ask):
 * the EDM fills in the core and DDK identity and the sizes of the structures
 * it was built with. Check every value against the registers and the
 * firmware file.
 */
static int sgxfive_miscinfo(struct sgxfive *sgx, struct drm_printer *p)
{
	u32 *mi = sgxfive_words(sgx, BUF_MISC);
	u32 want[MISCINFO_FEATURES];
	s64 us;
	int i, ret, bad = 0;

	ret = sgxfive_ident_ask(sgx, &us);
	if (ret == -ETIMEDOUT) {
		drm_printf(p, "no MISCINFO_READY after 500 ms\n");
		sgxfive_dump(sgx, p);
	}
	if (ret)
		return ret;
	drm_printf(p, "GETMISCINFO answered after %lld us\n", us);

	want[0] = sgxfive_rd(sgx, SGX_CORE_REVISION);
	want[1] = sgxfive_rd(sgx, SGX_CORE_ID);
	/* major, minor and branch: DDK 1.7 is branch 17 */
	want[2] = sgx->fw.ddk_major << 16 | sgx->fw.ddk_minor << 8 | 17;
	want[3] = sgx->fw.ddk_build;
	want[4] = SGXFIVE_CORE_ID_SW;
	want[5] = want[0];		/* the revision the EDM was built for */
	want[6] = sgx->fw.build_options;
	for (i = 0; i < MISCINFO_FEATURES; i++)
		sgxfive_check(p, sgxfive_feature_names[i], mi[1 + i], want[i],
			      &bad);
	for (i = 0; i < MISCINFO_STRUCT_SIZES; i++) {
		char what[16];

		snprintf(what, sizeof(what), "struct size %d", i);
		sgxfive_check(p, what, mi[1 + MISCINFO_FEATURES + i],
			      sgx->fw.meta[SGX_FW_META_STRUCT_SIZES + i], &bad);
	}
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "all values match");
	return bad ? -EIO : 0;
}

static int sgxfive_miscinfo_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_miscinfo(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_miscinfo);

/* Writing to idle asks the EDM to idle the core through the POWER command. */
static ssize_t sgxfive_idle_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_power(sgx, POWERCMD_IDLE, POWMAN_IDLE_COMPLETE);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_idle_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_idle_write,
};

/* Queue one dummy transfer, optionally behind a destination sync. */
static int sgxfive_xfer_submit(struct sgxfive *sgx,
			       const struct sgx_xfer_sync *dst, u32 *fence,
			       u32 *update, u32 *done)
{
	u8 *ccb = xb_va(sgx, XB_CCB), *ctx = xb_va(sgx, XB_CTX);
	u32 *ctl = xb_va(sgx, XB_CCB_CTL), *scr = xb_va(sgx, XB_SCRATCH);
	const u32 scr_addr = xb_addr(sgx, XB_SCRATCH);
	const u32 data[] = { 0, xb_addr(sgx, XB_CTX) };
	u32 size;
	int ret;

	if (!sgx->live)
		return -ENODEV;
	if (!sgx->xt.runs)
		sgx_xfer_ctx_init(ctx, sgx->layout.pd_phys, xb_addr(sgx, XB_CCB),
				  xb_addr(sgx, XB_CCB_CTL));
	/* A client-directory check may have left the context on its directory. */
	sgx_xfer_ctx_pd(ctx, sgx->layout.pd_phys);
	*fence = 0x1234 + sgx->xt.runs;
	*update = 0xcafe0000 + sgx->xt.runs;
	WRITE_ONCE(scr[SCR_U], 0);
	WRITE_ONCE(scr[SCR_F], 0);
	size = sgx_xfer_dummy(ccb + sgx->xt.woff, scr_addr + 4 * SCR_F, *fence,
			      scr_addr + 4 * SCR_U, *update, dst);
	*done = (sgx->xt.woff + size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], *done);		/* host write offset */
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	ret = sgxfive_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
	if (ret) {
		WRITE_ONCE(ctl[0], sgx->xt.woff);	/* not announced: withdraw */
		return ret;
	}
	sgx->xt.woff = *done;
	sgx->xt.runs++;
	return 0;
}

/*
 * Sleep until the EDM's next SW_EVENT interrupt (500 ms at most), then check
 * the completion word once, with no polling: the EDM writes its status words
 * and sync counters before it raises the event.
 */
static int sgxfive_wait_event(struct sgxfive *sgx, struct drm_printer *p,
			      int sw0, const u32 *w, u32 want)
{
	ktime_t start = ktime_get();
	long left;

	left = wait_event_timeout(sgx->event_wq,
				  atomic_read(&sgx->sw_events) != sw0,
				  msecs_to_jiffies(500));
	rmb(); /* fresh reads of what the EDM wrote before the event */
	if (!left) {
		drm_printf(p, "no SW_EVENT after 500 ms\n");
		return -ETIMEDOUT;
	}
	drm_printf(p, "SW_EVENT after %lld us, completion word %s\n",
		   ktime_us_delta(ktime_get(), start),
		   READ_ONCE(*w) == want ? "already written" : "NOT YET WRITTEN");
	return READ_ONCE(*w) == want ? 0 : -EIO;
}

/* After a completed dummy: its writes, the CCB, the count and one SW_EVENT. */
static void sgxfive_xfer_verify(struct sgxfive *sgx, struct drm_printer *p,
				u32 fence, u32 update, u32 done, int sw0,
				int *bad)
{
	u32 *ctl = xb_va(sgx, XB_CCB_CTL), *scr = xb_va(sgx, XB_SCRATCH);
	u32 *ctx = xb_va(sgx, XB_CTX);

	drm_printf(p, "dummy transfer %u: ", sgx->xt.runs - 1);
	if (sgxfive_wait_event(sgx, p, sw0, &scr[SCR_F], fence))
		(*bad)++;
	sgxfive_check(p, "status F", READ_ONCE(scr[SCR_F]), fence, bad);
	sgxfive_check(p, "update U", READ_ONCE(scr[SCR_U]), update, bad);
	sgxfive_check(p, "ccb read off", READ_ONCE(ctl[1]), done, bad);
	sgxfive_check(p, "ctx count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 0,
		      bad);
	sgxfive_check(p, "sw events", atomic_read(&sgx->sw_events) - sw0, 1, bad);
}

/*
 * A dummy transfer: no render, but the EDM must queue the context, apply one
 * memory update (U), write one status word (F), advance the client CCB, drop
 * the context's count and raise SW_EVENT.
 */
static int sgxfive_xfer_dummy(struct sgxfive *sgx, struct drm_printer *p)
{
	int sw0 = atomic_read(&sgx->sw_events), bad = 0, ret;
	u32 fence, update, done;

	ret = sgxfive_xfer_submit(sgx, NULL, &fence, &update, &done);
	if (ret)
		return ret;
	sgxfive_xfer_verify(sgx, p, fence, update, done, sw0, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "dummy transfer complete");
	return bad ? -EIO : 0;
}

/*
 * The readiness check: a dummy behind a destination sync whose write counter
 * is one short must stay queued; once the host completes it and sends
 * PROCESS_QUEUES it runs, and the EDM moves the write counter past its
 * pending value.
 */
static int sgxfive_xfer_sync(struct sgxfive *sgx, struct drm_printer *p)
{
	u32 *scr = xb_va(sgx, XB_SCRATCH), *ctl = xb_va(sgx, XB_CCB_CTL);
	u32 *ctx = xb_va(sgx, XB_CTX);
	const u32 scr_addr = xb_addr(sgx, XB_SCRATCH);
	const struct sgx_xfer_sync dst = {
		.read_pending = 0, .read_complete = scr_addr + 4 * SCR_SYNC_READ,
		.write_pending = 6, .write_complete = scr_addr + 4 * SCR_SYNC_WRITE,
	};
	u32 fence, update, done, before;
	int sw0 = atomic_read(&sgx->sw_events), bad = 0, ret;

	if (!sgx->live)
		return -ENODEV;
	before = READ_ONCE(ctl[1]);
	WRITE_ONCE(scr[SCR_SYNC_READ], 0);
	WRITE_ONCE(scr[SCR_SYNC_WRITE], 5);	/* one write op outstanding */
	ret = sgxfive_xfer_submit(sgx, &dst, &fence, &update, &done);
	if (ret)
		return ret;
	msleep(50);
	rmb(); /* fresh reads of what the EDM wrote */
	sgxfive_check(p, "held: status F", READ_ONCE(scr[SCR_F]), 0, &bad);
	sgxfive_check(p, "held: read off", READ_ONCE(ctl[1]), before, &bad);
	sgxfive_check(p, "held: count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 1,
		      &bad);

	WRITE_ONCE(scr[SCR_SYNC_WRITE], 6);	/* the write op completes */
	ret = sgxfive_command(sgx, SGX_CMD_PROCESS_QUEUES, NULL, 0);
	if (ret)
		return ret;
	sgxfive_xfer_verify(sgx, p, fence, update, done, sw0, &bad);
	/* A destination's read counter is left alone. */
	sgxfive_check(p, "sync read cmpl", READ_ONCE(scr[SCR_SYNC_READ]), 0, &bad);
	sgxfive_check(p, "sync write cmpl", READ_ONCE(scr[SCR_SYNC_WRITE]), 7, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "sync wait and release correct");
	return bad ? -EIO : 0;
}

/*
 * Where the transfer programs go: PDS code at the PDS heap base, USE code at
 * the pixel-shader code base, each inside the one page mapped there. The
 * programs come from the firmware file's templates; each operation places
 * only its own, and one transfer is in flight at a time.
 */
struct sgxfive_tq_slot {
	u8 area;		/* XB_PDS or XB_USE; 0 = not used */
	u16 off;
};

static const struct sgxfive_tq_slot sgxfive_fill_place[SGX_FW_TQ_MAX] = {
	[SGX_FW_TQ_PDS_PRIM] = { XB_PDS, 0x00 },
	[SGX_FW_TQ_PDS_SEC] = { XB_PDS, 0x20 },
	[SGX_FW_TQ_PDS_PIXEVENT] = { XB_PDS, 0x40 },
	[SGX_FW_TQ_USE_FILL] = { XB_USE, 0x00 },
	[SGX_FW_TQ_USE_SEC] = { XB_USE, 0x10 },
	[SGX_FW_TQ_USE_EOT] = { XB_USE, 0x20 },
	[SGX_FW_TQ_USE_EOR] = { XB_USE, 0x60 },
};

/* A copy's PDS primary is larger than a fill's: everything after it moves. */
static const struct sgxfive_tq_slot sgxfive_copy_place[SGX_FW_TQ_MAX] = {
	[SGX_FW_TQ_PDS_PRIM_1SRC] = { XB_PDS, 0x00 },
	[SGX_FW_TQ_PDS_SEC] = { XB_PDS, 0x40 },
	[SGX_FW_TQ_PDS_PIXEVENT] = { XB_PDS, 0x60 },
	[SGX_FW_TQ_USE_COPY] = { XB_USE, 0x00 },
	[SGX_FW_TQ_USE_SEC] = { XB_USE, 0x10 },
	[SGX_FW_TQ_USE_EOT] = { XB_USE, 0x20 },
	[SGX_FW_TQ_USE_EOR] = { XB_USE, 0x60 },
};

/* Place one operation's templates; progs[] gets their CPU addresses. */
static int sgxfive_tq_place(struct sgxfive *sgx,
			    const struct sgxfive_tq_slot *place, u32 set,
			    u8 *progs[SGX_FW_TQ_MAX])
{
	struct sgx_fw_layout *l = &sgx->layout;
	unsigned int i;

	l->tq_set = 0;
	for (i = 0; i < SGX_FW_TQ_MAX; i++) {
		progs[i] = NULL;
		if (!(set & 1u << i) || !sgx_fw_tq(&sgx->fw, i) || !place[i].area)
			continue;
		l->tq_addr[i] = xb_addr(sgx, place[i].area) + place[i].off;
		l->tq_set |= 1u << i;
		progs[i] = (u8 *)xb_va(sgx, place[i].area) + place[i].off;
	}
	return sgx_fw_tq_need(&sgx->fw, l, set);
}

#define FILL_SENTINEL	0xdeadbeef

/*
 * Fill cases: a rectangle of a surface at the start of the destination area,
 * whose stride may be padded. Every dword of the three destination pages is
 * checked: the colour inside the rectangle, the sentinel everywhere else.
 */
static const struct sgxfive_fill_case {
	const char *name;
	u32 x, y, w, h, stride, colour;
} sgxfive_fill_cases[] = {
	{ "whole 64x32", 0, 0, 64, 32, 256, 0xff336699 },
	{ "32x16 at (8,4), stride 320", 8, 4, 32, 16, 320, 0xff00ff00 },
	{ "33x16 at (20,10), across tiles", 20, 10, 33, 16, 256, 0x80112233 },
	{ "one pixel at (63,31)", 63, 31, 1, 1, 256, 0x12345678 },
};

/*
 * Where a transfer writes: the destination's CPU and device addresses, the
 * page directory the transfer context names (0: the EDM's own), and
 * optionally one memory update's target.
 */
struct sgxfive_fill_target {
	u32 *cpu;
	u32 addr;
	u32 pd_phys;
	u32 *upd_cpu;
	u32 upd_addr;
};

#define FILL_UPDATE	0xc0ffee00

/*
 * Queue one transfer whose programs and background object are in place and
 * wait for it: build the command with sgx_xfer_fill_cmd(), check it word for
 * word against the firmware file's TQ_CMD list, kick, sleep until SW_EVENT,
 * then check the fence, the CCB, the context count and the event count.
 */
static int sgxfive_tq_run(struct sgxfive *sgx, struct drm_printer *p,
			  struct sgx_xfer_fill *f,
			  const u32 params[SGX_FW_TQ_NUM_PARAMS],
			  const struct sgxfive_fill_target *t, int *bad)
{
	struct sgx_fw_layout *l = &sgx->layout;
	u32 *ctl = xb_va(sgx, XB_CCB_CTL), *scr = xb_va(sgx, XB_SCRATCH);
	u32 *ctx = xb_va(sgx, XB_CTX);
	u8 *cmd = (u8 *)xb_va(sgx, XB_CCB) + sgx->xt.woff;
	const u32 data[] = { 0, xb_addr(sgx, XB_CTX) };
	struct sgx_fw_write cw[16];
	u32 size, done;
	int i, n, ret, sw0, wrong = 0;

	f->bg = xb_addr(sgx, XB_BG);
	f->req_base = xb_addr(sgx, XB_BG) & 0xfff00000;
	f->pds_exec_base = xb_addr(sgx, XB_PDS);
	f->pixev_exec = l->tq_addr[SGX_FW_TQ_PDS_PIXEVENT];
	f->status_addr = xb_addr(sgx, XB_SCRATCH) + 4 * SCR_F;
	f->fence_id = sgx->xt.runs;
	if (!sgx->xt.runs)
		sgx_xfer_ctx_init((u8 *)ctx, l->pd_phys, xb_addr(sgx, XB_CCB),
				  xb_addr(sgx, XB_CCB_CTL));
	/* The context is idle between transfers, so its PD can change. */
	sgx_xfer_ctx_pd((u8 *)ctx, t->pd_phys ? t->pd_phys : l->pd_phys);
	WRITE_ONCE(scr[SCR_F], 0);
	if (t->upd_cpu)
		WRITE_ONCE(*t->upd_cpu, 0);

	size = sgx_xfer_fill_cmd(cmd, f);
	if (t->upd_cpu)		/* applied by the EDM through the context's PD */
		size = sgx_xfer_add_update(cmd, t->upd_addr,
					   FILL_UPDATE + sgx->xt.runs);
	n = sgx_fw_tq_cmd(&sgx->fw, l, params, cw, ARRAY_SIZE(cw));
	if (n < 0)
		return n;
	for (i = 0; i < n; i++) {
		u32 *w = (u32 *)(cmd + cw[i].reg);

		if (*w != cw[i].val) {
			drm_printf(p, "command word %#x: built %#x, TQ_CMD %#x\n",
				   cw[i].reg, *w, cw[i].val);
			wrong++;
		}
	}
	if (wrong)
		return -EINVAL;
	drm_printf(p, "command matches the %d TQ_CMD words\n", n);

	done = (sgx->xt.woff + size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], done);
	sw0 = atomic_read(&sgx->sw_events);
	/* New pages were mapped: the BIF must refetch PD and PT entries. */
	sgx->cache_control |= SGX_CC_INVAL_BIF | SGX_CC_INVAL_DATA;
	wmb(); /* programs, object, command and sentinels before the kick */
	ret = sgxfive_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
	if (ret) {
		WRITE_ONCE(ctl[0], sgx->xt.woff);	/* not announced: withdraw */
		return ret;
	}
	sgx->xt.woff = done;
	sgx->xt.runs++;
	if (sgxfive_wait_event(sgx, p, sw0, &scr[SCR_F], f->fence_id + 1))
		(*bad)++;

	if (t->upd_cpu)
		sgxfive_check(p, "update word", READ_ONCE(*t->upd_cpu),
			      FILL_UPDATE + f->fence_id, bad);
	sgxfive_check(p, "status F", READ_ONCE(scr[SCR_F]), f->fence_id + 1, bad);
	sgxfive_check(p, "ccb read off", READ_ONCE(ctl[1]), done, bad);
	sgxfive_check(p, "ctx count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 0,
		      bad);
	sgxfive_check(p, "sw events", atomic_read(&sgx->sw_events) - sw0, 1, bad);
	sgxfive_check(p, "bif faults", atomic_read(&sgx->bif_faults), 0, bad);
	return 0;
}

/*
 * Check every dword of the destination area (three pages): inside the
 * rectangle the colour, or with a source the texel at the same offset from
 * the source's first; the sentinel outside it.
 */
static void sgxfive_check_dst(struct drm_printer *p, const u32 *dst, u32 stride,
			      const struct sgx_xfer_rect *r, const u32 *src,
			      u32 src_stride, u32 colour, int *bad)
{
	unsigned int i, wrong = 0, first_bad = ~0u;

	for (i = 0; i < 3 * PAGE_SIZE / 4; i++) {
		u32 x = i % (stride / 4), y = i / (stride / 4), want = FILL_SENTINEL;

		if (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1)
			want = src ? src[(y - r->y0) * (src_stride / 4) + x - r->x0] :
				     colour;
		if (READ_ONCE(dst[i]) != want) {
			wrong++;
			if (first_bad == ~0u)
				first_bad = i;
		}
	}
	if (first_bad != ~0u)
		drm_printf(p, "first wrong dword %u (x %u, y %u) = %#x\n", first_bad,
			   first_bad % (stride / 4), first_bad / (stride / 4),
			   READ_ONCE(dst[first_bad]));
	sgxfive_check(p, "wrong dwords", wrong, 0, bad);
}

/* A solid fill of one case. */
static int sgxfive_xfer_fill(struct sgxfive *sgx, struct drm_printer *p,
			     const struct sgxfive_fill_case *fc,
			     const struct sgxfive_fill_target *t)
{
	struct sgx_fw_layout *l = &sgx->layout;
	const struct sgx_fw_tq_fill tf = {
		.dest = t->addr, .width = fc->w, .height = fc->h,
		.stride = fc->stride, .colour = fc->colour, .x = fc->x, .y = fc->y,
	};
	const struct sgx_fw_tq *pri = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_PRIM);
	const struct sgx_fw_tq *sec = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_SEC);
	struct sgx_xfer_fill f = {
		.rect = { fc->x, fc->y, fc->x + fc->w, fc->y + fc->h },
	};
	u32 params[SGX_FW_TQ_NUM_PARAMS];
	u8 *progs[SGX_FW_TQ_MAX];
	unsigned int i;
	int ret, bad = 0;

	if (!sgx->live)
		return -ENODEV;
	if (!pri || !sec) {
		drm_printf(p, "the firmware file has no fill templates (v1)\n");
		return -ENOENT;
	}
	ret = sgxfive_tq_place(sgx, sgxfive_fill_place, SGX_FW_TQ_FILL_SET, progs);
	if (!ret)
		ret = sgx_fw_tq_fill_params(&tf, params);
	if (!ret)
		ret = sgx_fw_tq_build(&sgx->fw, l, params, progs);
	if (ret) {
		drm_printf(p, "building the fill programs failed: %d\n", ret);
		return ret;
	}
	sgx_xfer_bgobj(xb_va(sgx, XB_BG), xb_addr(sgx, XB_PDS),
		       l->tq_addr[SGX_FW_TQ_PDS_PRIM], pri->pds_data,
		       l->tq_addr[SGX_FW_TQ_PDS_SEC], sec->pds_data, &f.rect);
	for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
		WRITE_ONCE(t->cpu[i], FILL_SENTINEL);

	drm_printf(p, "fill %s: ", fc->name);
	ret = sgxfive_tq_run(sgx, p, &f, params, t, &bad);
	if (ret)
		return ret;
	rmb(); /* fresh reads of what the GPU wrote */
	sgxfive_check_dst(p, t->cpu, fc->stride, &f.rect, NULL, 0, fc->colour,
			  &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "solid fill correct");
	return bad ? -EIO : 0;
}

/* Every fill case on the destination area; a queue failure stops the run. */
static int sgxfive_xfer_fills(struct sgxfive *sgx, struct drm_printer *p)
{
	const struct sgxfive_fill_target t = {
		.cpu = xb_va(sgx, XB_DST), .addr = xb_addr(sgx, XB_DST),
	};
	unsigned int i;
	int ret = 0, r;

	for (i = 0; i < ARRAY_SIZE(sgxfive_fill_cases); i++) {
		r = sgxfive_xfer_fill(sgx, p, &sgxfive_fill_cases[i], &t);
		if (r && !ret)
			ret = r;
		if (r && r != -EIO)
			break;
	}
	return ret;
}

/*
 * Copy cases: a width x height rectangle whose first texel is at (sx, sy) of
 * the source area, to (x, y) of the destination area. Strides may be padded.
 */
static const struct sgxfive_copy_case {
	const char *name;
	u32 sx, sy, src_stride, x, y, w, h, dst_stride;
} sgxfive_copy_cases[] = {
	{ "whole 64x32", 0, 0, 256, 0, 0, 64, 32, 256 },
	{ "32x16 from (5,3) stride 320 to (8,4) stride 288",
	  5, 3, 320, 8, 4, 32, 16, 288 },
	{ "33x16 across tiles", 0, 0, 256, 20, 10, 33, 16, 256 },
};

#define COPY_SRC_PAGES	2

/* A copy of one case, from a patterned source area. */
static int sgxfive_xfer_copy(struct sgxfive *sgx, struct drm_printer *p,
			     const struct sgxfive_copy_case *cc)
{
	struct sgx_fw_layout *l = &sgx->layout;
	const struct sgxfive_fill_target t = {
		.cpu = xb_va(sgx, XB_DST), .addr = xb_addr(sgx, XB_DST),
	};
	u32 *src = xb_va(sgx, XB_SRC);
	u32 src_first = cc->sy * (cc->src_stride / 4) + cc->sx;
	const struct sgx_fw_tq_copy tc = {
		.src = xb_addr(sgx, XB_SRC) + 4 * src_first,
		.dst = t.addr, .width = cc->w, .height = cc->h,
		.src_stride = cc->src_stride, .dst_stride = cc->dst_stride,
		.x = cc->x, .y = cc->y,
	};
	const struct sgx_fw_tq *pri = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_PRIM_1SRC);
	const struct sgx_fw_tq *sec = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_SEC);
	const struct sgx_xfer_rect whole = { 0, 0, cc->w, cc->h };
	struct sgx_xfer_fill f = {
		.rect = { cc->x, cc->y, cc->x + cc->w, cc->y + cc->h },
		.layers = 1,
	};
	u32 params[SGX_FW_TQ_NUM_PARAMS];
	u8 *progs[SGX_FW_TQ_MAX];
	unsigned int i;
	int ret, bad = 0;

	if (!sgx->live)
		return -ENODEV;
	if (!pri || !sec) {
		drm_printf(p, "the firmware file has no copy templates (v3)\n");
		return -ENOENT;
	}
	ret = sgxfive_tq_place(sgx, sgxfive_copy_place, SGX_FW_TQ_COPY_SET, progs);
	if (!ret)
		ret = sgx_fw_tq_copy_params(&tc, params);
	if (!ret)
		ret = sgx_fw_tq_build(&sgx->fw, l, params, progs);
	if (ret) {
		drm_printf(p, "building the copy programs failed: %d\n", ret);
		return ret;
	}
	/* The texture is exactly the copied rectangle: coordinates 0..1. */
	sgx_xfer_bgobj_copy(xb_va(sgx, XB_BG), xb_addr(sgx, XB_PDS),
			    l->tq_addr[SGX_FW_TQ_PDS_PRIM_1SRC], pri->pds_data,
			    l->tq_addr[SGX_FW_TQ_PDS_SEC], sec->pds_data, &f.rect,
			    &whole, cc->w, cc->h);
	/* Every byte varies: a swizzle or a skew shows. */
	for (i = 0; i < COPY_SRC_PAGES * PAGE_SIZE / 4; i++) {
		u32 x = i % (cc->src_stride / 4), y = i / (cc->src_stride / 4);

		WRITE_ONCE(src[i], 0xa5000000 | (y & 0xff) << 16 |
				   (x & 0xff) << 8 | ((x ^ y) & 0xff));
	}
	for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
		WRITE_ONCE(t.cpu[i], FILL_SENTINEL);

	drm_printf(p, "copy %s: ", cc->name);
	ret = sgxfive_tq_run(sgx, p, &f, params, &t, &bad);
	if (ret)
		return ret;
	rmb(); /* fresh reads of what the GPU wrote */
	sgxfive_check_dst(p, t.cpu, cc->dst_stride, &f.rect, src + src_first,
			  cc->src_stride, 0, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "copy correct");
	return bad ? -EIO : 0;
}

/* Every copy case; a queue failure stops the run. */
static int sgxfive_xfer_copies(struct sgxfive *sgx, struct drm_printer *p)
{
	unsigned int i;
	int ret = 0, r;

	for (i = 0; i < ARRAY_SIZE(sgxfive_copy_cases); i++) {
		r = sgxfive_xfer_copy(sgx, p, &sgxfive_copy_cases[i]);
		if (r && !ret)
			ret = r;
		if (r && r != -EIO)
			break;
	}
	return ret;
}

/*
 * The isolation check: does a transfer's 3D work use the page directory its
 * context names? A client PD maps a destination at CLIENT_DST and an update
 * target at CLIENT_UPD, neither of which exists in the EDM's own PD. The fill
 * must land, and the update must be applied, through the client PD alone.
 *
 * The EDM applies the memory updates while the context's directory is in
 * force, so the client PD also maps every EDM buffer at its kernel address,
 * read-write or (alias_ro) read-only. With no_alias it maps only what the
 * job itself uses (its programs and background object), and the job then
 * carries no update.
 */
#define CLIENT_DST	0x20000000
#define CLIENT_UPD	0x20010000

static int sgxfive_client_pd(struct sgxfive *sgx)
{
	struct sgxfive_xtest *xt = &sgx->xt;
	unsigned int i;
	int ret;

	/* Rebuilt each time, so a change of alias_ro or no_alias takes effect. */
	for (i = 0; i < ARRAY_SIZE(xt->cbuf); i++)
		sgxfive_unmap(sgx, &xt->cbuf[i]);
	if (xt->cpd)
		sgx_mmu_pd_destroy(xt->cpd);
	xt->cpd = sgx_mmu_pd_create();
	if (!xt->cpd)
		return -ENOMEM;
	ret = sgxfive_map_pd(sgx, xt->cpd, &xt->cbuf[0], CLIENT_DST, 0x3000, 0);
	if (!ret)
		ret = sgxfive_map_pd(sgx, xt->cpd, &xt->cbuf[1], CLIENT_UPD, 4, 0);
	for (i = 0; i < SGX_FW_MAX_BUFFERS + XB_COUNT && !ret; i++) {
		struct sgxfive_buf *b = i < SGX_FW_MAX_BUFFERS ? &sgx->buf[i] :
					&xt->buf[i - SGX_FW_MAX_BUFFERS];
		unsigned int x = i - SGX_FW_MAX_BUFFERS;
		bool job = i >= SGX_FW_MAX_BUFFERS &&
			   (x == XB_PDS || x == XB_USE || x == XB_BG);

		if (b->pd && (job || !xt->no_alias))
			ret = sgx_mmu_map(xt->cpd, b->addr, b->pages, b->npages,
					  xt->alias_ro ? SGX_MMU_RO : 0);
	}
	return ret;
}

static int sgxfive_xfer_iso(struct sgxfive *sgx, struct drm_printer *p)
{
	struct sgxfive_fill_target t;
	int ret;

	ret = sgx->live ? sgxfive_client_pd(sgx) : -ENODEV;
	if (ret)
		return ret;
	t = (struct sgxfive_fill_target) {
		.cpu = sgx->xt.cbuf[0].va, .addr = CLIENT_DST,
		.pd_phys = sgx_mmu_pd_phys(sgx->xt.cpd),
	};
	if (!sgx->xt.no_alias) {
		t.upd_cpu = sgx->xt.cbuf[1].va;
		t.upd_addr = CLIENT_UPD;
	}
	drm_printf(p, "client PD at %#x, EDM aliases %s\n", t.pd_phys,
		   sgx->xt.no_alias ? "none, no updates" :
		   sgx->xt.alias_ro ? "read-only" : "read-write");
	ret = sgxfive_xfer_fill(sgx, p, &sgxfive_fill_cases[1], &t);
	drm_printf(p, "%s\n", ret ? "ISOLATION CHECK FAILED" :
		   sgx->xt.no_alias ? "3D work used the client PD alone" :
		   "3D work and updates used the client PD");
	return ret;
}

/*
 * The negative check: a client fill aimed at a kernel buffer that the client
 * PD maps only read-only (the destination area, at its kernel address). The
 * writes must fault and the buffer must keep its sentinel. The fault leaves
 * the EDM unable to finish the job: reload and boot afterwards.
 */
static int sgxfive_xfer_ro(struct sgxfive *sgx, struct drm_printer *p)
{
	const bool alias_ro = sgx->xt.alias_ro, no_alias = sgx->xt.no_alias;
	struct sgxfive_fill_target t;
	unsigned int i, written = 0;
	u32 *dst = xb_va(sgx, XB_DST);
	int ret;

	/* Read-only aliases of every EDM buffer, for this check only. */
	sgx->xt.alias_ro = true;
	sgx->xt.no_alias = false;
	ret = sgx->live ? sgxfive_client_pd(sgx) : -ENODEV;
	if (ret)
		goto out;
	t = (struct sgxfive_fill_target) {
		.cpu = dst, .addr = xb_addr(sgx, XB_DST),
		.pd_phys = sgx_mmu_pd_phys(sgx->xt.cpd),
	};
	/* -EIO: kicked, then a mismatch; any other error: never kicked. */
	ret = sgxfive_xfer_fill(sgx, p, &sgxfive_fill_cases[0], &t);
	if (ret && ret != -EIO)
		goto out;
	rmb(); /* what, if anything, reached memory */
	for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
		written += READ_ONCE(dst[i]) != FILL_SENTINEL;
	drm_printf(p, "dwords written through the read-only alias: %u\n",
		   written);
	drm_printf(p, "BIF_INT_STAT %#x BIF_FAULT %#x bif_faults %d\n",
		   sgxfive_rd(sgx, SGX_BIF_INT_STAT), sgxfive_rd(sgx, SGX_BIF_FAULT),
		   atomic_read(&sgx->bif_faults));
	drm_printf(p, "%s\n", written ? "NOT PROTECTED" :
		   "kernel buffer protected; reload and boot to recover");
	ret = 0;
out:
	sgx->xt.alias_ro = alias_ro;
	sgx->xt.no_alias = no_alias;
	return ret;
}

/* Run one check under the device lock; a mismatch still reads as text. */
static int sgxfive_run(struct seq_file *m,
		       int (*fn)(struct sgxfive *sgx, struct drm_printer *p))
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = fn(sgx, &p);
	if (ret && ret != -ENODEV)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}

static int sgxfive_xfer_dummy_show(struct seq_file *m, void *unused)
{
	return sgxfive_run(m, sgxfive_xfer_dummy);
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_dummy);

static int sgxfive_xfer_sync_show(struct seq_file *m, void *unused)
{
	return sgxfive_run(m, sgxfive_xfer_sync);
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_sync);

static int sgxfive_xfer_fill_show(struct seq_file *m, void *unused)
{
	return sgxfive_run(m, sgxfive_xfer_fills);
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_fill);

static int sgxfive_xfer_copy_show(struct seq_file *m, void *unused)
{
	return sgxfive_run(m, sgxfive_xfer_copies);
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_copy);

static int sgxfive_xfer_iso_show(struct seq_file *m, void *unused)
{
	return sgxfive_run(m, sgxfive_xfer_iso);
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_iso);

static int sgxfive_xfer_ro_show(struct seq_file *m, void *unused)
{
	return sgxfive_run(m, sgxfive_xfer_ro);
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_ro);

/*
 * Writing to reset resets the firmware in place: the recovery of a hang, on
 * demand. It blames no job, and contexts with unfinished jobs run them again,
 * unless a recovery is held: then it is that recovery, which blames the held
 * job (sgxfive_recovery_hold).
 */
static ssize_t sgxfive_reset_write(struct file *f, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	sgxfive_render_keep_release(sgx);	/* render_keep's contexts go with a manual reset */
	ret = sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_MANUAL);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_reset_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_reset_write,
};

/*
 * Under the device lock, a recovery about to start: whether to hold it.
 * While debugfs hold_recovery is set, the hang and fault detectors only
 * report, once until the next reset (sgxfive_recovery_held): the hung job
 * stays in the firmware, its context's memory as the GPU left it, until reset
 * is written. A reset written to reset is never held; while one is held it
 * becomes the held recovery, its job (*suspect) blamed with the error of the
 * detector that found it (*why), so the job fails once instead of running
 * again to hang again. It stays blame-free if that job is no longer its
 * context's oldest unfinished one. The reset of a failed render-context
 * cleanup resets blame-free when nothing is held; while a recovery is held
 * it is dropped and the hold stays, its job blamed by no one: the held job's
 * own cleanup is expected to fail, and the context's memory is not freed
 * under the firmware, as its release waits for the firmware to read a later
 * command, which it does only after the reset that ends the hold.
 */
bool sgxfive_recovery_hold(struct sgxfive *sgx, u64 *suspect, enum sgxfive_hang *why)
{
	bool unfinished;

	if (*why == SGXFIVE_HANG_CLEANUP)
		return sgxfive_recovery_held(sgx);
	if (*why == SGXFIVE_HANG_MANUAL) {
		if (!sgxfive_recovery_held(sgx) || *suspect != U64_MAX)
			return false;
		spin_lock_irq(&sgx->ctx_lock);
		unfinished = sgxfive_ctx_of_job(&sgx->ctx_list, sgx->held_suspect);
		spin_unlock_irq(&sgx->ctx_lock);
		if (unfinished) {
			*suspect = sgx->held_suspect;
			*why = sgx->held_why;
		}
		return false;
	}
	if (!READ_ONCE(sgx->hold_recovery))
		return false;
	if (!sgxfive_recovery_held(sgx)) {
		drm_warn(sgx->dev,
			 "sgxfive: job %llu hung, recovery held (debugfs hold_recovery)\n",
			 *suspect);
		sgx->held_suspect = *suspect;
		sgx->held_why = *why;
	}
	WRITE_ONCE(sgx->recovery_held, true);
	return true;
}

/*
 * A recovery reset rebuilt the firmware's buffers: the test area starts over,
 * and a held recovery ends. Returns whether one was held.
 */
bool sgxfive_debug_reset(struct sgxfive *sgx)
{
	bool held = sgxfive_recovery_held(sgx);
	unsigned int i;

	WRITE_ONCE(sgx->recovery_held, false);
	sgxfive_debug_disarm(sgx);
	/* the stock firmware is back: nothing of ours is armed (the blob stays) */
	WRITE_ONCE(sgx->own.armed, false);
	for (i = 0; i < XB_COUNT; i++)
		if (sgx->xt.buf[i].va)
			memset(sgx->xt.buf[i].va, 0, sgxfive_xbuf[i].size);
	if (sgx->xt.buf[XB_OWNRES].va)		/* the header goes with the page: put it back */
		*(u32 *)sgx->xt.buf[XB_OWNRES].va = SGX_OWN_MAGIC_DWORD;
	sgx->xt.woff = 0;
	sgx->xt.runs = 0;	/* the next transfer initialises the context */
	return held;
}

/*
 * debugfs kpoke (sgxfive_kpoke.h): each line written arms a poke of the
 * firmware command the kernel builds for the next job of its kind, "ta:N",
 * "rd:N" or "tq:N" with "=V" or "^=V"; "clear" drops what is armed. Reading
 * shows what is armed, a poke a line. Root only: a poked kernel word, an
 * address above all, can fault or hang the job or make the firmware write
 * where it should not; the job fails and is recovered.
 */
static int sgxfive_kpoke_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	char line[SGXFIVE_KPOKE_LINE];
	struct sgxfive_kpokes *k;
	unsigned int i;

	k = kzalloc_obj(*k);
	if (!k)
		return -ENOMEM;
	spin_lock(&sgx->kpoke_lock);
	*k = sgx->kpokes;
	spin_unlock(&sgx->kpoke_lock);
	for (i = 0; i < k->n; i++) {
		sgxfive_kpoke_format(&k->p[i], line, sizeof(line));
		seq_printf(m, "%s\n", line);
	}
	kfree(k);
	return 0;
}

static int sgxfive_kpoke_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_kpoke_show, inode->i_private);
}

static ssize_t sgxfive_kpoke_fwrite(struct file *f, const char __user *ubuf,
				    size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	char *buf;
	int ret;

	if (!len || len > PAGE_SIZE)
		return -EINVAL;
	buf = memdup_user_nul(ubuf, len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	ret = sgxfive_kpoke_write(sgx, buf);
	kfree(buf);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_kpoke_fops = {
	.owner = THIS_MODULE, .open = sgxfive_kpoke_open, .read = seq_read,
	.write = sgxfive_kpoke_fwrite, .llseek = seq_lseek, .release = single_release,
};

/*
 * debugfs kreg: one write to a register the firmware never touches
 * (sgxfive_kreg_allowed). Only while the firmware runs and has no
 * render-node job, under the device lock so that none is kicked meanwhile;
 * every write is logged. The value stays until the next write or the next
 * reset of the SGX.
 */
int sgxfive_kreg_write(struct sgxfive *sgx, u32 off, bool flip, u32 value)
{
	u32 old, val;
	int ret = 0;

	if (!READ_ONCE(sgx->live))
		return -ENODEV;
	if (!sgxfive_kreg_ok(off))
		return -EPERM;
	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		ret = -ENODEV;
	} else if (sgxfive_oldest(sgx) != U64_MAX) {
		ret = -EBUSY;
	} else {
		old = sgxfive_rd(sgx, off);
		val = flip ? old ^ value : value;
		iowrite32(val, sgx->regs + off);
		drm_info(sgx->dev, "sgxfive: kreg 0x%04x: 0x%08x -> 0x%08x, reads 0x%08x\n",
			 off, old, val, sgxfive_rd(sgx, off));
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

/* Reading shows the allowed registers, "0xOFFS 0xVALUE" a line. */
static int sgxfive_kreg_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	unsigned int i;
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (!sgx->live)
		ret = -ENODEV;
	for (i = 0; !ret && i < SGXFIVE_KREG_COUNT; i++)
		seq_printf(m, "0x%04x 0x%08x\n", sgxfive_kreg_allowed[i],
			   sgxfive_rd(sgx, sgxfive_kreg_allowed[i]));
	mutex_unlock(&sgx->lock);
	return ret;
}

static int sgxfive_kreg_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_kreg_show, inode->i_private);
}

/* One line: OFFSET=VALUE or OFFSET^=VALUE. */
static ssize_t sgxfive_kreg_fwrite(struct file *f, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	char buf[SGXFIVE_KPOKE_LINE];
	u32 off, value;
	bool flip;
	int ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	ret = sgxfive_kreg_parse(strim(buf), &off, &flip, &value);
	if (!ret)
		ret = sgxfive_kreg_write(sgx, off, flip, value);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_kreg_fops = {
	.owner = THIS_MODULE, .open = sgxfive_kreg_open, .read = seq_read,
	.write = sgxfive_kreg_fwrite, .llseek = seq_lseek, .release = single_release,
};

/*
 * debugfs kregnow: a write to CR_CLKGATECTL2, a host event enable, a host
 * clear register or the register at 0x0740 (sgxfive_kregnow_allowed) at any
 * time, while a job runs or is held. A clear register is write-only: not read
 * before the write nor after it, so no flip; its log line has the event
 * status registers as read right after the write instead. CR_BIF_CTRL and
 * CR_BIF_INT_STAT, which the firmware reaches too, only while no job is
 * unfinished or a recovery is held (-EBUSY otherwise); their log line adds
 * BIF_INT_STAT and BIF_FAULT as read right after the write. The firmware writes none of them after its boot, so there is
 * nothing to race with, though it reads 0x0740 at a TA kick (a 32-bit store,
 * so it sees the old value or the new); the device lock only keeps a boot or
 * stop from running meanwhile.
 * The first write of an enable keeps what both held (sgxfive_hostirq): the
 * interrupt of an event the firmware did not enable puts them back, since
 * only the EDM's SW_EVENT is acknowledged here. That arm, the read and the
 * write are one step under the interrupt lock, so that the interrupt's
 * restore cannot land between them. Every write is logged. The value stays
 * until the next write or the next reset of the SGX.
 */
int sgxfive_kregnow_write(struct sgxfive *sgx, u32 off, bool flip, u32 value)
{
	struct sgxfive_hostirq *h = &sgx->hostirq;
	unsigned long flags;
	u32 old, val;
	int ret = 0;

	if (!READ_ONCE(sgx->live))
		return -ENODEV;
	if (!sgxfive_kregnow_ok(off))
		return -EPERM;
	if (flip && sgxfive_kregnow_wo(off))
		return -EINVAL;
	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		ret = -ENODEV;
	} else if (sgxfive_kregnow_bif(off) &&
		   !sgxfive_kregnow_bif_allowed(sgxfive_oldest(sgx) == U64_MAX,
						sgxfive_recovery_held(sgx))) {
		ret = -EBUSY;
	} else if (sgxfive_kregnow_wo(off)) {
		/* a clear register: no read before or after, the status read right after */
		iowrite32(value, sgx->regs + off);
		drm_info(sgx->dev, "sgxfive: kregnow 0x%04x = 0x%08x (write-only), EVENT_STATUS 0x%08x/0x%08x\n",
			 off, value, sgxfive_rd(sgx, SGX_EVENT_STATUS),
			 sgxfive_rd(sgx, SGX_EVENT_STATUS2));
	} else {
		bool enable = off == SGX_EVENT_HOST_ENABLE || off == SGX_EVENT_HOST_ENABLE2;

		spin_lock_irqsave(&h->lock, flags);
		if (enable && !h->armed)
			sgxfive_hostirq_arm(h, sgxfive_rd(sgx, SGX_EVENT_HOST_ENABLE),
					    sgxfive_rd(sgx, SGX_EVENT_HOST_ENABLE2));
		old = sgxfive_rd(sgx, off);
		val = flip ? old ^ value : value;
		iowrite32(val, sgx->regs + off);
		spin_unlock_irqrestore(&h->lock, flags);
		if (sgxfive_kregnow_bif(off))
			drm_info(sgx->dev, "sgxfive: kregnow 0x%04x: 0x%08x -> 0x%08x, reads 0x%08x, BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x\n",
				 off, old, val, sgxfive_rd(sgx, off),
				 sgxfive_rd(sgx, SGX_BIF_INT_STAT), sgxfive_rd(sgx, SGX_BIF_FAULT));
		else
			drm_info(sgx->dev, "sgxfive: kregnow 0x%04x: 0x%08x -> 0x%08x, reads 0x%08x\n",
				 off, old, val, sgxfive_rd(sgx, off));
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

/*
 * debugfs kicker: the event-kicker word the host bumps with every command
 * (sgx_ccb_submit), the CCB's write and read offsets and the EVENT_KICK
 * writes since the word was last zeroed (at the boot); "=N" sets the word,
 * live and with no job unfinished, and sends no command and writes no
 * register. No file kicks without a command: a kick with nothing at the read
 * offset would have the EDM read an empty command. debugfs kick_no_bump (a
 * one-shot bool) has the next command written and kicked with the word left.
 */
int sgxfive_kicker_write(struct sgxfive *sgx, u32 value)
{
	u32 *w, old;
	int ret = 0;

	if (!READ_ONCE(sgx->live))
		return -ENODEV;
	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		ret = -ENODEV;
	} else if (sgxfive_oldest(sgx) != U64_MAX) {
		ret = -EBUSY;
	} else {
		w = sgxfive_words(sgx, BUF_KICKER);
		old = READ_ONCE(*w);
		WRITE_ONCE(*w, value);
		wmb(); /* the EDM reads it at its next event */
		drm_info(sgx->dev, "sgxfive: kicker %u -> %u (no command, no kick)\n", old, value);
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

static int sgxfive_kicker_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	const u32 *ctl;
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		ret = -ENODEV;
	} else {
		ctl = sgxfive_words(sgx, BUF_CCB_CTL);
		seq_printf(m, "kicker %u ccbctl %u %u kicks %u\n",
			   READ_ONCE(*sgxfive_words(sgx, BUF_KICKER)), READ_ONCE(ctl[0]),
			   READ_ONCE(ctl[1]), READ_ONCE(sgx->event_kicks));
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

static int sgxfive_kicker_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_kicker_show, inode->i_private);
}

static ssize_t sgxfive_kicker_fwrite(struct file *f, const char __user *ubuf, size_t len,
				     loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	char buf[16];
	u32 value;
	int ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	ret = sgxfive_kicker_parse(strim(buf), &value);
	if (!ret)
		ret = sgxfive_kicker_write(sgx, value);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_kicker_fops = {
	.owner = THIS_MODULE, .open = sgxfive_kicker_open, .read = seq_read,
	.write = sgxfive_kicker_fwrite, .llseek = seq_lseek, .release = single_release,
};

/* From the host's SGX interrupt, before the driver acts on it. */
void sgxfive_debug_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2)
{
	struct sgxfive_hostirq *h = &sgx->hostirq;
	unsigned long flags;
	bool restore;

	spin_lock_irqsave(&h->lock, flags);
	restore = sgxfive_hostirq_note(h, stat_1, stat_2);
	if (restore) {
		iowrite32(h->saved[0], sgx->regs + SGX_EVENT_HOST_ENABLE);
		iowrite32(h->saved[1], sgx->regs + SGX_EVENT_HOST_ENABLE2);
	}
	spin_unlock_irqrestore(&h->lock, flags);
	if (!restore)
		return;
	dev_info_ratelimited(sgx->dev->dev,
			     "sgxfive: host interrupt with no SW_EVENT: status %#x/%#x\n",
			     stat_1, stat_2);
}

/* The registers, then the host interrupt counts. */
static int sgxfive_kregnow_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct sgxfive_hostirq *h = &sgx->hostirq;
	unsigned long flags;
	unsigned int i;
	int ret = 0;
	u32 last[2], saved[2];
	bool armed;

	mutex_lock(&sgx->lock);
	if (!sgx->live)
		ret = -ENODEV;
	for (i = 0; !ret && i < SGXFIVE_KREGNOW_COUNT; i++)
		if (!sgxfive_kregnow_wo(sgxfive_kregnow_allowed[i]))	/* never read */
			seq_printf(m, "0x%04x 0x%08x\n", sgxfive_kregnow_allowed[i],
				   sgxfive_rd(sgx, sgxfive_kregnow_allowed[i]));
	if (!ret) {
		spin_lock_irqsave(&h->lock, flags);
		armed = h->armed;
		memcpy(last, h->last, sizeof(last));
		memcpy(saved, h->saved, sizeof(saved));
		spin_unlock_irqrestore(&h->lock, flags);
		seq_printf(m, "host_irqs %d extra %d last 0x%08x 0x%08x "
			   "armed %d saved 0x%08x 0x%08x\n",
			   atomic_read(&h->irqs), atomic_read(&h->extra), last[0], last[1],
			   armed, saved[0], saved[1]);
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

static int sgxfive_kregnow_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_kregnow_show, inode->i_private);
}

static ssize_t sgxfive_kregnow_fwrite(struct file *f, const char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	char buf[SGXFIVE_KPOKE_LINE];
	u32 off, value;
	bool flip;
	int ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	ret = sgxfive_kregnow_parse(strim(buf), &off, &flip, &value);
	if (!ret)
		ret = sgxfive_kregnow_write(sgx, off, flip, value);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_kregnow_fops = {
	.owner = THIS_MODULE, .open = sgxfive_kregnow_open, .read = seq_read,
	.write = sgxfive_kregnow_fwrite, .llseek = seq_lseek, .release = single_release,
};

/*
 * debugfs tadata_fill: arm the fill of a page of the next render's TA data
 * (sgxfive_kpoke.h); reading shows what is armed. Root only.
 */
static int sgxfive_tdfill_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct sgxfive_tdfill f;

	spin_lock(&sgx->kpoke_lock);
	f = sgx->tdfill;
	spin_unlock(&sgx->kpoke_lock);
	if (f.armed)
		seq_printf(m, "%u 0x%08x%s\n", f.page, f.value, f.ramp ? " ramp" : "");
	return 0;
}

static int sgxfive_tdfill_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_tdfill_show, inode->i_private);
}

static ssize_t sgxfive_tdfill_fwrite(struct file *f, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	char *buf;
	int ret;

	if (!len || len >= SGXFIVE_KPOKE_LINE)
		return -EINVAL;
	buf = memdup_user_nul(ubuf, len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	ret = sgxfive_tdfill_write(sgx, buf);
	kfree(buf);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_tdfill_fops = {
	.owner = THIS_MODULE, .open = sgxfive_tdfill_open, .read = seq_read,
	.write = sgxfive_tdfill_fwrite, .llseek = seq_lseek, .release = single_release,
};

/*
 * debugfs render_param and render_tadata: the newest render context's
 * parameter slot from its PB on, and its TA data buffers, as the GPU left
 * them, to read what the TA wrote (sgxfive_render_dump). Read them while the
 * context lives and has no job in the firmware. Root only.
 */
static ssize_t sgxfive_render_dump_read(struct file *f, char __user *ubuf, size_t len,
					loff_t *ppos, bool tadata)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	struct sgxfive_ctx *ctx;
	ssize_t ret;
	size_t n;
	void *buf;

	len = min_t(size_t, len, SZ_64K);
	buf = kvmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ctx = sgxfive_render_newest(sgx);
	if (!ctx) {
		kvfree(buf);
		return -ENODEV;
	}
	n = sgxfive_render_dump(ctx, tadata, buf, len, *ppos);
	sgxfive_ctx_put(ctx);
	ret = n;
	if (n && copy_to_user(ubuf, buf, n))
		ret = -EFAULT;
	else
		*ppos += n;
	kvfree(buf);
	return ret;
}

static ssize_t sgxfive_render_param_read(struct file *f, char __user *ubuf, size_t len,
					 loff_t *ppos)
{
	return sgxfive_render_dump_read(f, ubuf, len, ppos, false);
}

static ssize_t sgxfive_render_tadata_read(struct file *f, char __user *ubuf, size_t len,
					  loff_t *ppos)
{
	return sgxfive_render_dump_read(f, ubuf, len, ppos, true);
}

/* debugfs render_kept: the TA data of the newest kept render context (render_keep). */
static ssize_t sgxfive_render_kept_read(struct file *f, char __user *ubuf, size_t len,
					loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	ssize_t ret;
	size_t n;
	void *buf;

	len = min_t(size_t, len, SZ_64K);
	buf = kvmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	n = sgxfive_keep_read(&sgx->kept, buf, len, *ppos);
	ret = n;
	if (n && copy_to_user(ubuf, buf, n))
		ret = -EFAULT;
	else
		*ppos += n;
	kvfree(buf);
	return ret;
}

static const struct file_operations sgxfive_render_kept_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = sgxfive_render_kept_read,
	.llseek = default_llseek,
};

static const struct file_operations sgxfive_render_param_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = sgxfive_render_param_read,
	.llseek = default_llseek,
};

static const struct file_operations sgxfive_render_tadata_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = sgxfive_render_tadata_read,
	.llseek = default_llseek,
};

/*
 * debugfs render_words: the newest render context's HW context, RT data set,
 * PB descriptor, the kernel's words for it and its render details, as they
 * are now, a word a line spelt as kpoke spells it (sgxfive_render_words).
 * Root only.
 */
static int sgxfive_render_words_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct sgxfive_ctx *ctx = sgxfive_render_newest(sgx);

	if (!ctx)
		return -ENODEV;
	sgxfive_render_words(ctx, &p);
	sgxfive_ctx_put(ctx);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_render_words);

/* The firmware buffer at a device address, or NULL. */
static struct sgxfive_buf *sgxfive_buf_at(struct sgxfive *sgx, u32 addr)
{
	unsigned int i;

	for (i = 0; addr && i < SGX_FW_MAX_BUFFERS; i++)
		if (sgx->buf[i].va && sgx->buf[i].addr == addr)
			return &sgx->buf[i];
	return NULL;
}

/*
 * debugfs hwperf: set HostCtl's counter group, then send the firmware's
 * SETHWPERFSTATUS command with word as its status. The firmware's handler
 * first writes 0xffffffff to word 2 of the HWPerf buffer TA3DCtl dword 43
 * points at, which is the acknowledgement waited for here; with no such
 * buffer mapped the command is not sent. Only while the firmware runs and has
 * no render-node job; logged with what the HWPerf control register reads
 * after.
 */
int sgxfive_hwperf_set(struct sgxfive *sgx, u32 group, u32 word)
{
	const u32 data[] = { word };
	struct sgxfive_buf *cb;
	u32 *hc, *ack;
	int ret;

	if (!READ_ONCE(sgx->live))
		return -ENODEV;
	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		ret = -ENODEV;
		goto out;
	}
	if (sgxfive_oldest(sgx) != U64_MAX) {
		ret = -EBUSY;
		goto out;
	}
	cb = sgxfive_buf_at(sgx, READ_ONCE(sgxfive_words(sgx, BUF_TA3D_CTL)[43]));
	if (!cb) {
		ret = -ENXIO;
		goto out;
	}
	hc = sgxfive_words(sgx, BUF_HOST_CTL);
	ack = (u32 *)cb->va + 2;
	WRITE_ONCE(*ack, 0);
	WRITE_ONCE(hc[HC_PERF_GROUP], group);
	wmb(); /* the group and the cleared ack, before the command */
	ret = sgxfive_command(sgx, SGX_CMD_SETHWPERFSTATUS, data, ARRAY_SIZE(data));
	if (!ret)
		ret = sgxfive_wait_mem(ack, ~0u, ~0u);
	drm_info(sgx->dev, "sgxfive: hwperf group %u word 0x%08x: %d, control 0x%08x\n",
		 group, word, ret, sgxfive_rd(sgx, sgxfive_hwperf_regs[0]));
out:
	mutex_unlock(&sgx->lock);
	return ret;
}

/* Reading shows HostCtl's group and the HWPerf registers, "0xOFFS 0xVALUE" a line. */
static int sgxfive_hwperf_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	unsigned int i;
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (!sgx->live)
		ret = -ENODEV;
	if (!ret)
		seq_printf(m, "group %u\n",
			   READ_ONCE(sgxfive_words(sgx, BUF_HOST_CTL)[HC_PERF_GROUP]));
	for (i = 0; !ret && i < SGXFIVE_HWPERF_NREGS; i++)
		seq_printf(m, "0x%04x 0x%08x\n", sgxfive_hwperf_regs[i],
			   sgxfive_rd(sgx, sgxfive_hwperf_regs[i]));
	mutex_unlock(&sgx->lock);
	return ret;
}

static int sgxfive_hwperf_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_hwperf_show, inode->i_private);
}

/* One line: GROUP WORD. */
static ssize_t sgxfive_hwperf_fwrite(struct file *f, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	char buf[SGXFIVE_KPOKE_LINE];
	u32 group, word;
	int ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	ret = sgxfive_hwperf_parse(strim(buf), &group, &word);
	if (!ret)
		ret = sgxfive_hwperf_set(sgx, group, word);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_hwperf_fops = {
	.owner = THIS_MODULE, .open = sgxfive_hwperf_open, .read = seq_read,
	.write = sgxfive_hwperf_fwrite, .llseek = seq_lseek, .release = single_release,
};

void sgxfive_cleanup_start(struct sgxfive_cleanup_trace *t, u32 rtds, u32 ctx,
			   u32 pbdesc, u32 flags)
{
	*t = (struct sgxfive_cleanup_trace){
		.rtds = rtds, .ctx = ctx, .pbdesc = pbdesc, .flags = flags,
	};
}

/* The slots now; past the last step nothing more is kept. */
void sgxfive_cleanup_note(struct sgxfive_cleanup_trace *t, const u32 *ta3dctl)
{
	if (t->steps >= SGXFIVE_CLEANUP_STEPS)
		return;
	t->slots[t->steps][0] = READ_ONCE(ta3dctl[SGXFIVE_TA3D_PB_TA]);
	t->slots[t->steps][1] = READ_ONCE(ta3dctl[SGXFIVE_TA3D_PB_3D]);
	t->steps++;
}

void sgxfive_cleanup_print(struct drm_printer *p, const struct sgxfive_cleanup_trace *t)
{
	static const char *const step[SGXFIVE_CLEANUP_STEPS] = { "before", "rt", "rc", "pb" };
	unsigned int i;

	drm_printf(p, "rtds %#010x ctx %#010x pbdesc %#010x flags %#010x ret %d\n",
		   t->rtds, t->ctx, t->pbdesc, t->flags, t->ret);
	for (i = 0; i < t->steps; i++)
		drm_printf(p, "%s ta %#010x 3d %#010x\n", step[i], t->slots[i][0],
			   t->slots[i][1]);
}

/* The last render-context cleanup: the loaded-PB slots around its steps. */
static int sgxfive_render_cleanup_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);

	mutex_lock(&sgx->lock);
	sgxfive_cleanup_print(&p, &sgx->cleanup);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_render_cleanup);

/*
 * debugfs alias_skip (sgxfive_alias_firmware): a mask of firmware buffer ids.
 * A bit past the last id (SGX_FW_MAX_BUFFERS) names nothing: refused.
 */
static int sgxfive_alias_skip_get(void *data, u64 *val)
{
	struct sgxfive *sgx = data;

	*val = READ_ONCE(sgx->alias_skip);
	return 0;
}

int sgxfive_alias_skip_set(void *data, u64 val)
{
	struct sgxfive *sgx = data;

	if (val & ~GENMASK_ULL(SGX_FW_MAX_BUFFERS - 1, 0))
		return -EINVAL;
	WRITE_ONCE(sgx->alias_skip, val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(sgxfive_alias_skip_fops, sgxfive_alias_skip_get,
			 sgxfive_alias_skip_set, "0x%08llx\n");

/*
 * debugfs own_code, own_res and own_arm: a firmware task of our own (sgxfive_owntask.h).
 *
 * own_code takes a blob of tools/fwtask, once per write, and keeps it only if the verifier
 * accepts it (-EPERM, or -EINVAL for a malformed one, and nothing stored otherwise). own_arm
 * names the slots of PDS #13 to point at it at the next load, "slots=<mask> entry=<n>,<n>
 * trc=<t>", or "0" to take it back; it is 0 at every boot of the module and is cleared by every
 * recovery reset. own_res is the results page the task and the tester share, in the EDM's page
 * directory at KERNEL_DATA + 0x140000: pread/pwrite, dword aligned, dword 0 (the header) is the
 * kernel's. The overlay itself is sgxfive_own_apply(), called from the load and nowhere else.
 */
static ssize_t sgxfive_own_code_write(struct file *f, const char __user *ubuf, size_t len,
				      loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	struct sgx_own_blob b;
	const char *why = "";
	u8 *data;
	int ret;

	if (*ppos || !len || len > SGX_OWN_MAX_BYTES)
		return -EINVAL;
	data = memdup_user(ubuf, len);
	if (IS_ERR(data))
		return PTR_ERR(data);
	ret = sgx_own_parse(data, len, &b);
	if (!ret)
		ret = sgx_own_verify(&b, &why);
	if (ret) {
		drm_info(sgx->dev, "sgxfive: own_code refused (%d): %s\n", ret, why);
		kfree(data);
		return ret;
	}
	mutex_lock(&sgx->lock);
	memcpy(sgx->own.blob, data, len);
	sgx->own.len = len;
	sgx->own.armed = false;		/* the new blob has to be armed again */
	mutex_unlock(&sgx->lock);
	kfree(data);
	drm_info(sgx->dev, "sgxfive: own_code: %zu bytes accepted\n", len);
	return len;
}

static const struct file_operations sgxfive_own_code_fops = {
	.owner = THIS_MODULE, .write = sgxfive_own_code_write,
};

static ssize_t sgxfive_own_res_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	ssize_t ret;

	mutex_lock(&sgx->lock);
	if (!sgx->loaded || !sgx->xt.buf[XB_OWNRES].va)
		ret = -ENODEV;
	else
		ret = simple_read_from_buffer(ubuf, len, ppos, sgx->xt.buf[XB_OWNRES].va,
					      SGX_OWN_RES_SIZE);
	mutex_unlock(&sgx->lock);
	return ret;
}

static ssize_t sgxfive_own_res_write(struct file *f, const char __user *ubuf, size_t len,
				     loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	ssize_t ret = len;
	u8 *data;

	if (!sgx_own_res_write_ok(*ppos, len))
		return *ppos < 4 ? -EPERM : -EINVAL;
	/* copied from the user before the lock: a page fault must not happen inside the mutex */
	data = memdup_user(ubuf, len);
	if (IS_ERR(data))
		return PTR_ERR(data);
	mutex_lock(&sgx->lock);
	if (!sgx->loaded || !sgx->xt.buf[XB_OWNRES].va) {
		ret = -ENODEV;
	} else {
		memcpy((u8 *)sgx->xt.buf[XB_OWNRES].va + *ppos, data, len);
		wmb();		/* the page is write-combined */
		*ppos += len;
	}
	mutex_unlock(&sgx->lock);
	kfree(data);
	return ret;
}

static const struct file_operations sgxfive_own_res_fops = {
	.owner = THIS_MODULE, .read = sgxfive_own_res_read, .write = sgxfive_own_res_write,
	.llseek = default_llseek,
};

static ssize_t sgxfive_own_arm_write(struct file *f, const char __user *ubuf, size_t len,
				     loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	struct sgx_own_blob b;
	struct sgx_own_arm a;
	char buf[96];
	int ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	strim(buf);
	if (!strcmp(buf, "0") || !strcmp(buf, "clear")) {
		WRITE_ONCE(sgx->own.armed, false);
		return len;
	}
	ret = sgx_own_arm_parse(buf, &a);
	if (ret)
		return ret;
	mutex_lock(&sgx->lock);
	ret = sgx->own.len ? sgx_own_parse(sgx->own.blob, sgx->own.len, &b) : -ENOENT;
	if (!ret) {
		unsigned int i;

		for (i = 0; i < a.nentry; i++)
			if (a.entry[i] >= b.n_entry)
				ret = -EINVAL;
	}
	if (!ret) {
		sgx->own.arm = a;
		sgx->own.armed = true;
	}
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static int sgxfive_own_arm_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct sgx_own_arm a;
	bool armed;
	size_t len;
	unsigned int i;

	mutex_lock(&sgx->lock);
	armed = sgx->own.armed;
	a = sgx->own.arm;
	len = sgx->own.len;
	mutex_unlock(&sgx->lock);
	if (!armed) {
		seq_printf(m, "off blob=%zu\n", len);
		return 0;
	}
	seq_printf(m, "slots=%#x entry=", a.slots);
	for (i = 0; i < a.nentry; i++)
		seq_printf(m, "%s%u", i ? "," : "", a.entry[i]);
	if (a.trc)
		seq_printf(m, " trc=%u", a.trc);
	else
		seq_puts(m, " trc=stock");
	seq_printf(m, " trc_hazard=%d blob=%zu\n", a.trc_hazard, len);
	return 0;
}

static int sgxfive_own_arm_open(struct inode *inode, struct file *f)
{
	return single_open(f, sgxfive_own_arm_show, inode->i_private);
}

static const struct file_operations sgxfive_own_arm_fops = {
	.owner = THIS_MODULE, .open = sgxfive_own_arm_open, .read = seq_read,
	.write = sgxfive_own_arm_write, .llseek = seq_lseek, .release = single_release,
};

/*
 * Called from the firmware load, once the buffers are built (not from a recovery reset, which
 * builds the stock ones): the results page's header, and, when armed, the overlay.
 */
int sgxfive_debug_own_apply(struct sgxfive *sgx, u8 *const bufs[])
{
	struct sgxfive_own *own = &sgx->own;
	u8 digest[SHA256_DIGEST_SIZE];
	struct sgx_own_arm arm;
	struct sgx_own_blob b;
	int ret;

	lockdep_assert_held(&sgx->lock);	/* the blob and the arming are read without more */
	if (sgx->xt.buf[XB_OWNRES].va)
		*(u32 *)sgx->xt.buf[XB_OWNRES].va = SGX_OWN_MAGIC_DWORD;
	if (!READ_ONCE(own->armed))
		return 0;
	/* the shared exit (a cont4 patch) only for the firmware file it was read from */
	arm = own->arm;
	arm.exit_index = 0;
	if (sgx->blob) {
		sha256(sgx->blob->data, sgx->blob->size, digest);
		arm.exit_index = sgx_own_exit_index(digest);
	}
	ret = sgx_own_parse(own->blob, own->len, &b);
	if (!ret && !sgx_fw_buffer(&sgx->fw, SGXFIVE_OWN_PDS_BUF))
		ret = -ENOENT;
	if (!ret)
		ret = sgx_own_apply(&b, &arm, &sgx->fw, &sgx->layout, SGXFIVE_OWN_PDS_BUF,
				    bufs[SGXFIVE_OWN_PDS_BUF],
				    sgx_fw_buffer(&sgx->fw, SGXFIVE_OWN_PDS_BUF)->size,
				    sgx->xt.buf[XB_OWNCODE].va, sgx->xt.buf[XB_OWNRES].addr);
	if (ret)
		drm_err(sgx->dev, "sgxfive: own task not armed: %d\n", ret);
	else
		drm_info(sgx->dev, "sgxfive: own task armed: slots %#x, trc %u (0: stock), exit %#x (0: unknown) (debugfs own_arm)\n",
			 own->arm.slots, own->arm.trc, arm.exit_index);
	return ret;
}

void sgxfive_debug_init(struct sgxfive *sgx, struct dentry *dir)
{
	debugfs_create_file("miscinfo", 0400, dir, sgx, &sgxfive_miscinfo_fops);
	debugfs_create_file("idle", 0200, dir, sgx, &sgxfive_idle_fops);
	debugfs_create_file("xfer_dummy", 0400, dir, sgx,
			    &sgxfive_xfer_dummy_fops);
	debugfs_create_file("xfer_sync", 0400, dir, sgx, &sgxfive_xfer_sync_fops);
	debugfs_create_file("xfer_fill", 0400, dir, sgx, &sgxfive_xfer_fill_fops);
	debugfs_create_file("xfer_copy", 0400, dir, sgx, &sgxfive_xfer_copy_fops);
	debugfs_create_file("xfer_iso", 0400, dir, sgx, &sgxfive_xfer_iso_fops);
	debugfs_create_bool("alias_ro", 0600, dir, &sgx->xt.alias_ro);
	debugfs_create_bool("no_alias", 0600, dir, &sgx->xt.no_alias);
	debugfs_create_file("xfer_ro", 0400, dir, sgx, &sgxfive_xfer_ro_fops);
	debugfs_create_file("reset", 0200, dir, sgx, &sgxfive_reset_fops);
	debugfs_create_bool("alias_ta3d_ro", 0600, dir, &sgx->alias_ta3d_ro);
	/*
	 * Firmware buffers (ids 0-15) left out of the alias of the render VMs
	 * made from now on, until written again: a reset leaves the mask
	 * alone, like alias_ta3d_ro. Bits 16-31 name no buffer: -EINVAL.
	 */
	debugfs_create_file_unsafe("alias_skip", 0600, dir, sgx, &sgxfive_alias_skip_fops);
	debugfs_create_bool("alias_ctx_rw", 0600, dir, &sgx->alias_ctx_rw);
	debugfs_create_bool("render_share_pb", 0600, dir, &sgx->render_share_pb);
	debugfs_create_bool("render_keep", 0600, dir, &sgx->render_keep);
	spin_lock_init(&sgx->kpoke_lock);
	sgxfive_hostirq_init(&sgx->hostirq);
	debugfs_create_file("kpoke", 0600, dir, sgx, &sgxfive_kpoke_fops);
	debugfs_create_file("kreg", 0600, dir, sgx, &sgxfive_kreg_fops);
	debugfs_create_file("tadata_fill", 0600, dir, sgx, &sgxfive_tdfill_fops);
	debugfs_create_file("kregnow", 0600, dir, sgx, &sgxfive_kregnow_fops);
	debugfs_create_file("kicker", 0600, dir, sgx, &sgxfive_kicker_fops);
	debugfs_create_bool("kick_no_bump", 0600, dir, &sgx->kick_no_bump);
	/* Pages of the PB of render contexts created from now on. */
	sgx->render_pb_pages = SGX_RENDER_PB_PAGES;
	debugfs_create_x32("render_pb_pages", 0600, dir, &sgx->render_pb_pages);
	/*
	 * Region-header arrays before the ZLS buffer in the TA data of render contexts created
	 * from now on: 1 (the default), 2 or 4; any other value is taken as 1. A maximum whose
	 * TA data would then pass its 2 MiB slot is refused at the context's creation (-E2BIG).
	 */
	sgx->render_rgn_arrays = 1;
	debugfs_create_x32("render_rgn_arrays", 0600, dir, &sgx->render_rgn_arrays);
	debugfs_create_file("render_param", 0400, dir, sgx, &sgxfive_render_param_fops);
	debugfs_create_file("render_tadata", 0400, dir, sgx, &sgxfive_render_tadata_fops);
	debugfs_create_file("render_words", 0400, dir, sgx, &sgxfive_render_words_fops);
	debugfs_create_file("render_kept", 0400, dir, sgx, &sgxfive_render_kept_fops);
	debugfs_create_bool("vm_low", 0600, dir, &sgx->vm_low);
	debugfs_create_file("hwperf", 0600, dir, sgx, &sgxfive_hwperf_fops);
	/* Flags besides NEW of render contexts created from now on. */
	debugfs_create_x32("render_ctx_flags", 0600, dir, &sgx->render_ctx_flags);
	debugfs_create_file("render_cleanup", 0400, dir, sgx, &sgxfive_render_cleanup_fops);
	/* HostCtl's timer clock for the next boot; 0 for the default. */
	debugfs_create_x32("timer_clock", 0600, dir, &sgx->timer_clock);
	/* Hangs and faults reported, not recovered, until reset is written. */
	debugfs_create_bool("hold_recovery", 0600, dir, &sgx->hold_recovery);
	/* Renders leave the RT data set's size words and counters as they are. */
	debugfs_create_bool("rtds_keep", 0600, dir, &sgx->rtds_keep);
	/* Our own firmware task: its code, its results page and what is armed (sgxfive_owntask.h). */
	debugfs_create_file("own_code", 0200, dir, sgx, &sgxfive_own_code_fops);
	debugfs_create_file("own_res", 0600, dir, sgx, &sgxfive_own_res_fops);
	debugfs_create_file("own_arm", 0600, dir, sgx, &sgxfive_own_arm_fops);
}
