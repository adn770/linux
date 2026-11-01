// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * SGX545 microkernel bring-up for Cedarview (GMA3600/3650).
 *
 * Loads sgx545_ukernel.fw (see sgx_fw.c), places its buffers in the SGX
 * address space with the DDK's heap layout and maps them through a private
 * page directory, the one the EDM will use. Nothing runs at probe: writing to
 * debugfs dri/N/sgx/load does the work, and the buffers can then be read back
 * from dri/N/sgx/bufNN to check them against the reference loader.
 *
 * dri/N/sgx/boot then runs the DDK's first power-on sequence (sgx_seq.c) and
 * waits for the EDM to report init complete in HostCtl; dri/N/sgx/status shows
 * the state and dri/N/sgx/stop puts the core back into reset. The row labels
 * in the comments (B1, C7, ...) are those of resgx545's
 * doc/specs/boot_sequence.md.
 *
 * The render node (sgxfive_drv.c) loads and boots the firmware itself when
 * its first context is created, and keeps the memory of its contexts in the
 * EDM's address space (sgxfive_kmem_alloc).
 * When a job faults or hangs, the firmware is reset in place and the
 * contexts that did not cause it start over (sgxfive_recover).
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <asm/set_memory.h>

#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>

#include "sgxfive.h"
#include "sgxfive_coredump.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"
#include "sgxfive_regdump.h"
#include "sgx_fw.h"
#include "sgx_mmu.h"
#include "sgx_seq.h"
#include "sgx_xfer.h"

#define SGX_FW_NAME	"sgx545_ukernel.fw"

/*
 * The GPU address layout, by sgxapi_km.h heap id: the DDK's SGX545 REV1 table
 * (sgxconfig.h), which Intel's Cedarview kernel uses when the host bridge is
 * below revision 4 (FIX_HW_BRN_SAMPLE_CACHE): every heap below 0x3e000000 and
 * textures in a heap of their own (resgx545's doc/specs/boot_sequence.md (d)).
 */
static const u32 sgx_heap_base[] = {
	[HEAP_KERNEL_CODE] = 0x30000000,
	[HEAP_KERNEL_DATA] = 0x32000000,
	[HEAP_PIXELSHADER] = 0x37000000,
	[HEAP_VERTEXSHADER] = 0x3c000000,
	[HEAP_PDSPIXEL] = 0x2e000000,
	[HEAP_TEXTURE] = 0xf0000000,
};

/*
 * srvinit's allocation order: the control blocks, then the ukernel and the
 * five PDS programs. The ukernel must start exactly at the EDM code base: its
 * branch targets and the CCB service addresses are instruction indices from
 * that base, and srvinit relocates only the PDS code words. So the first
 * buffer of each heap goes at the heap base (debugfs sgx/heap_start moves it).
 */
static const u8 sgx_alloc_order[] = {
	6, 7, 8, 9, 10, 11, 12, 13, 14, 0, 1, 2, 3, 4, 5,
};

static u32 sgx_rd(struct sgxfive *sgx, u32 reg)
{
	return ioread32(sgx->regs + reg);
}

static void sgx_wr(struct sgxfive *sgx, u32 reg, u32 val)
{
	iowrite32(val, sgx->regs + reg);
}

#define TA3D_CTL_DWORDS		(0xec / 4)
static_assert(HC_DWORDS == SGXFIVE_HC_DWORDS);
static_assert(TA3D_CTL_DWORDS == SGXFIVE_TA3D_CTL_DWORDS);
#define EDM_INIT_COMPLETE	0x1
#define SGX_CLOCK_HZ		400000000	/* SYS_SGX_CLOCK_SPEED */

void sgxfive_unmap(struct sgxfive *sgx, struct sgxfive_buf *b)
{
	unsigned int i;

	if (!b->pages)
		return;
	if (b->pd)
		sgx_mmu_unmap(b->pd, b->addr, b->npages);
	if (b->va)
		vunmap(b->va);
	if (b->wc)
		set_pages_array_wb(b->pages, b->npages);
	for (i = 0; i < b->npages; i++)
		if (b->pages[i])
			__free_page(b->pages[i]);
	kfree(b->pages);
	memset(b, 0, sizeof(*b));
}

static void sgxfive_stop(struct sgxfive *sgx);

static void sgxfive_unload(struct sgxfive *sgx)
{
	unsigned int id;

	sgxfive_stop(sgx);

	sgxfive_debug_unload(sgx);
	for (id = 0; id < SGX_FW_MAX_BUFFERS; id++)
		sgxfive_unmap(sgx, &sgx->buf[id]);
	if (sgx->pd)
		sgx_mmu_pd_destroy(sgx->pd);
	sgx->pd = NULL;
	release_firmware(sgx->blob);
	sgx->blob = NULL;
	memset(&sgx->layout, 0, sizeof(sgx->layout));
	sgx->loaded = false;
}

/*
 * Allocate a zeroed buffer write-combined, map it for the CPU and in pd, with the page flags
 * (0, or SGX_MMU_RO for a buffer the SGX only reads: the CPU alias stays writable).
 */
int sgxfive_map_pd(struct sgxfive *sgx, struct sgx_mmu_pd *pd,
		   struct sgxfive_buf *b, u32 addr, u32 size, u32 flags)
{
	unsigned int i;
	int ret;

	b->npages = DIV_ROUND_UP(size, PAGE_SIZE);
	b->pages = kcalloc(b->npages, sizeof(*b->pages), GFP_KERNEL);
	if (!b->pages)
		return -ENOMEM;
	for (i = 0; i < b->npages; i++) {
		b->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!b->pages[i])
			return -ENOMEM;
	}
	set_pages_array_wc(b->pages, b->npages);
	b->wc = true;
	b->va = vmap(b->pages, b->npages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!b->va)
		return -ENOMEM;
	ret = sgx_mmu_map(pd, addr, b->pages, b->npages, flags);
	if (ret)
		return ret;
	b->addr = addr;
	b->pd = pd;
	b->blob.data = b->va;
	b->blob.size = size;
	return 0;
}

/* ...in the EDM's own (kernel) page directory. */
static int sgxfive_map(struct sgxfive *sgx, struct sgxfive_buf *b, u32 addr,
		       u32 size)
{
	return sgxfive_map_pd(sgx, sgx->pd, b, addr, size, 0);
}

