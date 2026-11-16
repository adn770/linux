// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sgxfive VM contexts: one GPU address space (an SGX page directory) per
 * context, managed by the client through VM_MAP and VM_UNMAP. Mappings are
 * exact ranges: a map may not overlap another, and an unmap removes exactly
 * one whole mapping, so a drm_mm of reserved nodes tracks them. Client VMs
 * hold only the client's own buffers: without memory updates the EDM never
 * runs a transfer through them, so they carry no alias of the firmware's
 * buffers.
 * A render context changes that: the EDM loads its parameter buffer through
 * the context's directory, so its VM gets read-only aliases of the firmware
 * while it has render contexts, and each context its kernel-owned slots in
 * the reserved render heaps.
 *
 * The GPU caches page-table entries in the BIF. Whatever it may still reach
 * through a stale entry (a page directory and its tables, the pages of an
 * unmapped range) is freed only after a kernel-CCB command has made the BIF
 * drop them (sgxfive_defer).
 */
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <asm/set_memory.h>

#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>

#include "sgx_mmu.h"
#include "sgx_seq.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

struct sgxfive_vm {
	struct kref ref;
	struct sgxfive *sgx;
	struct mutex lock;		/* the range tracker and the page tables */
	struct sgx_mmu_pd *pd;
	struct drm_mm mm;		/* one reserved node per mapping */
	struct sgxfive_deferred release;	/* of the page directory */
	unsigned long render_slots;	/* render contexts' slots in use */
};

struct sgxfive_mapping {
	struct drm_mm_node node;
	struct drm_gem_shmem_object *shmem;	/* pinned while mapped */
	struct sgxfive_large *large;	/* debugfs vm_pde_*: mapped over this instead */
	struct sgxfive_deferred release;	/* of the pages */
};

u64 sgxfive_ccb_done(u64 sent, u32 woff, u32 roff)
{
	return sent - (woff - roff) % SGX_CCB_ENTRIES;
}

/* Move the items of list whose command is done, in order, to due. */
unsigned int sgxfive_reap(struct list_head *list, u64 done, struct list_head *due)
{
	struct sgxfive_deferred *d, *next;
	unsigned int n = 0;

	list_for_each_entry_safe(d, next, list, link) {
		if (d->after > done)
			break;
		list_move_tail(&d->link, due);
		n++;
	}
	return n;
}

void sgxfive_free_all(struct list_head *due)
{
	struct sgxfive_deferred *d, *next;

	list_for_each_entry_safe(d, next, due, link) {
		list_del(&d->link);
		d->free(d);
	}
}

/* A VM_MAP range: page aligned, non-empty, inside one present user heap. */
int sgxfive_vm_check_range(const struct drm_sgxfive_heap *heaps, unsigned int n,
			   u64 addr, u64 size)
{
	unsigned int i;

	if (!size || (addr | size) & (PAGE_SIZE - 1) || addr + size < addr)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		const struct drm_sgxfive_heap *h = &heaps[i];

		if (!h->size || (h->flags & DRM_SGXFIVE_HEAP_RESERVED))
			continue;
		if (addr >= h->base && addr + size <= h->base + h->size)
			return 0;
	}
	return -EINVAL;
}

/*
 * A VM_MAP range in the first MiB, page 0 excepted: what the PDS's load and
 * store reach (their 18-bit dword address), opened to VM_MAP by debugfs
 * vm_low. No heap lies there.
 */
#define SGXFIVE_VM_LOW_START	0x1000ull
#define SGXFIVE_VM_LOW_END	0x100000ull

int sgxfive_vm_check_low(u64 addr, u64 size)
{
	if (!size || (addr | size) & (PAGE_SIZE - 1) || addr + size < addr)
		return -EINVAL;
	return addr >= SGXFIVE_VM_LOW_START && addr + size <= SGXFIVE_VM_LOW_END ?
	       0 : -EINVAL;
}

/*
 * debugfs vm_pde_*: whether a VM_MAP takes the large-page path. Only one
 * that starts at the knob's address (0: off): it must be exactly the 4 MiB
 * slot and the code and layout must make a region (sgx_mmu_large_geom),
 * else -EINVAL. The pair is checked here, not when written, so the knobs
 * may be written in any order.
 */
