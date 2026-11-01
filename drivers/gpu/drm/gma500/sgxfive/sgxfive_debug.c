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
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <linux/wait.h>

#include <drm/drm_print.h>

#include "sgxfive_priv.h"
#include "sgx_seq.h"
#include "sgx_xfer.h"

/*
 * The test area, in KERNEL_DATA above the firmware's buffers so the EDM's
 * own page directory maps it: a 64 KiB client CCB (plus room for a command
 * at its end), its control block, one HW transfer context and a scratch
 * page: U (update target), F (status target) and, from SCR_SYNC_READ, the
 * read- and write-complete counters of a destination sync.
 */
enum { SCR_U = 0, SCR_F = 1, SCR_SYNC_READ = 8, SCR_SYNC_WRITE = 9 };
enum { XB_CCB, XB_CCB_CTL, XB_CTX, XB_SCRATCH, XB_COUNT };

/* By heap and offset, so the area follows the layout. */
static const struct {
	u8 heap;
	u32 off, size;
} sgxfive_xbuf[XB_COUNT] = {
	[XB_CCB] = { HEAP_KERNEL_DATA, 0x100000,
		     SGX_XFER_CCB_SIZE + SGX_XFER_CMD_SIZE },
	[XB_CCB_CTL] = { HEAP_KERNEL_DATA, 0x120000, 8 },
	[XB_CTX] = { HEAP_KERNEL_DATA, 0x121000, SGX_XFER_CTX_SIZE },
	[XB_SCRATCH] = { HEAP_KERNEL_DATA, 0x122000, 0x40 },
};

static_assert(XB_COUNT <= SGXFIVE_XBUFS);

int sgxfive_debug_load(struct sgxfive *sgx)
{
	unsigned int i;
	int ret;

	for (i = 0; i < XB_COUNT; i++) {
		ret = sgxfive_map_pd(sgx, sgx->pd, &sgx->xt.buf[i],
				     sgx->layout.heap_base[sgxfive_xbuf[i].heap] +
				     sgxfive_xbuf[i].off, sgxfive_xbuf[i].size);
		if (ret)
			return ret;
	}
	return 0;
}

void sgxfive_debug_unload(struct sgxfive *sgx)
{
	unsigned int i;

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

/* SGXMKIF_CMD_GETMISCINFO's request and its reply in the misc-info buffer. */
#define MISCINFO_READY			0x1
#define MISCINFO_GET_STRUCT_SIZES	0x2
#define MISCINFO_FEATURES		7
#define MISCINFO_STRUCT_SIZES		13
#define MISCINFO_DWORDS		(1 + MISCINFO_FEATURES + MISCINFO_STRUCT_SIZES)

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
 * GETMISCINFO with GET_STRUCT_SIZES: the EDM fills in the core and DDK
 * identity and the sizes of the structures it was built with. Check every
 * value against the registers and the firmware file.
 */
static int sgxfive_miscinfo(struct sgxfive *sgx, struct drm_printer *p)
{
	u32 *mi = sgxfive_words(sgx, BUF_MISC);
	const u32 data[] = { 0, sgx->layout.addr[BUF_MISC] };
	u32 want[MISCINFO_FEATURES];
	ktime_t start;
	int i, ret, bad = 0;

	if (!sgx->live)
		return -ENODEV;
	for (i = 1; i < MISCINFO_DWORDS; i++)
		WRITE_ONCE(mi[i], 0);
	WRITE_ONCE(mi[0], MISCINFO_GET_STRUCT_SIZES);
	ret = sgxfive_command(sgx, SGX_CMD_GETMISCINFO, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	start = ktime_get();
	ret = sgxfive_wait_mem(&mi[0], MISCINFO_READY, MISCINFO_READY);
	if (ret) {
		drm_printf(p, "no MISCINFO_READY after 500 ms\n");
		sgxfive_dump(sgx, p);
		return ret;
	}
	drm_printf(p, "GETMISCINFO answered after %lld us\n",
		   ktime_us_delta(ktime_get(), start));

	want[0] = sgxfive_rd(sgx, SGX_CORE_REVISION);
	want[1] = sgxfive_rd(sgx, SGX_CORE_ID);
	/* major, minor and branch: DDK 1.7 is branch 17 */
	want[2] = sgx->fw.ddk_major << 16 | sgx->fw.ddk_minor << 8 | 17;
	want[3] = sgx->fw.ddk_build;
	want[4] = 545;
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

void sgxfive_debug_init(struct sgxfive *sgx, struct dentry *dir)
{
	debugfs_create_file("miscinfo", 0400, dir, sgx, &sgxfive_miscinfo_fops);
	debugfs_create_file("idle", 0200, dir, sgx, &sgxfive_idle_fops);
	debugfs_create_file("xfer_dummy", 0400, dir, sgx,
			    &sgxfive_xfer_dummy_fops);
	debugfs_create_file("xfer_sync", 0400, dir, sgx, &sgxfive_xfer_sync_fops);
}