static int sgxfive_load(struct sgxfive *sgx)
{
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { NULL };
	struct sgx_fw_layout *l = &sgx->layout;
	unsigned int i;
	int ret;

	ret = request_firmware(&sgx->blob, SGX_FW_NAME, sgx->dev->dev);
	if (ret)
		return ret;
	ret = sgx_fw_parse(&sgx->fw, sgx->blob->data, sgx->blob->size);
	if (ret)
		goto err;

	for (i = 0; i < ARRAY_SIZE(sgx_heap_base); i++) {
		if (sgx_heap_base[i]) {
			l->heap_base[i] = sgx_heap_base[i];
			l->heap_set |= 1u << i;
		}
	}
	ret = sgx_fw_place(sgx->fw.buffers, sgx->fw.nbuffers, sgx_alloc_order,
			   ARRAY_SIZE(sgx_alloc_order), sgx->heap_start, PAGE_SIZE,
			   l);
	if (ret)
		goto err;

	/* Faults trap rather than hit a dummy page, so a bad access shows. */
	sgx->pd = sgx_mmu_pd_create();
	if (!sgx->pd) {
		ret = -ENOMEM;
		goto err;
	}
	l->pd_phys = sgx_mmu_pd_phys(sgx->pd);

	for (i = 0; i < sgx->fw.nbuffers; i++) {
		const struct sgx_fw_buffer *fb = &sgx->fw.buffers[i];

		ret = sgxfive_map(sgx, &sgx->buf[fb->id], l->addr[fb->id],
				  fb->size);
		if (ret)
			goto err;
		bufs[fb->id] = sgx->buf[fb->id].va;
	}
	ret = sgxfive_debug_load(sgx);
	if (ret)
		goto err;
	ret = sgx_fw_build(&sgx->fw, l, bufs);
	if (ret)
		goto err;
	ret = sgxfive_debug_own_apply(sgx, bufs);
	if (ret)
		goto err;
	/* The buffers are write-combined: drain them before the SGX reads. */
	wmb();

	sgx->loaded = true;
	drm_info(sgx->dev, "SGX firmware DDK %u.%u.%u loaded, EDM PD at %#x\n",
		 sgx->fw.ddk_major, sgx->fw.ddk_minor, sgx->fw.ddk_build,
		 l->pd_phys);
	return 0;
err:
	sgxfive_unload(sgx);
	return ret;
}

static ssize_t sgxfive_load_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	if (sgx->kmem_users) {
		ret = -EBUSY;	/* contexts live in the EDM's page directory */
	} else {
		if (sgx->loaded)
			sgxfive_unload(sgx);
		ret = sgxfive_load(sgx);
	}
	mutex_unlock(&sgx->lock);
	if (ret)
		drm_err(sgx->dev, "SGX firmware load failed: %d\n", ret);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_load_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_load_write,
};

/*
 * The placement and PD the buffers were built for, as the layout JSON of
 * resgx545's tools/fwcutter/fwload.py.
 */
static int sgxfive_layout_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	const struct sgx_fw_layout *l = &sgx->layout;
	unsigned int i;
	const char *sep = "";

	mutex_lock(&sgx->lock);
	if (!sgx->loaded) {
		mutex_unlock(&sgx->lock);
		return -ENODATA;
	}
	seq_puts(m, "{\"addr\": {");
	for (i = 0; i < SGX_FW_MAX_BUFFERS; i++) {
		if (l->addr_set & 1u << i) {
			seq_printf(m, "%s\"%u\": \"%#x\"", sep, i, l->addr[i]);
			sep = ", ";
		}
	}
	seq_puts(m, "}, \"heap_base\": {");
	sep = "";
	for (i = 0; i < SGX_FW_MAX_HEAPS; i++) {
		if (l->heap_set & 1u << i) {
			seq_printf(m, "%s\"%u\": \"%#x\"", sep, i, l->heap_base[i]);
			sep = ", ";
		}
	}
	seq_printf(m, "}, \"pd_phys\": \"%#x\"}\n", l->pd_phys);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_layout);

static u32 seq_rd(void *ctx, u32 reg)
{
	return sgx_rd(ctx, reg);
}

static void seq_wr(void *ctx, u32 reg, u32 val)
{
	sgx_wr(ctx, reg, val);
	(void)sgx_rd(ctx, reg);		/* post */
}

static void seq_udelay(void *ctx, unsigned int us)
{
	udelay(us);
}

static void seq_wmb(void *ctx)
{
	wmb(); /* the command and the offsets before the kick */
}

/* C7, C9-C11: the kicker word, the host clock, InitStatus, then a barrier. */
static void seq_pre_kick(void *ctx)
{
	struct sgxfive *sgx = ctx;
	u32 *hc = sgxfive_words(sgx, BUF_HOST_CTL);

	WRITE_ONCE(*sgxfive_words(sgx, BUF_KICKER), 0);
	WRITE_ONCE(hc[HC_HOST_CLOCK], (u32)(jiffies * (1000000 / HZ)));
	WRITE_ONCE(hc[HC_INIT_STATUS], 0);
	wmb(); /* the EDM must see all of the above before the kick */
}

/* Poll a dword the EDM writes until (v & mask) == want, for up to 500 ms. */
int sgxfive_wait_mem(const u32 *w, u32 mask, u32 want)
{
	ktime_t start = ktime_get();

	while ((READ_ONCE(*w) & mask) != want) {
		if (ktime_ms_delta(ktime_get(), start) > 500)
			return -ETIMEDOUT;
		usleep_range(50, 100);
		rmb(); /* re-read from memory */
	}
	return 0;
}

static const u32 sgxfive_dump_regs[] = {
	SGX_CORE_ID, SGX_CORE_REVISION, SGX_CLKGATECTL, SGX_CLKGATESTATUS,
	SGX_SOFT_RESET, SGX_EVENT_STATUS, SGX_EVENT_STATUS2,
	SGX_EVENT_HOST_ENABLE, SGX_BIF_CTRL, SGX_BIF_BANK0, SGX_BIF_INT_STAT,
	SGX_BIF_FAULT, SGX_BIF_MEM_REQ_STAT, SGX_BIF_DIR_LIST_BASE0,
	SGX_BIF_DIR_LIST_BASE15, SGX_USE0_SERV_EVENT, SGX_USE1_SERV_EVENT,
};

static_assert(ARRAY_SIZE(sgxfive_dump_regs) == SGXFIVE_DUMP_NREGS);

/* What SGXDumpDebugInfo shows: registers, HostCtl, TA3DCtl, CCB control. */
void sgxfive_devstate(struct sgxfive *sgx, struct sgxfive_devstate *d)
{
	unsigned int i;

	d->loaded = sgx->loaded;
	d->live = sgx->live;
	d->sw_events = atomic_read(&sgx->sw_events);
	d->bif_faults = atomic_read(&sgx->bif_faults);
	d->ccb_sent = sgx->ccb_sent;
	d->ndeferred = sgx->ndeferred;
	for (i = 0; i < ARRAY_SIZE(sgxfive_dump_regs); i++) {
		d->regs[i].reg = sgxfive_dump_regs[i];
		d->regs[i].val = sgx_rd(sgx, sgxfive_dump_regs[i]);
	}
	if (!sgx->loaded)
		return;
	rmb(); /* fresh reads of what the EDM wrote */
	memcpy(d->hostctl, sgxfive_words(sgx, BUF_HOST_CTL), sizeof(d->hostctl));
	memcpy(d->ta3dctl, sgxfive_words(sgx, BUF_TA3D_CTL), sizeof(d->ta3dctl));
	memcpy(d->ccbctl, sgxfive_words(sgx, BUF_CCB_CTL), sizeof(d->ccbctl));
}

static void sgxfive_print_words(struct drm_printer *p, const char *name,
				const u32 *w, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i += 8)
		drm_printf(p, "%s[%2u]: %*ph\n", name, i,
			   (int)(min(8u, n - i) * 4), &w[i]);
}