int sgxfive_vm_large_wanted(u64 addr, u64 size, u32 knob_addr, unsigned int code,
			    unsigned int layout)
{
	struct sgx_mmu_large_geom g;

	if (!knob_addr || addr != knob_addr)
		return 0;
	if (size != SZ_4M || sgx_mmu_large_geom(code, layout, &g))
		return -EINVAL;
	return 1;
}

#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
/*
 * A large mapping: the client's buffer stays pinned but is not mapped; the
 * slot's PDE (size code and layout from debugfs vm_pde_size and
 * vm_pde_layout) names the table page of a region of contiguous pages the
 * driver owns, every other 4 KiB of it tagged (sgx_mmu_large.c). The region
 * is uncached like the page tables, so the tags are in memory when the GPU
 * reads them and its writes are what the read-out (debugfs vm_large) sees.
 * One at a time; it goes like any mapping, through sgxfive_defer, and its
 * region into a pool of at most 12 MiB for the next.
 */
#define SGXFIVE_LARGE_POOL_MAX	(12 * SZ_1M)

struct sgxfive_large {
	struct sgxfive *sgx;
	struct sgx_mmu_large_geom g;
	struct page *region;		/* 2^order pages, page_private the order */
	u32 phys, pde, addr;
};

void sgxfive_vm_large_init(struct sgxfive *sgx)
{
	mutex_init(&sgx->large.lock);
	INIT_LIST_HEAD(&sgx->large.free);
}

void sgxfive_vm_large_fini(struct sgxfive *sgx)
{
	struct page *p, *next;

	list_for_each_entry_safe(p, next, &sgx->large.free, lru) {
		list_del(&p->lru);
		__free_pages(p, page_private(p));
	}
	sgx->large.free_bytes = 0;
	mutex_destroy(&sgx->large.lock);
}

/* 2^order write-back pages, from the pool or the allocator (NULL: none). */
static struct page *sgxfive_large_get(struct sgxfive *sgx, unsigned int order)
{
	struct page *p;

	mutex_lock(&sgx->large.lock);
	list_for_each_entry(p, &sgx->large.free, lru) {
		if (page_private(p) == order) {
			list_del(&p->lru);
			sgx->large.free_bytes -= PAGE_SIZE << order;
			mutex_unlock(&sgx->large.lock);
			return p;
		}
	}
	mutex_unlock(&sgx->large.lock);
	p = alloc_pages(GFP_KERNEL | GFP_DMA32 | __GFP_NOWARN, order);
	if (p)
		set_page_private(p, order);
	return p;
}

static void sgxfive_large_put(struct sgxfive *sgx, struct page *p)
{
	unsigned int order = page_private(p);

	mutex_lock(&sgx->large.lock);
	if (sgx->large.free_bytes + (PAGE_SIZE << order) <= SGXFIVE_LARGE_POOL_MAX) {
		list_add(&p->lru, &sgx->large.free);
		sgx->large.free_bytes += PAGE_SIZE << order;
		p = NULL;
	}
	mutex_unlock(&sgx->large.lock);
	if (p)
		__free_pages(p, order);
}

/* The region, its table and tags, uncached: ERR_PTR on a refusal, never partial. */
static struct sgxfive_large *sgxfive_large_create(struct sgxfive *sgx, u32 addr,
						  unsigned int code, unsigned int layout)
{
	struct sgxfive_large *l = kzalloc_obj(*l);
	unsigned int n;
	u32 *va, off;
	int ret = -ENOMEM;

	if (!l)
		return ERR_PTR(-ENOMEM);
	if (sgx_mmu_large_geom(code, layout, &l->g)) {
		ret = -EINVAL;
		goto err;
	}
	n = 1 << l->g.order;
	l->region = sgxfive_large_get(sgx, l->g.order);
	if (!l->region)
		goto err;
	if ((u64)page_to_phys(l->region) + l->g.region > 1ull << 32) {
		ret = -EFAULT;		/* the MMU has 32-bit addresses */
		goto err_put;
	}
	l->sgx = sgx;
	l->addr = addr;
	l->phys = page_to_phys(l->region);
	l->pde = sgx_mmu_large_pde(&l->g, l->phys);
	va = page_address(l->region);
	sgx_mmu_large_table(&l->g, l->phys, va);
	for (off = SZ_4K; off < l->g.region; off += SZ_4K)
		sgx_mmu_large_fill(&l->g, off, va + off / 4);
	if (set_pages_uc(l->region, n))		/* flushes what the CPU wrote */
		goto err_put;
	return l;
err_put:
	sgxfive_large_put(sgx, l->region);
err:
	kfree(l);
	return ERR_PTR(ret);
}

