/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The SGX MMU's page tables: one 4 KiB page directory of 1024 PDEs per GPU
 * address space, each naming a 4 KiB page table of 1024 PTEs (4 KiB pages,
 * 32-bit device addresses, physical addresses below 4 GiB).
 */
#ifndef _GMA500_SGX_MMU_H_
#define _GMA500_SGX_MMU_H_

#include <linux/sizes.h>
#include <linux/types.h>

struct page;
struct sgx_mmu_pd;

#define SGX_MMU_VALID	0x1
#define SGX_MMU_WO	0x2	/* write-only */
#define SGX_MMU_RO	0x4	/* read-only */
#define SGX_MMU_CACHED	0x8	/* cache-coherent (snooped) */
#define SGX_MMU_EDMPROT	0x10	/* EDM-protected (sgxmmu.h EDMPROTECT) */

static inline u32 sgx_mmu_pde(u32 pt_phys)
{
	return pt_phys | SGX_MMU_VALID;
}

static inline u32 sgx_mmu_pte(u32 page_phys, u32 flags)
{
	return page_phys | flags | SGX_MMU_VALID;
}

static inline unsigned int sgx_mmu_pd_index(u32 addr)
{
	return addr >> 22;
}

static inline unsigned int sgx_mmu_pt_index(u32 addr)
{
	return (addr >> 12) & 0x3ff;
}

/*
 * A PDE's bits 3:1 name the page size of its table's entries (the GPL CE
 * DDK's sgxmmu.h): codes 0-5 for 4K, 16K, 64K, 256K, 1M and 4M; 6 and 7 are
 * unnamed. Every directory entry the driver writes is code 0.
 */
#define SGX_MMU_PDE_SIZE_SHIFT	1
#define SGX_MMU_PDE_CODES	8
#define SGX_MMU_PDE_NAMED	6

static inline u32 sgx_mmu_pde_sized(u32 pt_phys, unsigned int code)
{
	return pt_phys | (code & 7) << SGX_MMU_PDE_SIZE_SHIFT | SGX_MMU_VALID;
}

/* The page size a named code gives its table's entries, 0 for 6 and 7. */
static inline u32 sgx_mmu_pde_page_size(unsigned int code)
{
	return code < SGX_MMU_PDE_NAMED ? SZ_4K << 2 * code : 0;
}

/*
 * A large-page directory entry over memory the driver owns (debugfs
 * vm_pde_*, sgx_mmu_large.c): one 4 MiB directory slot, its table and its
 * data in one naturally aligned region of contiguous pages. Page 0 of the
 * region is the table page; every other 4 KiB of it is tagged.
 */
enum sgx_mmu_large_layout {
	SGX_LARGE_DENSE,	/* 4 MiB / S entries, entry k = block k */
	SGX_LARGE_PT4K,		/* 1024 entries of 4 KiB over the blocks */
	SGX_LARGE_MISALIGN,	/* dense, entry k = block k + 4 KiB */
	SGX_LARGE_PTOFF,	/* dense at table + 0x400 (PDE bits 11:4), a decoy at + 0 */
	SGX_LARGE_LAYOUTS
};

#define SGX_LARGE_PTOFF_BYTES	0x400
#define SGX_LARGE_MAX_BLOCKS	3
#define SGX_LARGE_PT_ENTRIES	1024

struct sgx_mmu_large_geom {
	unsigned int code, layout;
	u32 size;		/* S, the code's page size (codes 6, 7: 4 MiB units) */
	u32 unit;		/* the tag's slab size */
	u32 region;		/* bytes of the region, a power of two */
	unsigned int order;	/* its page order */
	unsigned int nblocks;	/* large pages backed by a block */
	u32 block_off[SGX_LARGE_MAX_BLOCKS];	/* their offsets in the region */
	u32 trap_off;		/* what every other entry names */
	u32 pt_off;		/* the table's offset in the table page */
};

/* The readings sgx_mmu_large_phys() tells apart (SGX_RD_PTOFF: offset table). */
enum {
	SGX_RD_DENSE,		/* index va / S, PTE masked to S */
	SGX_RD_4K,		/* the size field ignored */
	SGX_RD_MASK4K,		/* index va / S, PTE masked to 4 KiB */
	SGX_RD_SECTION,		/* the PDE names the data, no table */
	SGX_RD_N
};
#define SGX_RD_PTOFF		0x10	/* the table starts at the PDE's bits 11:4 */
#define SGX_LARGE_OUTSIDE	(~0ull)	/* an entry outside the table page */
#define SGX_LARGE_FAULT		(~0ull - 1)	/* an entry without VALID */

int sgx_mmu_large_geom(unsigned int code, unsigned int layout,
		       struct sgx_mmu_large_geom *g);
u32 sgx_mmu_large_pde(const struct sgx_mmu_large_geom *g, u32 region_phys);
void sgx_mmu_large_table(const struct sgx_mmu_large_geom *g, u32 region_phys,
			 u32 *pt);
u64 sgx_mmu_large_phys(unsigned int reading, u32 size, u32 pde, const u32 *pt,
		       u32 va);

/* The tag of dword i of page p of slab s: 0xa0000000 | s << 24 | p << 12 | i. */
static inline u32 sgx_mmu_large_tag(unsigned int slab, unsigned int page,
				    unsigned int dword)
{
	return 0xa0000000u | (slab & 0xf) << 24 | (page & 0x3ff) << 12 | (dword & 0x3ff);
}

#define SGX_LARGE_DIFFS		8
struct sgx_mmu_large_diff {
	unsigned int changed;	/* dwords that are not their tag */
	struct { u16 off; u32 val; } first[SGX_LARGE_DIFFS];	/* byte offsets */
};

void sgx_mmu_large_fill(const struct sgx_mmu_large_geom *g, u32 off, u32 *page);
unsigned int sgx_mmu_large_scan(const struct sgx_mmu_large_geom *g, u32 off,
				const u32 *page, struct sgx_mmu_large_diff *d);

struct sgx_mmu_pd *sgx_mmu_pd_create(void);
struct sgx_mmu_pd *sgx_mmu_pd_create_user(void);
void sgx_mmu_pd_destroy(struct sgx_mmu_pd *pd);
u32 sgx_mmu_pd_phys(const struct sgx_mmu_pd *pd);
int sgx_mmu_map(struct sgx_mmu_pd *pd, u32 addr, struct page **pages,
		unsigned int n, u32 flags);
void sgx_mmu_unmap(struct sgx_mmu_pd *pd, u32 addr, unsigned int n);
u32 sgx_mmu_lookup(const struct sgx_mmu_pd *pd, u32 addr);
u32 sgx_mmu_pd_entry(const struct sgx_mmu_pd *pd, unsigned int i);

#endif