void sgxfive_devstate_print(struct drm_printer *p, const struct sgxfive_devstate *d)
{
	unsigned int i;

	drm_printf(p, "loaded %d live %d sw_events %d bif_faults %d ccb_sent %llu deferred %u\n",
		   d->loaded, d->live, d->sw_events, d->bif_faults, d->ccb_sent,
		   d->ndeferred);
	for (i = 0; i < SGXFIVE_DUMP_NREGS; i++)
		drm_printf(p, "reg %#05x = %#010x\n", d->regs[i].reg, d->regs[i].val);
	if (!d->loaded)
		return;
	sgxfive_print_words(p, "hostctl", d->hostctl, SGXFIVE_HC_DWORDS);
	sgxfive_print_words(p, "ta3dctl", d->ta3dctl, SGXFIVE_TA3D_CTL_DWORDS);
	sgxfive_print_words(p, "ccbctl", d->ccbctl, 2);
}

void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p)
{
	struct sgxfive_devstate d;

	sgxfive_devstate(sgx, &d);
	sgxfive_devstate_print(p, &d);
}

/*
 * SGXPrePowerState(OFF) from the deinit script on: run it, then hold every
 * block in reset, so the EDM is certain to have stopped touching memory
 * before its buffers are freed.
 */
static void sgxfive_halt(struct sgxfive *sgx)
{
	struct sgx_fw_write w[16];
	int n, i;

	/* The firmware stops: what it answered is no longer its running self. */
	sgxfive_ident_clear(sgx);
	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_DEINIT, w, ARRAY_SIZE(w));
	for (i = 0; i < n; i++)
		seq_wr(sgx, w[i].reg, w[i].val);
	seq_wr(sgx, SGX_SOFT_RESET, SGX_SOFT_RESET_ALL);
	WRITE_ONCE(sgx->live, false);
	drm_info(sgx->dev, "SGX stopped, %d events seen\n",
		 atomic_read(&sgx->sw_events));
	/* The BIF is held in reset: nothing deferred is reachable any more. */
	if (sgx->ndeferred)
		mod_delayed_work(system_dfl_wq, &sgx->reaper, 0);
}

/*
 * Stop the EDM if it runs, as SGXPrePowerState(OFF) does: ask it to power off
 * through the kernel CCB, then halt it. The halt runs whether or not the EDM
 * answered, so it is certain to have stopped touching memory.
 */
static void sgxfive_stop(struct sgxfive *sgx)
{
	int ret;

	if (!sgx->live)
		return;
	ret = sgxfive_power(sgx, POWERCMD_POWEROFF, POWMAN_POWEROFF_COMPLETE);
	if (ret)
		drm_warn(sgx->dev, "SGX power-off request failed: %d\n", ret);
	sgxfive_halt(sgx);
}

static int sgxfive_boot(struct sgxfive *sgx)
{
	struct sgx_fw_write part1[16], part2[64];
	struct drm_printer p = drm_info_printer(sgx->dev->dev);
	struct sgx_seq s = {
		.rd = seq_rd, .wr = seq_wr, .udelay = seq_udelay,
		.pre_kick = seq_pre_kick, .ctx = sgx,
		.part1 = part1, .part2 = part2,
	};
	ktime_t start;
	u32 *hc;
	int n, ret;

	if (sgx->live)
		return -EBUSY;
	if (!sgx->loaded) {
		ret = sgxfive_load(sgx);
		if (ret)
			return ret;
	}
	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_PART1, part1,
			  ARRAY_SIZE(part1));
	if (n < 0)
		return n;
	s.npart1 = n;
	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_PART2, part2,
			  ARRAY_SIZE(part2));
	if (n < 0)
		return n;
	s.npart2 = n;
	s.clkgatectl = sgx->fw.meta[SGX_FW_META_CLKGATE];
	s.clkgatectl2 = sgx->fw.meta[SGX_FW_META_CLKGATE + 1];
	/* DIR_LIST_BASE0 gets gma500's PD back, as BRN28011 does in the DDK. */
	s.reset_pd = sgx->reset_pd;
	s.edm_pd = sgx->layout.pd_phys;

	/* HostCtl as SGXPostPowerState leaves it (B1-B5). */
	hc = sgxfive_words(sgx, BUF_HOST_CTL);
	WRITE_ONCE(hc[HC_POWER_STATUS], 0);
	WRITE_ONCE(hc[HC_HWR_SAMPLE_RATE], 10);
	WRITE_ONCE(hc[HC_UKERNEL_TIMER_CLOCK],
		   sgxfive_timer_clock(sgx, SGX_CLOCK_HZ / 1000));
	WRITE_ONCE(hc[HC_APM_SAMPLE_RATE], 0);

	atomic_set(&sgx->sw_events, 0);
	atomic_set(&sgx->bif_faults, 0);
	WRITE_ONCE(sgx->live, true);
	drm_info(sgx->dev, "SGX boot: kicking the EDM\n");
	ret = sgx_seq_boot(&s);
	if (ret) {
		drm_err(sgx->dev, "SGX boot: BIF fault during reset\n");
		sgxfive_dump(sgx, &p);
		WRITE_ONCE(sgx->live, false);
		return ret;
	}
	mb(); /* OSMemoryBarrier after the kick */

	start = ktime_get();
	ret = sgxfive_wait_mem(&hc[HC_INIT_STATUS], EDM_INIT_COMPLETE,
			       EDM_INIT_COMPLETE);
	if (ret) {
		drm_err(sgx->dev, "SGX boot: no INIT_COMPLETE after 500 ms\n");
		sgxfive_dump(sgx, &p);
		return ret;
	}
	drm_info(sgx->dev, "SGX boot: EDM up after %lld us\n",
		 ktime_us_delta(ktime_get(), start));
	/* The first command carries the init info's cache control. */
	sgx->cache_control = sgx->fw.meta[SGX_FW_META_CACHE_CONTROL];
	sgxfive_dump(sgx, &p);
	/* The identity DEV_QUERY reports; an EDM that does not answer is no reason to fail. */
	sgxfive_ident_ask(sgx, NULL);
	return 0;
}

/* With every block in reset: drop a latched BIF fault and stale events. */
static void sgxfive_clear_faults(struct sgxfive *sgx)
{
	u32 st = sgx_rd(sgx, SGX_EVENT_STATUS), st2 = sgx_rd(sgx, SGX_EVENT_STATUS2);

	drm_info(sgx->dev, "SGX reset: BIF_FAULT %#x EVENT_STATUS %#x/%#x\n",
		 sgx_rd(sgx, SGX_BIF_FAULT), st, st2);
	sgx_wr(sgx, SGX_BIF_CTRL, SGX_BIF_CTRL_CLEAR_FAULT);
	sgx_wr(sgx, SGX_BIF_CTRL, 0);
	sgx_wr(sgx, SGX_EVENT_HOST_CLEAR, st);
	sgx_wr(sgx, SGX_EVENT_HOST_CLEAR2, st2);
	udelay(1);
}