static void sgxfive_large_free(struct sgxfive_large *l)
{
	set_pages_wb(l->region, 1 << l->g.order);
	sgxfive_large_put(l->sgx, l->region);
	kfree(l);
}

/* Under the VM's lock: the slot's PDE names the region's table. */
static int sgxfive_large_attach(struct sgxfive_vm *vm, struct sgxfive_large *l)
{
	struct sgxfive *sgx = vm->sgx;
	int ret;

	mutex_lock(&sgx->large.lock);
	ret = sgx->large.live ? -EBUSY : sgx_mmu_pd_override(vm->pd, l->addr, l->pde);
	if (!ret)
		sgx->large.live = l;
	mutex_unlock(&sgx->large.lock);
	return ret;
}

/* Under the VM's lock: the slot back as it was; the region goes with the mapping. */
static void sgxfive_large_detach(struct sgxfive_vm *vm, struct sgxfive_large *l)
{
	struct sgxfive *sgx = vm->sgx;

	mutex_lock(&sgx->large.lock);
	sgx_mmu_pd_override(vm->pd, l->addr, 0);
	if (sgx->large.live == l)
		sgx->large.live = NULL;
	mutex_unlock(&sgx->large.lock);
}

static const char *const sgxfive_large_layouts[SGX_LARGE_LAYOUTS] = {
	"dense", "pt4k", "misalign", "ptoff",
};

static void sgxfive_large_show_run(struct seq_file *m, const struct sgxfive_large *l,
				   u32 from, u32 to)
{
	if (from == to)
		return;
	seq_printf(m, "slab %u page %u .. slab %u page %u: intact\n",
		   from / l->g.unit, from % l->g.unit / SZ_4K,
		   (to - SZ_4K) / l->g.unit, (to - SZ_4K) % l->g.unit / SZ_4K);
}

/*
 * debugfs vm_large: the live large mapping's PDE, the table entries that are
 * not the trap (runs of consecutive pages folded), whether the table page is
 * still what was written, and per 4 KiB of the region whether its tags are
 * intact: runs of intact pages, and for a changed one the count of changed
 * dwords and the first eight (offset, value).
 */
