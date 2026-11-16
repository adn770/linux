// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * A large-page directory entry over memory the driver owns, for debugfs
 * vm_pde_* (Q109: does a PDE whose size field names 16 KiB to 4 MiB
 * translate, with which mask and which table layout?). Pure functions: the
 * region's geometry, its PDE and table, the tags of its pages, and the
 * readings of a large entry a session tells apart.
 *
 * A client's mapped pages are not physically contiguous, so a large page
 * cannot be a buffer's own: the knob maps one 4 MiB directory slot over a
 * naturally aligned region of contiguous pages instead. Page 0 of the region
 * is the table page; every other 4 KiB holds tags naming its slab and page,
 * so a copy's destination shows which physical page each 4 KiB came from.
 *
 *   code 0      eight pages: blocks at pages 1-3, the trap at page 7
 *   codes 1-4   4 S: the table's slab, blocks at slabs 1-3 (misalign: 1-2,
 *               slab 3 takes block 1's last 4 KiB), the trap at slab 3
 *   code 5      one 4 MiB block, the table its first page, the trap its last
 *   codes 6, 7  one 4 MiB block, each 4 KiB entry naming its own page
 *
 * The geometry is chosen so that under every reading of sgx_mmu_large_phys()
 * the session's rows reach only the region, and a write never the table
 * page (sgx_mmu_large_test.c's safety case holds the rows).
 */
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/string.h>

#include "sgx_mmu.h"

int sgx_mmu_large_geom(unsigned int code, unsigned int layout,
		       struct sgx_mmu_large_geom *g)
{
	unsigned int i;
	u32 s;

	if (code >= SGX_MMU_PDE_CODES || layout >= SGX_LARGE_LAYOUTS)
		return -EINVAL;
	/* one block or 4 KiB pages: misalign and the decoy tell nothing apart */
	if ((code == 0 || code == 5) && layout > SGX_LARGE_PT4K)
		return -EINVAL;
	if (code >= SGX_MMU_PDE_NAMED && layout != SGX_LARGE_DENSE)
		return -EINVAL;
	s = code < SGX_MMU_PDE_NAMED ? sgx_mmu_pde_page_size(code) : SZ_4M;
	memset(g, 0, sizeof(*g));
	g->code = code;
	g->layout = layout;
	g->size = s;
	g->unit = s;
	g->pt_off = layout == SGX_LARGE_PTOFF ? SGX_LARGE_PTOFF_BYTES : 0;
	if (code == 0) {
		g->region = 8 * SZ_4K;
		g->nblocks = 3;
		for (i = 0; i < g->nblocks; i++)
			g->block_off[i] = (i + 1) * SZ_4K;
		g->trap_off = 7 * SZ_4K;
	} else if (code < 5) {
		g->region = 4 * s;
		g->nblocks = layout == SGX_LARGE_MISALIGN ? 2 : 3;
		for (i = 0; i < g->nblocks; i++)
			g->block_off[i] = (i + 1) * s;
		g->trap_off = 3 * s;
	} else {
		g->region = SZ_4M;
		g->nblocks = 1;
		g->block_off[0] = 0;
		g->trap_off = SZ_4M - SZ_4K;
	}
	g->order = ilog2(g->region / SZ_4K);
	return 0;
}

u32 sgx_mmu_large_pde(const struct sgx_mmu_large_geom *g, u32 region_phys)
{
	return sgx_mmu_pde_sized(region_phys | g->pt_off, g->code);
}

/* The 1024 entries of the table page; every one valid and in the region. */
void sgx_mmu_large_table(const struct sgx_mmu_large_geom *g, u32 region_phys,
			 u32 *pt)
{
	u32 trap = sgx_mmu_pte(region_phys + g->trap_off, 0);
	unsigned int i, k;

	for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++)
		pt[i] = trap;
	if (g->code >= SGX_MMU_PDE_NAMED) {
		for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++)
			pt[i] = sgx_mmu_pte(region_phys + (i ? i : 1) * SZ_4K, 0);
		return;
	}
	switch (g->layout) {
	case SGX_LARGE_PT4K:
		for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++) {
			k = i * SZ_4K / g->size;
			if (k < g->nblocks)
				pt[i] = sgx_mmu_pte(region_phys + g->block_off[k] +
						    i * SZ_4K % g->size, 0);
		}
		break;
	case SGX_LARGE_MISALIGN:
		for (k = 0; k < g->nblocks; k++)
			pt[k] = sgx_mmu_pte(region_phys + g->block_off[k] + SZ_4K, 0);
		break;
	case SGX_LARGE_PTOFF:
		for (k = 0; k < g->nblocks; k++) {
			pt[g->pt_off / 4 + k] = sgx_mmu_pte(region_phys + g->block_off[k], 0);
			/* the decoy names the blocks rotated by one */
			pt[k] = sgx_mmu_pte(region_phys + g->block_off[(k + 1) % g->nblocks], 0);
		}
		break;
	default:
		for (k = 0; k < g->nblocks; k++)
			pt[k] = sgx_mmu_pte(region_phys + g->block_off[k], 0);
		break;
	}
}