/*
 * The order of the job a hang is blamed on, or U64_MAX if none is unfinished:
 * with pd, a page directory a BIF fault went through, the oldest unfinished
 * job on that directory, else the oldest of all (sgxfive_ctx_suspect).
 */
static u64 sgxfive_suspect(struct sgxfive *sgx, u32 pd)
{
	u64 order;

	spin_lock_irq(&sgx->ctx_lock);
	sgxfive_ctx_suspect(&sgx->ctx_list, pd, &order);
	spin_unlock_irq(&sgx->ctx_lock);
	return order;
}

/* The order of the oldest job the firmware has not finished, or U64_MAX. */
u64 sgxfive_oldest(struct sgxfive *sgx)
{
	return sgxfive_suspect(sgx, 0);
}

/*
 * The page directory a BIF fault went through, from BIF_FAULT's directory
 * list and that list's DIR_LIST_BASE; 0 for the EDM's own list 15, which
 * names no client. Read while the faulting pipe is stalled, so the EDM has
 * not handed the list to another context.
 */
static u32 sgxfive_fault_pd(struct sgxfive *sgx, u32 bif_fault)
{
	const unsigned int list = sgx_bif_fault_list(bif_fault);

	if (list == SGX_BIF_EDM_LIST)
		return 0;
	return sgx_rd(sgx, sgx_bif_dir_list_reg(list)) & SGX_BIF_DIR_LIST_ADDR;
}

/*
 * The firmware hung on job suspect, which the detector why chose (the oldest
 * job on a BIF fault's directory, or the oldest of all): fail its context,
 * suspect with the hang's error and the jobs behind it with -ECANCELED, and
 * count the fault against the file that owns the context. False when suspect
 * is no longer its context's oldest unfinished job, because another detector
 * already recovered from it. U64_MAX blames no one.
 * When there is a hang, cd (if any) gets every context's unfinished jobs
 * first.
 */
static bool sgxfive_blame(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why,
			  struct sgxfive_coredump *cd)
{
	struct sgxfive_ctx *guilty;
	bool hang;

	spin_lock_irq(&sgx->ctx_lock);
	guilty = sgxfive_ctx_of_job(&sgx->ctx_list, suspect);
	hang = suspect == U64_MAX || guilty;
	if (hang && cd)
		sgxfive_coredump_ctxs(cd, &sgx->ctx_list, guilty);
	if (hang && guilty) {
		WRITE_ONCE(guilty->lost, true);
		sgxfive_fence_fail_guilty(guilty, sgxfive_hang_error(why));
		if (!guilty->client->banned &&
		    sgxfive_client_fault(guilty->client, jiffies))
			drm_warn(sgx->dev, "sgxfive: %u faults within a minute, file banned\n",
				 SGXFIVE_FAULT_BUDGET);
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return hang;
}

/*
 * Recovery reset: halt the firmware without asking a wedged EDM to power off,
 * rebuild every firmware buffer from the file in place and boot again. The
 * buffers keep their pages and addresses, so the kernel memory of the render
 * node's contexts, mapped in the same directory, stays where it is. Every
 * context starts over: its HW context image is rebuilt and its client CCB
 * rewritten with its unfinished jobs, which run again after the boot, except
 * that the reset which ends a held recovery (debug option) cancels the jobs of
 * a closed context instead, with -ECANCELED: its client has given up on them,
 * and what they read may be gone (sgxfive_fence_cancel_closed). With a
 * suspect job (not U64_MAX), its context is lost instead (see sgxfive_blame),
 * and nothing is done if it has already been recovered from.
 * A reset first captures the error state into *dump (sgxfive_coredump.c),
 * NULL when there is none.
 */
static int sgxfive_reset(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why,
			 struct sgxfive_coredump **dump)
{
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { NULL };
	struct sgxfive_coredump *cd;
	struct sgxfive_ctx *ctx;
	unsigned int i;
	unsigned int cancelled = 0;
	bool ended_hold;
	int ret;

	*dump = NULL;
	if (!sgx->loaded)
		return -ENODEV;
	cd = sgxfive_coredump_alloc(sgx, why, suspect);
	if (!sgxfive_blame(sgx, suspect, why, cd)) {
		sgxfive_coredump_discard(cd);
		return 0;
	}
	if (cd) {
		cd->bif_int_stat = sgx_rd(sgx, SGX_BIF_INT_STAT);
		cd->bif_fault = sgx_rd(sgx, SGX_BIF_FAULT);
		sgxfive_devstate(sgx, &cd->dev);
		*dump = cd;
	}
	if (sgx->live)
		sgxfive_halt(sgx);
	sgxfive_clear_faults(sgx);
	for (i = 0; i < sgx->fw.nbuffers; i++) {
		unsigned int id = sgx->fw.buffers[i].id;

		bufs[id] = sgx->buf[id].va;
	}
	/*
	 * The stock buffers are built before the debug state is reset: if the build fails the own
	 * task stays armed and the function returns without a boot; a build that succeeds writes
	 * every start dword from the records again, so the stock EDM is what boots.
	 */
	ret = sgx_fw_build(&sgx->fw, &sgx->layout, bufs);
	if (ret)
		return ret;
	ended_hold = sgxfive_debug_reset(sgx);
	spin_lock_irq(&sgx->ctx_lock);
	if (ended_hold)
		cancelled = sgxfive_fence_cancel_closed(&sgx->ctx_list);
	list_for_each_entry(ctx, &sgx->ctx_list, link) {
		if (ctx->type == DRM_SGXFIVE_CTX_TYPE_RENDER) {
			ctx->replay = sgxfive_render_reset(ctx);
			continue;
		}
		sgx_xfer_ctx_init(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_HW_OFF),
				  ctx->pd_phys,
				  ctx->mem.addr + SGXFIVE_CTX_CCB_OFF,
				  ctx->mem.addr + SGXFIVE_CTX_CTL_OFF);
		ctx->replay = sgxfive_ctx_rewind(ctx);
	}
	spin_unlock_irq(&sgx->ctx_lock);
	if (cancelled)
		drm_info(sgx->dev,
			 "sgxfive: the hold ends: %u job(s) of closed contexts cancelled\n",
			 cancelled);
	wmb(); /* the write-combined buffers, before the SGX reads them */