int sgxfive_vm_large_show(struct seq_file *m, struct sgxfive *sgx)
{
	struct sgx_mmu_large_diff d;
	const struct sgxfive_large *l;
	unsigned int i, j, changed = 0, bad = 0, k;
	u32 *want, *pt, trap, off, run;

	want = kmalloc_array(SGX_LARGE_PT_ENTRIES, sizeof(u32), GFP_KERNEL);
	if (!want)
		return -ENOMEM;
	mutex_lock(&sgx->large.lock);
	l = sgx->large.live;
	if (!l) {
		seq_puts(m, "none\n");
		goto out;
	}
	pt = page_address(l->region);
	trap = sgx_mmu_pte(l->phys + l->g.trap_off, 0);
	seq_printf(m, "addr 0x%08x code %u layout %u (%s) size 0x%x unit 0x%x\n", l->addr,
		   l->g.code, l->g.layout, sgxfive_large_layouts[l->g.layout], l->g.size,
		   l->g.unit);
	seq_printf(m, "region 0x%08x +0x%x order %u\n", l->phys, l->g.region, l->g.order);
	seq_printf(m, "pde 0x%08x\n", l->pde);
	for (k = 0; k < l->g.nblocks; k++)
		seq_printf(m, "block %u 0x%08x\n", k, l->phys + l->g.block_off[k]);
	seq_printf(m, "trap 0x%08x\n", trap);
	for (i = 0; i < SGX_LARGE_PT_ENTRIES; i = j) {
		u32 e = READ_ONCE(pt[i]);

		for (j = i + 1; j < SGX_LARGE_PT_ENTRIES &&
		     READ_ONCE(pt[j]) == e + (j - i) * SZ_4K && e != trap; j++)
			;
		if (e == trap)
			continue;
		if (j - i == 1)
			seq_printf(m, "pt %u 0x%08x\n", i, e);
		else
			seq_printf(m, "pt %u-%u 0x%08x step 0x1000\n", i, j - 1, e);
	}
	sgx_mmu_large_table(&l->g, l->phys, want);
	for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++)
		if (READ_ONCE(pt[i]) != want[i])
			bad++;
	seq_printf(m, "table %s (%u entries changed)\n", bad ? "CHANGED" : "intact", bad);
	run = SZ_4K;
	for (off = SZ_4K; off < l->g.region; off += SZ_4K) {
		if (!sgx_mmu_large_scan(&l->g, off, pt + off / 4, &d))
			continue;
		sgxfive_large_show_run(m, l, run, off);
		run = off + SZ_4K;
		changed++;
		seq_printf(m, "slab %u page %u: %u changed", off / l->g.unit,
			   off % l->g.unit / SZ_4K, d.changed);
		for (k = 0; k < min_t(unsigned int, d.changed, SGX_LARGE_DIFFS); k++)
			seq_printf(m, " +0x%03x 0x%08x", d.first[k].off, d.first[k].val);
		seq_putc(m, '\n');
	}
	sgxfive_large_show_run(m, l, run, off);
	seq_printf(m, "pages %u intact %u changed\n",
		   l->g.region / SZ_4K - 1 - changed, changed);
out:
	mutex_unlock(&sgx->large.lock);
	kfree(want);
	return 0;
}
#else
struct sgxfive_large {
	struct page *unused;
};

static struct sgxfive_large *sgxfive_large_create(struct sgxfive *sgx, u32 addr,
						  unsigned int code, unsigned int layout)
{
	return ERR_PTR(-EINVAL);
}

static void sgxfive_large_free(struct sgxfive_large *l)
{
}

static int sgxfive_large_attach(struct sgxfive_vm *vm, struct sgxfive_large *l)
{
	return -EINVAL;
}

static void sgxfive_large_detach(struct sgxfive_vm *vm, struct sgxfive_large *l)
{
}
#endif

static void sgxfive_mapping_release(struct sgxfive_deferred *d)
{
	struct sgxfive_mapping *m = container_of(d, struct sgxfive_mapping, release);

	if (m->large)
		sgxfive_large_free(m->large);
	drm_gem_shmem_unpin(m->shmem);
	drm_gem_object_put(&m->shmem->base);
	kfree(m);
}

/*
 * The range is free for new mappings at once; the pages stay pinned until
 * the GPU's stale TLB entries for it are gone.
 */
static void sgxfive_mapping_free(struct sgxfive_vm *vm, struct sgxfive_mapping *m)
{
	if (m->large)
		sgxfive_large_detach(vm, m->large);
	else
		sgx_mmu_unmap(vm->pd, m->node.start, m->node.size >> PAGE_SHIFT);
	drm_mm_remove_node(&m->node);
	m->release.free = sgxfive_mapping_release;
	sgxfive_defer(vm->sgx, &m->release);
}

static void sgxfive_vm_pd_release(struct sgxfive_deferred *d)
{
	struct sgxfive_vm *vm = container_of(d, struct sgxfive_vm, release);

	sgx_mmu_pd_destroy(vm->pd);
	kfree(vm);
}