/*
 * The physical address VA offset va (in the 4 MiB slot) reaches under a
 * reading of a PDE over table page pt, assuming page size size:
 *   SGX_RD_DENSE    entry va / size, its address masked to size, + va % size
 *   SGX_RD_4K       entry va / 4 KiB, + va % 4 KiB (the size field ignored)
 *   SGX_RD_MASK4K   entry va / size, its address masked to 4 KiB, + va % size
 *   SGX_RD_SECTION  the PDE's address + va (no table)
 * With SGX_RD_PTOFF the table (or the section) starts at the PDE's bits 11:4
 * instead of its page. SGX_LARGE_FAULT: the entry is not valid, nothing is
 * reached; SGX_LARGE_OUTSIDE: the index runs past the table page.
 */
u64 sgx_mmu_large_phys(unsigned int reading, u32 size, u32 pde, const u32 *pt,
		       u32 va)
{
	u32 off = reading & SGX_RD_PTOFF ? pde & 0xff0 : 0;
	u32 base = pde & 0xfffff000;
	unsigned int idx;
	u32 e;

	switch (reading & ~SGX_RD_PTOFF) {
	case SGX_RD_SECTION:
		return (u64)base + off + va;
	case SGX_RD_4K:
		idx = va / SZ_4K;
		break;
	default:
		idx = va / size;
		break;
	}
	idx += off / 4;
	if (idx >= SGX_LARGE_PT_ENTRIES)
		return SGX_LARGE_OUTSIDE;
	e = pt[idx];
	if (!(e & SGX_MMU_VALID))
		return SGX_LARGE_FAULT;
	switch (reading & ~SGX_RD_PTOFF) {
	case SGX_RD_4K:
		return (u64)(e & 0xfffff000) + va % SZ_4K;
	case SGX_RD_MASK4K:
		return (u64)(e & 0xfffff000) + va % size;
	default:
		return (u64)(e & 0xfffff000 & ~(size - 1)) + va % size;
	}
}

/* Tag the 4 KiB at region offset off. */
void sgx_mmu_large_fill(const struct sgx_mmu_large_geom *g, u32 off, u32 *page)
{
	unsigned int slab = off / g->unit, pg = off % g->unit / SZ_4K, i;

	for (i = 0; i < SZ_4K / 4; i++)
		page[i] = sgx_mmu_large_tag(slab, pg, i);
}

/* The dwords of the 4 KiB at region offset off that are not its tags. */
unsigned int sgx_mmu_large_scan(const struct sgx_mmu_large_geom *g, u32 off,
				const u32 *page, struct sgx_mmu_large_diff *d)
{
	unsigned int slab = off / g->unit, pg = off % g->unit / SZ_4K, i;

	memset(d, 0, sizeof(*d));
	for (i = 0; i < SZ_4K / 4; i++) {
		u32 v = page[i];

		if (v == sgx_mmu_large_tag(slab, pg, i))
			continue;
		if (d->changed < SGX_LARGE_DIFFS) {
			d->first[d->changed].off = i * 4;
			d->first[d->changed].val = v;
		}
		d->changed++;
	}
	return d->changed;
}