	ret = sgxfive_boot(sgx);
	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry(ctx, &sgx->ctx_list, link) {
		if (ret) {
			WRITE_ONCE(ctx->lost, true);	/* nothing runs any more */
			sgxfive_fence_fail_all(ctx, -EIO);
		} else if (ctx->replay && ctx->type == DRM_SGXFIVE_CTX_TYPE_RENDER) {
			sgxfive_kick_render(sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
		} else if (ctx->replay) {
			sgxfive_kick_transfer(sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
		}
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return ret;
}

/*
 * Reset the firmware and recover the render node's contexts (see above); why
 * says which detector found the hang, for the error. With the debug option a
 * recovery may be held: a debugfs reset written while one is held recovers
 * its job, a failed cleanup's reset is dropped (sgxfive_recovery_hold).
 * *dropped, if given, says whether a held recovery dropped the reset; it is
 * decided under the device lock, with the reset, so it is what happened. A
 * failed cleanup is logged here for the same reason, with that outcome.
 */
int __sgxfive_recover(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why,
		      bool *dropped)
{
	struct sgxfive_coredump *cd;
	bool held;
	int ret;

	mutex_lock(&sgx->lock);
	held = sgxfive_recovery_hold(sgx, &suspect, &why);
	if (why == SGXFIVE_HANG_CLEANUP)
		drm_warn(sgx->dev, "sgxfive: render context cleanup failed, %s\n",
			 held ? "no reset while recovery is held" : "resetting");
	if (dropped)
		*dropped = held;
	if (held) {
		mutex_unlock(&sgx->lock);
		return 0;
	}
	ret = sgxfive_reset(sgx, suspect, why, &cd);
	mutex_unlock(&sgx->lock);
	if (cd) {
		cd->result = ret;
		sgxfive_coredump_emit(cd);
	}
	if (ret)
		drm_err(sgx->dev, "SGX recovery failed: %d\n", ret);
	return ret;
}

int sgxfive_recover(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why)
{
	return __sgxfive_recover(sgx, suspect, why, NULL);
}

/*
 * Kernel memory for the render node (contexts and their CCBs): pages mapped
 * write-combined in the EDM's page directory, from the kernel data heap above
 * the firmware's buffers and the debug group's test area.
 */
#define KMEM_START	0x400000
#define KMEM_END	0x05000000		/* REV1 kernel data heap size */

int sgxfive_kmem_alloc(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b, u32 size)
{
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgx->pd ? 0 : -ENODEV;	/* the firmware must be loaded */
	if (!ret)
		ret = drm_mm_insert_node(&sgx->kmem, node, PAGE_ALIGN(size));
	if (!ret) {
		ret = sgxfive_map_pd(sgx, sgx->pd, b,
				     sgx_heap_base[HEAP_KERNEL_DATA] + node->start,
				     size, 0);
		if (ret) {
			sgxfive_unmap(sgx, b);
			drm_mm_remove_node(node);
		}
	}
	if (!ret)
		sgx->kmem_users++;
	mutex_unlock(&sgx->lock);
	return ret;
}

void sgxfive_kmem_free(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b)
{
	mutex_lock(&sgx->lock);
	sgxfive_unmap(sgx, b);
	drm_mm_remove_node(node);
	sgx->kmem_users--;
	mutex_unlock(&sgx->lock);
}

/* Queue the firmware's transfer work for one render-node context. */
int sgxfive_kick_transfer(struct sgxfive *sgx, u32 hw_ctx_addr)
{
	const u32 data[] = { 0, hw_ctx_addr };

	lockdep_assert_held(&sgx->lock);
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return sgxfive_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
}

/* Queue the firmware's TA work for one render context. */
int sgxfive_kick_render(struct sgxfive *sgx, u32 hw_ctx_addr)
{
	const u32 data[] = { 0, hw_ctx_addr };

	lockdep_assert_held(&sgx->lock);
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return sgxfive_command(sgx, SGX_CMD_TA, data, ARRAY_SIZE(data));
}

/* SGXCleanupRequest: one CLEANUP command, acknowledged in HostCtl. */
static int sgxfive_cleanup(struct sgxfive *sgx, u32 type, u32 addr)
{
	u32 *hc = sgxfive_words(sgx, BUF_HOST_CTL);
	const u32 data[] = { type, addr };
	int ret;

	WRITE_ONCE(hc[HC_CLEANUP_STATUS],
		   READ_ONCE(hc[HC_CLEANUP_STATUS]) & ~CLEANUP_COMPLETE);
	ret = sgxfive_command(sgx, SGX_CMD_CLEANUP, data, ARRAY_SIZE(data));
	if (!ret)
		ret = sgxfive_wait_mem(&hc[HC_CLEANUP_STATUS], CLEANUP_COMPLETE,
				       CLEANUP_COMPLETE);
	WRITE_ONCE(hc[HC_CLEANUP_STATUS],
		   READ_ONCE(hc[HC_CLEANUP_STATUS]) & ~CLEANUP_COMPLETE);
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return ret;
}

/*
 * The CLEANUP commands of a render context that goes, in order: its RT data
 * set, the context and the PB descriptor it may still have loaded. A zero
 * pbdesc leaves the PB alone (a context that shares one, debug option).
 * Returns how many.
 */
unsigned int sgxfive_render_cleanup_steps(u32 rtds, u32 ctx, u32 pbdesc,
					  struct sgxfive_cleanup_step *step)
{
	unsigned int n = 0;

	step[n++] = (struct sgxfive_cleanup_step){ CLEANUP_RT, rtds };
	step[n++] = (struct sgxfive_cleanup_step){ CLEANUP_RC, ctx };
	if (pbdesc)	/* none: a PB shared with a context that stays */
		step[n++] = (struct sgxfive_cleanup_step){ CLEANUP_PB, pbdesc };
	return n;
}

/*
 * A render context goes: make the firmware drop what it keeps of it, those
 * steps. Nothing to do with the firmware off.
 */
int sgxfive_render_cleanup(struct sgxfive *sgx, u32 rtds, u32 ctx, u32 pbdesc,
			   u32 flags)
{
	struct sgxfive_cleanup_step step[SGXFIVE_CLEANUP_CMDS];
	const unsigned int n = sgxfive_render_cleanup_steps(rtds, ctx, pbdesc, step);
	ktime_t start = ktime_get();
	unsigned int i;
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (sgx->live) {
		sgxfive_cleanup_trace_start(sgx, rtds, ctx, pbdesc, flags);
		for (i = 0; !ret && i < n; i++) {
			sgxfive_cleanup_trace_note(sgx);
			ret = sgxfive_cleanup(sgx, step[i].type, step[i].addr);
		}
		if (!ret)
			sgxfive_cleanup_trace_note(sgx);
		sgxfive_cleanup_trace_end(sgx, ret);
		drm_dbg(sgx->dev, "sgxfive: render context cleanup %d after %lld us\n",
			ret, ktime_us_delta(ktime_get(), start));
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

/*
 * How buffer id is aliased: the map flags, or -ENOENT when skip (a mask of
 * buffer ids, debugfs alias_skip) leaves it out.
 */
int sgxfive_alias_flags(unsigned int id, bool ta3d_ro, u32 skip)
{
	if (id < 32 && skip & BIT(id))
		return -ENOENT;
	return id == BUF_TA3D_CTL && !ta3d_ro ? 0 : SGX_MMU_RO;
}

/*
 * Map (or unmap) every firmware buffer into a render VM's page directory at
 * its own address: the EDM runs through the context's directory while it
 * loads a parameter buffer. Read-only, except the TA/3D control block, which
 * the EDM writes in that window (TA3DCtl dwords 46 and 47, the loaded PB):
 * read-only, the write faults and the render stalls (debugfs alias_ta3d_ro
 * reproduces it).
 *
 * The buffers alias_skip names are left out when the alias is made, which is
 * when a render context joins a VM that has none; the VM keeps that alias
 * until its last render context goes, and the next one makes it again with
 * the mask of then. Unmapping takes every buffer, so that a change of the
 * mask in between leaves nothing mapped.
 */
int sgxfive_alias_firmware(struct sgxfive *sgx, struct sgx_mmu_pd *pd, bool map)
{
	const bool ta3d_ro = sgxfive_alias_ta3d_ro(sgx);
	const u32 skip = map ? sgxfive_alias_skip(sgx) : 0;
	unsigned int id;
	int ret = 0;

	mutex_lock(&sgx->lock);
	for (id = 0; id < SGX_FW_MAX_BUFFERS && !ret; id++) {
		const struct sgxfive_buf *b = &sgx->buf[id];
		const int flags = sgxfive_alias_flags(id, ta3d_ro, skip);

		if (!b->pd || flags < 0)
			continue;
		if (map)
			ret = sgx_mmu_map(pd, b->addr, b->pages, b->npages, flags);
		else
			sgx_mmu_unmap(pd, b->addr, b->npages);
	}
	if (ret)
		for (id = 0; id < SGX_FW_MAX_BUFFERS; id++)
			if (sgx->buf[id].pd)
				sgx_mmu_unmap(pd, sgx->buf[id].addr, sgx->buf[id].npages);
	sgx->cache_control |= SGX_CC_INVAL_BIF;
	mutex_unlock(&sgx->lock);
	return ret;
}

#define SGXFIVE_REAP_MS	10

/*
 * Free, once the firmware cannot reach it, memory it might have cached
 * translations for: the next kernel-CCB command invalidates the BIF caches,
 * and the item goes when that command has been processed, that is when the
 * EDM's read offset has passed it. With the firmware halted the BIF is in
 * reset, so it goes at once.
 */
void sgxfive_defer(struct sgxfive *sgx, struct sgxfive_deferred *d)
{
	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		mutex_unlock(&sgx->lock);
		d->free(d);
		return;
	}
	sgx->cache_control |= SGX_CC_INVAL_BIF;
	d->after = sgx->ccb_sent + 1;
	list_add_tail(&d->link, &sgx->deferred);
	sgx->ndeferred++;
	mutex_unlock(&sgx->lock);
	queue_delayed_work(system_dfl_wq, &sgx->reaper,
			   msecs_to_jiffies(SGXFIVE_REAP_MS));
}

/* Under lock: move what may be freed to due. */
static void sgxfive_reap_locked(struct sgxfive *sgx, struct list_head *due)
{
	u64 done = U64_MAX;

	if (sgx->live) {
		u32 *ctl = sgxfive_words(sgx, BUF_CCB_CTL);

		rmb(); /* the read offset the EDM wrote */
		done = sgxfive_ccb_done(sgx->ccb_sent, READ_ONCE(ctl[0]),
					READ_ONCE(ctl[1]));
	}
	sgx->ndeferred -= sgxfive_reap(&sgx->deferred, done, due);
}

static void sgxfive_reaper(struct work_struct *work)
{
	struct sgxfive *sgx = container_of(to_delayed_work(work), struct sgxfive,
					   reaper);
	struct sgxfive_deferred *last;
	LIST_HEAD(due);
	bool more;

	mutex_lock(&sgx->lock);
	sgxfive_reap_locked(sgx, &due);
	last = list_last_entry_or_null(&sgx->deferred, struct sgxfive_deferred, link);
	/* No command has carried the invalidation yet: send one that does. */
	if (last && last->after > sgx->ccb_sent)
		sgxfive_command(sgx, SGX_CMD_PROCESS_QUEUES, NULL, 0);
	more = sgx->ndeferred;
	mutex_unlock(&sgx->lock);
	sgxfive_free_all(&due);
	if (more)
		queue_delayed_work(system_dfl_wq, &sgx->reaper,
				   msecs_to_jiffies(SGXFIVE_REAP_MS));
}

#define SGXFIVE_WATCHDOG_MS	50

/*
 * While jobs are in flight. A BIF fault under a job stalls the 3D pipe without
 * raising an interrupt, so poll for one instead of waiting for the job
 * timeout. The job to blame is the oldest one on the page directory the
 * fault went through, the faulting VM's (the oldest of all when the fault
 * names none): a transfer the firmware ran ahead of an older render is
 * blamed, not the render. Not every fault is a hang: the tiling engine and
 * cache requestors fault at page 0 during dummy transfers and the firmware
 * carries on. So recover only when that job is still the same, unfinished, a
 * poll after the fault was seen. The directory is read when a new BIF_FAULT
 * value is first seen. While a recovery is held (debug option) the hang has
 * been reported: the polls go on without a word until the reset.
 */
static void sgxfive_watchdog(struct work_struct *work)
{
	struct sgxfive *sgx = container_of(to_delayed_work(work), struct sgxfive,
					   watchdog);
	u32 stat, fault;
	u64 suspect;

	if (sgxfive_oldest(sgx) == U64_MAX || !READ_ONCE(sgx->live))
		return;			/* the next job arms it again */
	stat = sgx_rd(sgx, SGX_BIF_INT_STAT);
	if (!(stat & SGX_BIF_FAULT_REQ)) {
		sgx->fault_suspect = U64_MAX;
		sgxfive_watchdog_arm(sgx);
		return;
	}
	fault = sgx_rd(sgx, SGX_BIF_FAULT);
	if (sgx->fault_suspect == U64_MAX || fault != sgx->fault_reg) {
		sgx->fault_reg = fault;
		sgx->fault_pd = sgxfive_fault_pd(sgx, fault);
	}
	suspect = sgxfive_suspect(sgx, sgx->fault_pd);
	if (suspect != sgx->fault_suspect) {
		sgx->fault_suspect = suspect;	/* look again next time */
		sgxfive_watchdog_arm(sgx);
		return;
	}
	if (!sgxfive_recovery_held(sgx))
		drm_err(sgx->dev,
			"sgxfive: BIF fault %#x at %#x (PD %#x) stalls job %llu, recovering\n",
			stat, fault, sgx->fault_pd, suspect);
	sgxfive_recover(sgx, suspect, SGXFIVE_HANG_FAULT);
	sgx->fault_suspect = U64_MAX;
	sgxfive_watchdog_arm(sgx);
}

void sgxfive_watchdog_arm(struct sgxfive *sgx)
{
	queue_delayed_work(system_dfl_wq, &sgx->watchdog,
			   msecs_to_jiffies(SGXFIVE_WATCHDOG_MS));
}

/* Load and boot the firmware if it is not running yet. */
int sgxfive_ensure_live(struct sgxfive *sgx)
{
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (!sgx->live)
		ret = sgxfive_boot(sgx);	/* loads it first if needed */
	mutex_unlock(&sgx->lock);
	return ret;
}

/* Page tables changed: the next command makes the BIF refetch them. */
void sgxfive_need_bif_inval(struct sgxfive *sgx)
{
	mutex_lock(&sgx->lock);
	sgx->cache_control |= SGX_CC_INVAL_BIF;
	mutex_unlock(&sgx->lock);
}

/*
 * Submit one kernel CCB command (SGXScheduleCCBCommand): cmd is its index in
 * the firmware file's kick table, data its words. The cache control bits
 * gathered since the last command go with it.
 */
int sgxfive_command(struct sgxfive *sgx, unsigned int cmd, const u32 *data,
		    unsigned int ndata)
{
	struct sgx_seq s = {
		.wr = seq_wr, .wmb = seq_wmb, .ctx = sgx,
	};
	int ret;

	if (!sgx->live)
		return -ENODEV;
	if (cmd >= SGX_FW_META_STRUCT_SIZES - SGX_FW_META_KICK)
		return -EINVAL;
	ret = sgx_ccb_submit(&s, sgxfive_words(sgx, BUF_CCB),
			     sgxfive_words(sgx, BUF_CCB_CTL),
			     sgxfive_words(sgx, BUF_KICKER),
			     sgx->fw.meta[SGX_FW_META_KICK + cmd],
			     sgx->cache_control, data, ndata);
	if (!ret) {
		sgx->cache_control = 0;
		sgx->ccb_sent++;
	}
	return ret;
}

/*
 * The POWER command (SGXPrePowerState): POWEROFF or IDLE. Wait for the EDM to
 * report the transition in HostCtl's power status, then for the clocks to
 * gate (the init info's clock-gating status register and mask). The firmware
 * must be running: before the boot there is no HostCtl to write.
 */
int sgxfive_power(struct sgxfive *sgx, u32 powercmd, u32 done)
{
	const char *what = powercmd == POWERCMD_IDLE ? "idle" : "power-off";
	const u32 data[] = { 0, powercmd };
	u32 reg, mask, *hc;
	ktime_t start;
	int ret;

	if (!sgx->live)
		return -ENODEV;
	hc = sgxfive_words(sgx, BUF_HOST_CTL);
	reg = sgx->fw.meta[SGX_FW_META_CLKGATE + 2];
	mask = sgx->fw.meta[SGX_FW_META_CLKGATE + 3];
	start = ktime_get();
	WRITE_ONCE(hc[HC_POWER_STATUS], READ_ONCE(hc[HC_POWER_STATUS]) & ~done);
	ret = sgxfive_command(sgx, SGX_CMD_POWER, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	ret = sgxfive_wait_mem(&hc[HC_POWER_STATUS], done, done);
	if (ret) {
		drm_warn(sgx->dev, "SGX %s: the EDM did not report it (%d)\n",
			 what, ret);
		return ret;
	}
	while (sgx_rd(sgx, reg) & mask) {
		if (ktime_ms_delta(ktime_get(), start) > 500) {
			drm_warn(sgx->dev,
				 "SGX %s: clocks still on, %#x = %#x (mask %#x)\n",
				 what, reg, sgx_rd(sgx, reg), mask);
			return -ETIMEDOUT;
		}
		usleep_range(50, 100);
	}
	drm_info(sgx->dev, "SGX %s after %lld us\n", what,
		 ktime_us_delta(ktime_get(), start));
	return 0;
}

/*
 * Whether a GETMISCINFO reply names the core the registers name: the core
 * and revision the EDM runs on are the ones read at probe, and the core and
 * revision it was built for are 545 and the same revision.
 */
bool sgxfive_ident_core_ok(const struct sgxfive *sgx, const u32 *mi)
{
	return mi[MISCINFO_CORE_REV] == sgx->core_revision &&
	       mi[MISCINFO_CORE_ID] == sgx->core_id &&
	       mi[MISCINFO_CORE_ID_SW] == SGXFIVE_CORE_ID_SW &&
	       mi[MISCINFO_CORE_REV_SW] == sgx->core_revision;
}

/*
 * The firmware's identity is not known: SW_VALID clear, until an answer
 * (sgxfive_ident_result). Done when the firmware stops (sgxfive_halt), so a
 * reset or a failed boot leaves no answer of the firmware that is gone, and
 * when a request goes unanswered. Readers ignore the values while it is clear.
 */
void sgxfive_ident_clear(struct sgxfive *sgx)
{
	WRITE_ONCE(sgx->sw_valid, false);
}

/*
 * What a GETMISCINFO request came to, under the device lock: ret is its
 * error, 0 when the EDM answered with the reply mi. An answer is what
 * DEV_QUERY reports as the firmware's own identity (GPU_INFO_SW_VALID); a
 * reply for another core than the registers name is kept and warned about. No
 * answer clears SW_VALID, and logs once until the next answer. Returns how
 * many warnings it logged.
 */
unsigned int sgxfive_ident_result(struct sgxfive *sgx, int ret, const u32 *mi)
{
	unsigned int warned = 0;

	if (ret) {
		sgxfive_ident_clear(sgx);
		if (!sgx->ident_warned) {
			drm_warn(sgx->dev,
				 "sgxfive: no GETMISCINFO answer (%d): DEV_QUERY has no firmware identity\n",
				 ret);
			warned++;
		}
		sgx->ident_warned = true;
		return warned;
	}
	sgx->ident_warned = false;
	if (!sgxfive_ident_core_ok(sgx, mi)) {
		drm_warn(sgx->dev,
			 "sgxfive: the firmware runs on core %#x rev %#x and is built for core %u rev %#x, the registers say %#x rev %#x\n",
			 mi[MISCINFO_CORE_ID], mi[MISCINFO_CORE_REV],
			 mi[MISCINFO_CORE_ID_SW], mi[MISCINFO_CORE_REV_SW],
			 sgx->core_id, sgx->core_revision);
		warned++;
	}
	WRITE_ONCE(sgx->core_id_sw, mi[MISCINFO_CORE_ID_SW]);
	WRITE_ONCE(sgx->core_revision_sw, mi[MISCINFO_CORE_REV_SW]);
	WRITE_ONCE(sgx->ddk_version, mi[MISCINFO_DDK_VERSION]);
	/*
	 * Pairs with the smp_load_acquire() of sw_valid in sgxfive_gpu_info()
	 * and sgxfive_ddk_version(): the values are stored before the flag.
	 */
	smp_store_release(&sgx->sw_valid, true);
	return warned;
}

/*
 * Ask the running EDM for its identity (GETMISCINFO, with the sizes of its
 * structures) and take the answer (sgxfive_ident_result), with how long it
 * took in *us if that is given: the boot does, after the firmware is loaded
 * and after each reset, and logs the answer. Under the device lock.
 */
int sgxfive_ident_ask(struct sgxfive *sgx, s64 *us)
{
	u32 *mi = sgxfive_words(sgx, BUF_MISC);
	const u32 data[] = { 0, sgx->layout.addr[BUF_MISC] };
	ktime_t start;
	s64 elapsed;
	int i, ret;

	if (!sgx->live)
		return -ENODEV;
	for (i = 1; i < MISCINFO_DWORDS; i++)
		WRITE_ONCE(mi[i], 0);
	WRITE_ONCE(mi[0], MISCINFO_GET_STRUCT_SIZES);
	start = ktime_get();
	ret = sgxfive_command(sgx, SGX_CMD_GETMISCINFO, data, ARRAY_SIZE(data));
	if (!ret)
		ret = sgxfive_wait_mem(&mi[0], MISCINFO_READY, MISCINFO_READY);
	elapsed = ktime_us_delta(ktime_get(), start);
	if (us)
		*us = elapsed;
	if (!ret)
		dma_rmb(); /* the reply's dwords, after the one that says it is there */
	sgxfive_ident_result(sgx, ret, mi);
	if (!ret)
		drm_info(sgx->dev,
			 "SGX firmware: core %u rev %#x, DDK %u.%u.%u build %u, answered after %lld us\n",
			 mi[MISCINFO_CORE_ID_SW], mi[MISCINFO_CORE_REV_SW],
			 mi[MISCINFO_DDK_VERSION] >> 16, (mi[MISCINFO_DDK_VERSION] >> 8) & 0xff,
			 mi[MISCINFO_DDK_VERSION] & 0xff, mi[MISCINFO_DDK_BUILD], elapsed);
	return ret;
}

/* From the host's SGX interrupt; true when the event belongs to the EDM. */
bool sgxfive_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2)
{
	u32 fault;

	if (!sgx || !READ_ONCE(sgx->live))
		return false;
	sgxfive_debug_irq(sgx, stat_1, stat_2);
	fault = sgx_rd(sgx, SGX_BIF_INT_STAT) & SGX_BIF_FAULT_REQ;
	if (fault) {
		atomic_inc(&sgx->bif_faults);
		drm_err_ratelimited(sgx->dev, "SGX BIF fault: stat %#x addr %#x\n",
				    fault, sgx_rd(sgx, SGX_BIF_FAULT));
	}
	/* SGX_ISRHandler: acknowledge only the EDM's software event. */
	if (stat_1 & SGX_EVENT_SW) {
		atomic_inc(&sgx->sw_events);
		wake_up_all(&sgx->event_wq);
		sgx_wr(sgx, SGX_EVENT_HOST_CLEAR, SGX_EVENT_SW | SGX_EVENT_MASTER);
		sgx_wr(sgx, SGX_EVENT_HOST_CLEAR2, 0);
		(void)sgx_rd(sgx, SGX_EVENT_HOST_CLEAR2);
		sgxfive_fences_irq(sgx);	/* render-node jobs completed */
	}
	return true;
}

static ssize_t sgxfive_boot_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_boot(sgx);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_boot_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_boot_write,
};

static ssize_t sgxfive_stop_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;

	mutex_lock(&sgx->lock);
	sgxfive_stop(sgx);
	mutex_unlock(&sgx->lock);
	return len;
}

static const struct file_operations sgxfive_stop_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_stop_write,
};

static int sgxfive_status_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);

	mutex_lock(&sgx->lock);
	sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_status);