static void sgxfive_vm_release(struct kref *ref)
{
	struct sgxfive_vm *vm = container_of(ref, struct sgxfive_vm, ref);
	struct drm_mm_node *node, *next;

	drm_mm_for_each_node_safe(node, next, &vm->mm)
		sgxfive_mapping_free(vm, container_of(node, struct sgxfive_mapping,
						      node));
	drm_mm_takedown(&vm->mm);
	mutex_destroy(&vm->lock);
	/*
	 * The EDM keeps directory-list assignments by PD address and its BIF
	 * caches what it read through them, so a later VM whose PD landed on
	 * this page would run on stale entries (the tiling engine and cache
	 * requestors fault at page 0 and the job stalls). The directory and its
	 * tables go only after a command has made the BIF drop them.
	 */
	vm->release.free = sgxfive_vm_pd_release;
	sgxfive_defer(vm->sgx, &vm->release);
}

struct sgxfive_vm *sgxfive_vm_get(struct sgxfive_file *f, u32 handle)
{
	struct sgxfive_vm *vm;

	xa_lock(&f->vms);
	vm = xa_load(&f->vms, handle);
	if (vm)
		kref_get(&vm->ref);
	xa_unlock(&f->vms);
	return vm;
}

void sgxfive_vm_put(struct sgxfive_vm *vm)
{
	kref_put(&vm->ref, sgxfive_vm_release);
}

/* The PTE at addr in the VM's page tables, 0 for none. */
u32 sgxfive_vm_lookup(struct sgxfive_vm *vm, u32 addr)
{
	u32 pte;

	mutex_lock(&vm->lock);
	pte = sgx_mmu_lookup(vm->pd, addr);
	mutex_unlock(&vm->lock);
	return pte;
}

u32 sgxfive_vm_pd_phys(const struct sgxfive_vm *vm)
{
	return sgx_mmu_pd_phys(vm->pd);
}

/*
 * A render context joins the VM: the lowest free slot of the render heaps,
 * and the firmware's read-only alias if the VM has none yet.
 */
int sgxfive_vm_render_attach(struct sgxfive_vm *vm, unsigned int *slot)
{
	unsigned int s;
	int ret = 0;

	mutex_lock(&vm->lock);
	s = find_first_zero_bit(&vm->render_slots, SGXFIVE_RENDER_SLOTS);
	if (s >= SGXFIVE_RENDER_SLOTS)
		ret = -ENOSPC;
	else if (!vm->render_slots)
		ret = sgxfive_alias_firmware(vm->sgx, vm->pd, true);
	if (!ret) {
		__set_bit(s, &vm->render_slots);
		*slot = s;
	}
	mutex_unlock(&vm->lock);
	if (!ret)
		sgxfive_need_bif_inval(vm->sgx);
	return ret;
}

/*
 * Its slot is free again once its pages are unmapped; the last render
 * context takes the alias with it (the firmware pages are not freed, so no
 * stale translation can reach reused memory).
 */
void sgxfive_vm_render_detach(struct sgxfive_vm *vm, unsigned int slot)
{
	mutex_lock(&vm->lock);
	__clear_bit(slot, &vm->render_slots);
	if (!vm->render_slots)
		sgxfive_alias_firmware(vm->sgx, vm->pd, false);
	mutex_unlock(&vm->lock);
	sgxfive_need_bif_inval(vm->sgx);
}

/* Kernel pages into the VM, outside the range tracker (reserved heaps). */
int sgxfive_vm_map_pages(struct sgxfive_vm *vm, u32 addr, struct page **pages,
			 unsigned int n, u32 flags)
{
	int ret;

	mutex_lock(&vm->lock);
	ret = sgx_mmu_map(vm->pd, addr, pages, n, flags);
	mutex_unlock(&vm->lock);
	sgxfive_need_bif_inval(vm->sgx);
	return ret;
}

/* The caller frees the pages through sgxfive_defer. */
void sgxfive_vm_unmap_pages(struct sgxfive_vm *vm, u32 addr, unsigned int n)
{
	mutex_lock(&vm->lock);
	sgx_mmu_unmap(vm->pd, addr, n);
	mutex_unlock(&vm->lock);
	sgxfive_need_bif_inval(vm->sgx);
}

int sgxfive_ioctl_create_vm(struct sgxfive *sgx, void *data,
			    struct drm_file *file)
{
	struct drm_sgxfive_create_vm_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_vm *vm;
	int ret;

