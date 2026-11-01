// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * SGX545 microkernel bring-up for Cedarview (GMA3600/3650).
 *
 * Loads sgx545_ukernel.fw (see sgx_fw.c), places its buffers in the SGX
 * address space with the DDK's heap layout and maps them through a private
 * page directory, the one the EDM will use. Nothing runs at probe: writing to
 * debugfs dri/N/sgx/load does the work, and the buffers can then be read back
 * from dri/N/sgx/bufNN to check them against the reference loader.
 */
#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>

#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include "sgxfive.h"
#include "sgxfive_priv.h"
#include "sgx_fw.h"
#include "sgx_mmu.h"

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

static void sgxfive_unload(struct sgxfive *sgx)
{
	unsigned int id;

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

/*
 * Create the SGX device for a host: the DRM device and the register block.
 * Nothing touches the hardware until the firmware is loaded.
 */
struct sgxfive *sgxfive_create(const struct sgxfive_host *host)
{
	struct sgxfive *sgx = kzalloc_obj(*sgx);

	if (!sgx)
		return NULL;
	sgx->dev = host->drm;
	sgx->regs = host->regs;
	mutex_init(&sgx->lock);
	return sgx;
}

void sgxfive_debugfs_init(struct sgxfive *sgx, struct dentry *root)
{
	struct dentry *dir = debugfs_create_dir("sgx", root);
	unsigned int i;

	debugfs_create_file("load", 0200, dir, sgx, &sgxfive_load_fops);
	debugfs_create_file("layout", 0400, dir, sgx, &sgxfive_layout_fops);
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