#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
/*
 * Every register of the snapshot table and the firmware control blocks, one
 * value a line, to diff before and after a step (sgxfive_regdump.c). Reads
 * only; the registers whose write starts work are not read.
 */
static int sgxfive_regs_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct sgxfive_devstate *d;
	struct sgxfive_regdump *rd;

	d = kzalloc_obj(*d);
	rd = kzalloc_obj(*rd);
	if (!d || !rd) {
		kfree(d);
		kfree(rd);
		return -ENOMEM;
	}
	mutex_lock(&sgx->lock);
	sgxfive_devstate(sgx, d);
	sgxfive_regdump_capture(rd, seq_rd, sgx);
	mutex_unlock(&sgx->lock);
	sgxfive_regdump_print(&p, rd, d);
	kfree(rd);
	kfree(d);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_regs);

/* The debug group (CONFIG_DRM_GMA500_SGXFIVE_DEBUG). */
static void sgxfive_debugfs_debug(struct sgxfive *sgx, struct dentry *dir)
{
	debugfs_create_file("regs", 0400, dir, sgx, &sgxfive_regs_fops);
	sgxfive_debug_init(sgx, dir);
}
#else
static void sgxfive_debugfs_debug(struct sgxfive *sgx, struct dentry *dir)
{
}
#endif