	if (a->_padding_4)
		return -EINVAL;
	vm = kzalloc_obj(*vm);
	if (!vm)
		return -ENOMEM;
	vm->pd = sgx_mmu_pd_create_user();
	if (!vm->pd) {
		kfree(vm);
		return -ENOMEM;
	}
	kref_init(&vm->ref);
	vm->sgx = sgx;
	mutex_init(&vm->lock);
	drm_mm_init(&vm->mm, 0, 1ull << 32);
	ret = xa_alloc(&f->vms, &a->handle, vm,
		       XA_LIMIT(1, DRM_SGXFIVE_FILE_MAX_VM_CONTEXTS), GFP_KERNEL);
	if (ret)
		sgxfive_vm_put(vm);
	return ret == -EBUSY ? -ENOSPC : ret;
}

int sgxfive_ioctl_destroy_vm(struct sgxfive *sgx, void *data,
			     struct drm_file *file)
{
	struct drm_sgxfive_destroy_vm_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_vm *vm;

	if (a->_padding_4)
		return -EINVAL;
	vm = xa_erase(&f->vms, a->handle);
	if (!vm)
		return -EINVAL;
	sgxfive_vm_put(vm);	/* contexts using it hold their own reference */
	return 0;
}

/*
 * The PTE flags of a VM_MAP at addr: READONLY as the user asks, and the flags
 * of debugfs vm_pte_flags (only WRITEONLY, CACHECONSISTENT and EDMPROTECT) when
 * the mapping starts at its vm_pte_addr (0: off). RO and WO may come together.
 */
u32 sgxfive_vm_pte_flags(bool ro, u32 addr, u32 knob_addr, u32 knob_flags)
{
	u32 flags = ro ? SGX_MMU_RO : 0;

	if (knob_addr && addr == knob_addr)
		flags |= knob_flags & SGXFIVE_VM_PTE_KNOB;
	return flags;
}

int sgxfive_ioctl_vm_map(struct sgxfive *sgx, void *data, struct drm_file *file)
{
	struct drm_sgxfive_vm_map_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct drm_gem_shmem_object *shmem;
	struct sgxfive_mapping *m = NULL;
	struct drm_gem_object *obj;
	struct sgxfive_large *large = NULL;
	struct sgxfive_vm *vm;
	u32 knob_addr, knob_flags, flags, pde_addr;
	unsigned int code, layout;
	int ret;

	if (a->_padding_14 || (a->flags & ~DRM_SGXFIVE_VM_MAP_FLAGS_MASK) ||
	    (a->offset & (PAGE_SIZE - 1)))
		return -EINVAL;
	ret = sgxfive_vm_check_range(sgxfive_rev1_heaps, DRM_SGXFIVE_HEAP_COUNT,
				     a->device_addr, a->size);
	if (ret && sgxfive_vm_low(sgx))
		ret = sgxfive_vm_check_low(a->device_addr, a->size);
	if (ret)
		return ret;
	pde_addr = sgxfive_vm_pde_knob(sgx, &code, &layout);
	ret = sgxfive_vm_large_wanted(a->device_addr, a->size, pde_addr, code, layout);
	if (ret < 0)
		return ret;
	if (ret) {
		large = sgxfive_large_create(sgx, a->device_addr, code, layout);
		if (IS_ERR(large))
			return PTR_ERR(large);
	}
	obj = drm_gem_object_lookup(file, a->handle);
	if (!obj) {
		ret = -ENOENT;
		goto err_large;
	}
	if (!sgxfive_is_bo(obj) || a->offset > obj->size ||
	    a->size > obj->size - a->offset) {
		drm_gem_object_put(obj);
		ret = -EINVAL;
		goto err_large;
	}
	vm = sgxfive_vm_get(f, a->vm_context_handle);
	if (!vm) {
		drm_gem_object_put(obj);
		ret = -EINVAL;
		goto err_large;
	}
	shmem = to_drm_gem_shmem_obj(obj);
	m = kzalloc_obj(*m);
	ret = m ? drm_gem_shmem_pin(shmem) : -ENOMEM;
	if (ret)
		goto err;
	m->shmem = shmem;
	m->node.start = a->device_addr;
	m->node.size = a->size;
	knob_flags = sgxfive_vm_pte_knob(sgx, &knob_addr);
	flags = sgxfive_vm_pte_flags(a->flags & DRM_SGXFIVE_VM_MAP_READ_ONLY, a->device_addr,
				     knob_addr, knob_flags);

	mutex_lock(&vm->lock);
	ret = drm_mm_reserve_node(&vm->mm, &m->node);
	if (ret == -ENOSPC)
		ret = -EEXIST;		/* overlaps a mapping */
	if (!ret) {
		if (large)
			ret = sgxfive_large_attach(vm, large);
		else
			ret = sgx_mmu_map(vm->pd, a->device_addr,
					  shmem->pages + (a->offset >> PAGE_SHIFT),
					  a->size >> PAGE_SHIFT, flags);
		if (ret)
			drm_mm_remove_node(&m->node);
		else
			m->large = large;
	}
	mutex_unlock(&vm->lock);
	if (ret) {
		drm_gem_shmem_unpin(shmem);
		goto err;
	}
	if (large)
		drm_info(sgx->dev, "sgxfive: vm_map 0x%08llx +0x%llx: PDE size code %u layout %u (debugfs vm_pde_size)\n",
			 a->device_addr, a->size, code, layout);
	else if (flags & SGXFIVE_VM_PTE_KNOB)
		drm_info(sgx->dev, "sgxfive: vm_map 0x%08llx +0x%llx: PTE flags 0x%x (debugfs vm_pte_flags)\n",
			 a->device_addr, a->size, flags);
	/* The BIF must refetch the page tables (next command's cache control). */
	sgxfive_need_bif_inval(sgx);
	sgxfive_vm_put(vm);
	return 0;		/* the mapping keeps the object reference */
err:
	kfree(m);
	sgxfive_vm_put(vm);
	drm_gem_object_put(obj);
err_large:
	if (!IS_ERR_OR_NULL(large))
		sgxfive_large_free(large);	/* never mapped: no stale translation */
	return ret;
}

