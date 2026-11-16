// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * SGX MMU page tables for the SGX545 driver. The directory and table pages
 * are mapped uncached, so every entry the CPU writes is in memory when the
 * BIF next walks the tables: no cache flushes before a kick. The BIF's own
 * PD and TLB caches are invalidated through the next CCB command's cache
 * control (or the reset sequence at boot).
 */
#include <linux/bitmap.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/set_memory.h>

#include "sgx_mmu.h"

#define SGX_MMU_ENTRIES	1024

struct sgx_mmu_pd {
	struct page *dir;
	u32 *pde;			/* the directory, uncached */
	gfp_t gfp;			/* for the directory and its tables */
	struct page *table[SGX_MMU_ENTRIES];
	DECLARE_BITMAP(over, SGX_MMU_ENTRIES);	/* slots pointed at a caller's table */
};

static struct page *sgx_mmu_alloc_uc(gfp_t gfp)
{
	struct page *p = alloc_page(gfp | GFP_DMA32 | __GFP_ZERO);

	if (p && set_pages_uc(p, 1)) {
		__free_page(p);
		return NULL;
	}
	return p;
}

static void sgx_mmu_free_uc(struct page *p)
{
	set_pages_wb(p, 1);
	__free_page(p);
}

static struct sgx_mmu_pd *sgx_mmu_pd_create_gfp(gfp_t gfp)
{
	struct sgx_mmu_pd *pd = kzalloc_obj(*pd, gfp);

	if (!pd)
		return NULL;
	pd->gfp = gfp;
	pd->dir = sgx_mmu_alloc_uc(gfp);
	if (!pd->dir) {
		kfree(pd);
		return NULL;
	}
	pd->pde = page_address(pd->dir);
	return pd;
}

struct sgx_mmu_pd *sgx_mmu_pd_create(void)
{
	return sgx_mmu_pd_create_gfp(GFP_KERNEL);
}

/* A client's directory: it and its tables are charged to the caller's memcg. */
struct sgx_mmu_pd *sgx_mmu_pd_create_user(void)
{
	return sgx_mmu_pd_create_gfp(GFP_KERNEL_ACCOUNT);
}

void sgx_mmu_pd_destroy(struct sgx_mmu_pd *pd)
{
	unsigned int i;

	if (!pd)
		return;
	for (i = 0; i < SGX_MMU_ENTRIES; i++)
		if (pd->table[i])
			sgx_mmu_free_uc(pd->table[i]);
	sgx_mmu_free_uc(pd->dir);
	kfree(pd);
}

u32 sgx_mmu_pd_phys(const struct sgx_mmu_pd *pd)
{
	return page_to_phys(pd->dir);
}

u32 sgx_mmu_pd_entry(const struct sgx_mmu_pd *pd, unsigned int i)
{
	return READ_ONCE(pd->pde[i]);
}

static u32 *sgx_mmu_ptes(const struct sgx_mmu_pd *pd, u32 addr)
{
	struct page *t = pd->table[sgx_mmu_pd_index(addr)];

	return t ? page_address(t) : NULL;
}

u32 sgx_mmu_lookup(const struct sgx_mmu_pd *pd, u32 addr)
{
	u32 *pte = sgx_mmu_ptes(pd, addr);

	return pte ? READ_ONCE(pte[sgx_mmu_pt_index(addr)]) : 0;
}

/*
 * Map n pages at addr. Page tables are created on demand and kept until the
 * directory goes; a live entry is never replaced (-EBUSY), so a caller cannot
 * silently redirect a mapping the GPU may be using.
 */
int sgx_mmu_map(struct sgx_mmu_pd *pd, u32 addr, struct page **pages,
		unsigned int n, u32 flags)
{
	unsigned int i;
	u32 a;

	if (addr % PAGE_SIZE || (u64)addr + (u64)n * PAGE_SIZE > 1ull << 32)
		return -EINVAL;
	for (i = 0, a = addr; i < n; i++, a += PAGE_SIZE)
		if (sgx_mmu_lookup(pd, a) || test_bit(sgx_mmu_pd_index(a), pd->over))
			return -EBUSY;	/* live, or a slot a caller owns (sgx_mmu_pd_override) */
	for (i = 0, a = addr; i < n; i++, a += PAGE_SIZE) {
		unsigned int d = sgx_mmu_pd_index(a);
		u32 *pte;

		if (!pd->table[d]) {
			pd->table[d] = sgx_mmu_alloc_uc(pd->gfp);
			if (!pd->table[d]) {
				sgx_mmu_unmap(pd, addr, i);
				return -ENOMEM;
			}
			WRITE_ONCE(pd->pde[d],
				   sgx_mmu_pde(page_to_phys(pd->table[d])));
		}
		if ((u64)page_to_phys(pages[i]) >> 32) {
			sgx_mmu_unmap(pd, addr, i);
			return -EFAULT;		/* the MMU has 32-bit addresses */
		}
		pte = page_address(pd->table[d]);
		WRITE_ONCE(pte[sgx_mmu_pt_index(a)],
			   sgx_mmu_pte(page_to_phys(pages[i]), flags));
	}
	return 0;
}

void sgx_mmu_unmap(struct sgx_mmu_pd *pd, u32 addr, unsigned int n)
{
	unsigned int i;
	u32 a, *pte;

	for (i = 0, a = addr; i < n; i++, a += PAGE_SIZE) {
		pte = sgx_mmu_ptes(pd, a);
		if (pte)
			WRITE_ONCE(pte[sgx_mmu_pt_index(a)], 0);
	}
}

/*
 * Point the directory slot at addr (4 MiB aligned) at a table the caller
 * owns: pde is the whole entry (debugfs vm_pde_*). Refused while the slot's
 * own table has a live PTE or the slot is already pointed elsewhere; maps
 * into the slot are refused until pde 0 puts back the entry the slot had
 * (its own table's, or none). The caller invalidates the BIF.
 */
int sgx_mmu_pd_override(struct sgx_mmu_pd *pd, u32 addr, u32 pde)
{
	unsigned int d = sgx_mmu_pd_index(addr), i;
	u32 *pte;

	if (addr & (SZ_4M - 1))
		return -EINVAL;
	if (!pde) {
		if (!test_bit(d, pd->over))
			return -EINVAL;
		WRITE_ONCE(pd->pde[d], pd->table[d] ?
			   sgx_mmu_pde(page_to_phys(pd->table[d])) : 0);
		clear_bit(d, pd->over);
		return 0;
	}
	if (test_bit(d, pd->over))
		return -EBUSY;
	pte = pd->table[d] ? page_address(pd->table[d]) : NULL;
	for (i = 0; pte && i < SGX_MMU_ENTRIES; i++)
		if (READ_ONCE(pte[i]))
			return -EBUSY;
	set_bit(d, pd->over);
	WRITE_ONCE(pd->pde[d], pde);
	return 0;
}