/*
 * Create the SGX device for a host: the DRM device, the register block and
 * the page directory the host keeps in BIF directory list 0. Nothing touches
 * the hardware until the firmware is loaded and booted.
 */
struct sgxfive *sgxfive_create(const struct sgxfive_host *host)
{
	struct sgxfive *sgx = kzalloc_obj(*sgx);

	if (!sgx)
		return NULL;
	sgx->dev = host->drm;
	sgx->regs = host->regs;
	sgx->reset_pd = host->reset_pd;
	sgx->host_bridge_rev = host->host_bridge_rev;
	/* gma500 powers the SGX island at probe, so the identity reads. */
	sgx->core_id = sgx_rd(sgx, SGX_CORE_ID);
	sgx->core_revision = sgx_rd(sgx, SGX_CORE_REVISION);
	mutex_init(&sgx->lock);
	init_waitqueue_head(&sgx->event_wq);
	INIT_LIST_HEAD(&sgx->deferred);
	INIT_DELAYED_WORK(&sgx->reaper, sgxfive_reaper);
	drm_mm_init(&sgx->kmem, KMEM_START, KMEM_END - KMEM_START);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	INIT_DELAYED_WORK(&sgx->watchdog, sgxfive_watchdog);
	sgx->fault_suspect = U64_MAX;
	sgx->hang_oldest = U64_MAX;
	if (sgxfive_sched_init(sgx)) {
		drm_mm_takedown(&sgx->kmem);
		mutex_destroy(&sgx->lock);
		kfree(sgx);
		return NULL;
	}
	return sgx;
}