int sgxfive_ioctl_vm_unmap(struct sgxfive *sgx, void *data,
			   struct drm_file *file)
{
	struct drm_sgxfive_vm_unmap_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct drm_mm_node *node;
	struct sgxfive_vm *vm;
	int ret = -ENOENT;

	if (a->_padding_4)
		return -EINVAL;
	vm = sgxfive_vm_get(f, a->vm_context_handle);
	if (!vm)
		return -EINVAL;
	mutex_lock(&vm->lock);
	drm_mm_for_each_node(node, &vm->mm) {
		if (node->start == a->device_addr && node->size == a->size) {
			struct sgxfive_mapping *m;

			m = container_of(node, struct sgxfive_mapping, node);
			sgxfive_mapping_free(vm, m);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&vm->lock);
	if (!ret)
		sgxfive_need_bif_inval(sgx);
	sgxfive_vm_put(vm);
	return ret;
}

int sgxfive_open(struct sgxfive *sgx, struct drm_file *file)
{
	struct sgxfive_file *f = kzalloc_obj(*f);

	if (!f)
		return -ENOMEM;
	f->client = sgxfive_client_create();
	if (!f->client) {
		kfree(f);
		return -ENOMEM;
	}
	f->client->pid = task_tgid_nr(current);
	get_task_comm(f->client->comm, current);
	xa_init_flags(&f->vms, XA_FLAGS_ALLOC1);
	xa_init_flags(&f->ctxs, XA_FLAGS_ALLOC1);
	file->driver_priv = f;
	return 0;
}

void sgxfive_postclose(struct sgxfive *sgx, struct drm_file *file)
{
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_vm *vm;
	unsigned long i;

	if (!f)
		return;
	sgxfive_ctx_close_all(f);	/* contexts hold their VMs */
	xa_for_each(&f->vms, i, vm)
		sgxfive_vm_put(vm);
	xa_destroy(&f->vms);
	sgxfive_client_put(f->client);
	kfree(f);
	file->driver_priv = NULL;
}
