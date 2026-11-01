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
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>

#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include "sgxfive.h"
#include "sgxfive_priv.h"
#include "sgxfive_regdump.h"
#include "sgx_fw.h"
#include "sgx_mmu.h"
#include "sgx_seq.h"

#define SGX_FW_NAME	"sgx545_ukernel.fw"

/*
 * The GPU address layout, by sgxapi_km.h heap id: the DDK's SGX545 REV1 table
 * (sgxconfig.h), which Intel's Cedarview kernel uses when the host bridge is
 * below revision 4 (FIX_HW_BRN_SAMPLE_CACHE): every heap below 0x3e000000 and
 * textures in a heap of their own (resgx545's doc/specs/boot_sequence.md (d)).
 */
enum {
	HEAP_KERNEL_CODE = 2,
	HEAP_KERNEL_DATA = 3,
	HEAP_PIXELSHADER = 4,
	HEAP_VERTEXSHADER = 5,
	HEAP_PDSPIXEL = 6,
	HEAP_TEXTURE = 14,	/* SGX_TEXTURE_HEAP_ID */
};

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

/*
 * Buffer ids and HostCtl dwords, as resgx545's doc/specs/fw_format.md and
 * doc/specs/hw_spec.md give them.
 */
enum {
	BUF_CCB_CTL = 7, BUF_KICKER = 8, BUF_TA3D_CTL = 9, BUF_HOST_CTL = 10,
};

enum {
	HC_INIT_STATUS = 0, HC_POWER_STATUS = 1, HC_HWR_SAMPLE_RATE = 5,
	HC_UKERNEL_TIMER_CLOCK = 6, HC_APM_SAMPLE_RATE = 7, HC_HOST_CLOCK = 13,
	HC_DWORDS = 17,
};

#define TA3D_CTL_DWORDS		(0xec / 4)
static_assert(HC_DWORDS == SGXFIVE_HC_DWORDS);
static_assert(TA3D_CTL_DWORDS == SGXFIVE_TA3D_CTL_DWORDS);
#define EDM_INIT_COMPLETE	0x1
#define SGX_CLOCK_HZ		400000000	/* SYS_SGX_CLOCK_SPEED */
#define SGX_EVENT_SW		0x00004000
#define SGX_EVENT_MASTER	0x80000000

static void sgxfive_unmap(struct sgxfive *sgx, struct sgxfive_buf *b)
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

/* Allocate a zeroed buffer write-combined, map it for the CPU and in pd. */
static int sgxfive_map_pd(struct sgxfive *sgx, struct sgx_mmu_pd *pd,
			  struct sgxfive_buf *b, u32 addr, u32 size)
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
	ret = sgx_mmu_map(pd, addr, b->pages, b->npages, 0);
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
	return sgxfive_map_pd(sgx, sgx->pd, b, addr, size);
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
	ret = sgx_fw_build(&sgx->fw, l, bufs);
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
	if (sgx->loaded)
		sgxfive_unload(sgx);
	ret = sgxfive_load(sgx);
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

static u32 *sgxfive_words(struct sgxfive *sgx, unsigned int id)
{
	return sgx->buf[id].va;
}

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
static int sgxfive_wait_mem(const u32 *w, u32 mask, u32 want)
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
static void sgxfive_devstate(struct sgxfive *sgx, struct sgxfive_devstate *d)
{
	unsigned int i;

	d->loaded = sgx->loaded;
	d->live = sgx->live;
	d->sw_events = atomic_read(&sgx->sw_events);
	d->bif_faults = atomic_read(&sgx->bif_faults);
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

static void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p)
{
	struct sgxfive_devstate d;
	unsigned int i;

	sgxfive_devstate(sgx, &d);
	drm_printf(p, "loaded %d live %d sw_events %d bif_faults %d\n",
		   d.loaded, d.live, d.sw_events, d.bif_faults);
	for (i = 0; i < SGXFIVE_DUMP_NREGS; i++)
		drm_printf(p, "reg %#05x = %#010x\n", d.regs[i].reg, d.regs[i].val);
	if (!d.loaded)
		return;
	sgxfive_print_words(p, "hostctl", d.hostctl, SGXFIVE_HC_DWORDS);
	sgxfive_print_words(p, "ta3dctl", d.ta3dctl, SGXFIVE_TA3D_CTL_DWORDS);
	sgxfive_print_words(p, "ccbctl", d.ccbctl, 2);
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

	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_DEINIT, w, ARRAY_SIZE(w));
	for (i = 0; i < n; i++)
		seq_wr(sgx, w[i].reg, w[i].val);
	seq_wr(sgx, SGX_SOFT_RESET, SGX_SOFT_RESET_ALL);
	WRITE_ONCE(sgx->live, false);
	drm_info(sgx->dev, "SGX stopped, %d events seen\n",
		 atomic_read(&sgx->sw_events));
}

/*
 * Stop the EDM if it runs. The DDK first asks it to power off through the
 * kernel CCB; this driver gives it no work, so the deinit script and the
 * reset are enough.
 */
static void sgxfive_stop(struct sgxfive *sgx)
{
	if (!sgx->live)
		return;
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
	WRITE_ONCE(hc[HC_UKERNEL_TIMER_CLOCK], SGX_CLOCK_HZ / 1000);
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
	sgxfive_dump(sgx, &p);
	return 0;
}

/* From the host's SGX interrupt; true when the event belongs to the EDM. */
bool sgxfive_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2)
{
	u32 fault;

	if (!sgx || !READ_ONCE(sgx->live))
		return false;
	fault = sgx_rd(sgx, SGX_BIF_INT_STAT) & SGX_BIF_FAULT_REQ;
	if (fault) {
		atomic_inc(&sgx->bif_faults);
		drm_err_ratelimited(sgx->dev, "SGX BIF fault: stat %#x addr %#x\n",
				    fault, sgx_rd(sgx, SGX_BIF_FAULT));
	}
	/* SGX_ISRHandler: acknowledge only the EDM's software event. */
	if (stat_1 & SGX_EVENT_SW) {
		atomic_inc(&sgx->sw_events);
		sgx_wr(sgx, SGX_EVENT_HOST_CLEAR, SGX_EVENT_SW | SGX_EVENT_MASTER);
		sgx_wr(sgx, SGX_EVENT_HOST_CLEAR2, 0);
		(void)sgx_rd(sgx, SGX_EVENT_HOST_CLEAR2);
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
	mutex_init(&sgx->lock);
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
	mutex_lock(&sgx->lock);
	sgxfive_unload(sgx);
	mutex_unlock(&sgx->lock);
	mutex_destroy(&sgx->lock);
	kfree(sgx);
}