void sgxfive_debugfs_init(struct sgxfive *sgx, struct dentry *root)
{
	struct dentry *dir = debugfs_create_dir("sgx", root);
	unsigned int i;

	debugfs_create_file("load", 0200, dir, sgx, &sgxfive_load_fops);
	debugfs_create_file("layout", 0400, dir, sgx, &sgxfive_layout_fops);
	debugfs_create_file("boot", 0200, dir, sgx, &sgxfive_boot_fops);
	debugfs_create_file("stop", 0200, dir, sgx, &sgxfive_stop_fops);
	debugfs_create_file("status", 0400, dir, sgx, &sgxfive_status_fops);
	sgxfive_debugfs_debug(sgx, dir);
	debugfs_create_x32("heap_start", 0600, dir, &sgx->heap_start);
	for (i = 0; i < SGX_FW_MAX_BUFFERS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "buf%02u", i);
		debugfs_create_blob(name, 0400, dir, &sgx->buf[i].blob);
	}
}

void sgxfive_destroy(struct sgxfive *sgx)
{
	if (!sgx)
		return;
	cancel_delayed_work_sync(&sgx->watchdog);
	sgxfive_coredump_fini(sgx);
	mutex_lock(&sgx->lock);
	sgxfive_unload(sgx);
	mutex_unlock(&sgx->lock);
	/* Halted now: whatever is still deferred goes. */
	cancel_delayed_work_sync(&sgx->reaper);
	sgxfive_reaper(&sgx->reaper.work);
	sgxfive_sched_fini(sgx);
	mutex_destroy(&sgx->lock);
	drm_mm_takedown(&sgx->kmem);
	kfree(sgx);
}
