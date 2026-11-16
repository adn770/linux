// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_mmu_large.c: a large-page directory entry over a
 * region the driver owns (debugfs vm_pde_*), its table in each layout, the
 * readings a session tells apart, and the safety rule that under every one
 * of them the GPU reaches only the region. Physical addresses are made up
 * (naturally aligned, as the page allocator gives them).
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgx_mmu.h"

#define R0	0x3c000000u	/* a region address, 4 MiB aligned */
#define R1	0x7fc00000u	/* another, at the top of the 31-bit range */
#define V	SGX_MMU_VALID

static u32 *pt_alloc(struct kunit *test)
{
	u32 *pt = kunit_kcalloc(test, SGX_LARGE_PT_ENTRIES, sizeof(u32), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, pt);
	return pt;
}

/* Bits 3:1 carry the code, VALID is set, code 0 is the directory's own entry. */
static void sgx_mmu_large_test_pde_sized(struct kunit *test)
{
	unsigned int code;

	for (code = 0; code < SGX_MMU_PDE_CODES; code++)
		KUNIT_EXPECT_EQ(test, sgx_mmu_pde_sized(0x01234000, code),
				0x01234001u | code << 1);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_sized(0x01234000, 0), sgx_mmu_pde(0x01234000));
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_sized(0x7ffff000, 0), sgx_mmu_pde(0x7ffff000));
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_sized(0x01234000, 8), sgx_mmu_pde(0x01234000));
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(0), (u32)SZ_4K);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(1), (u32)SZ_16K);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(2), (u32)SZ_64K);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(3), (u32)SZ_256K);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(4), (u32)SZ_1M);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(5), (u32)SZ_4M);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(6), 0u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_pde_page_size(7), 0u);
}

/*
 * The region of each code: a table page and up to three blocks in 4 S
 * (codes 1-4), eight pages (code 0), one 4 MiB block (codes 5-7). Refused:
 * misalign and ptoff for codes 0 and 5 (one block, or 4 KiB pages: neither
 * can tell its readings apart), any layout but dense for 6 and 7, codes and
 * layouts past the last.
 */
static void sgx_mmu_large_test_geom(struct kunit *test)
{
	static const unsigned int order[SGX_MMU_PDE_CODES] = { 3, 4, 6, 8, 10, 10, 10, 10 };
	struct sgx_mmu_large_geom g;
	unsigned int code, layout;

	for (code = 0; code < SGX_MMU_PDE_CODES; code++) {
		for (layout = 0; layout < SGX_LARGE_LAYOUTS; layout++) {
			bool ok = code >= 1 && code <= 4 ? true :
				  code == 0 || code == 5 ? layout <= SGX_LARGE_PT4K :
				  layout == SGX_LARGE_DENSE;
			int ret = sgx_mmu_large_geom(code, layout, &g);

			KUNIT_EXPECT_EQ_MSG(test, ret, ok ? 0 : -EINVAL, "code %u layout %u",
					    code, layout);
			if (ret)
				continue;
			KUNIT_EXPECT_EQ(test, g.code, code);
			KUNIT_EXPECT_EQ(test, g.layout, layout);
			KUNIT_EXPECT_EQ(test, g.order, order[code]);
			KUNIT_EXPECT_EQ(test, g.region, (u32)SZ_4K << order[code]);
			KUNIT_EXPECT_EQ(test, g.region / g.unit * g.unit, g.region);
			KUNIT_EXPECT_LE(test, g.region / g.unit, 15u);	/* the tag's slab field */
			KUNIT_EXPECT_LE(test, g.unit / SZ_4K, 1024u);	/* its page field */
			KUNIT_EXPECT_LE(test, g.nblocks, (unsigned int)SGX_LARGE_MAX_BLOCKS);
			KUNIT_EXPECT_GE(test, g.trap_off, (u32)SZ_4K);	/* never the table */
			KUNIT_EXPECT_LT(test, g.trap_off, g.region);
		}
	}
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_geom(8, 0, &g), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_geom(1, SGX_LARGE_LAYOUTS, &g), -EINVAL);

	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(2, SGX_LARGE_DENSE, &g), 0);
	KUNIT_EXPECT_EQ(test, g.size, (u32)SZ_64K);
	KUNIT_EXPECT_EQ(test, g.unit, (u32)SZ_64K);
	KUNIT_EXPECT_EQ(test, g.nblocks, 3u);
	KUNIT_EXPECT_EQ(test, g.block_off[0], (u32)SZ_64K);
	KUNIT_EXPECT_EQ(test, g.block_off[2], 3u * SZ_64K);
	KUNIT_EXPECT_EQ(test, g.trap_off, 3u * SZ_64K);
	KUNIT_EXPECT_EQ(test, g.pt_off, 0u);
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(2, SGX_LARGE_MISALIGN, &g), 0);
	KUNIT_EXPECT_EQ(test, g.nblocks, 2u);
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(2, SGX_LARGE_PTOFF, &g), 0);
	KUNIT_EXPECT_EQ(test, g.pt_off, (u32)SGX_LARGE_PTOFF_BYTES);
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(0, SGX_LARGE_DENSE, &g), 0);
	KUNIT_EXPECT_EQ(test, g.size, (u32)SZ_4K);
	KUNIT_EXPECT_EQ(test, g.block_off[0], (u32)SZ_4K);
	KUNIT_EXPECT_EQ(test, g.trap_off, 7u * SZ_4K);
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(5, SGX_LARGE_DENSE, &g), 0);
	KUNIT_EXPECT_EQ(test, g.nblocks, 1u);
	KUNIT_EXPECT_EQ(test, g.block_off[0], 0u);
	KUNIT_EXPECT_EQ(test, g.trap_off, (u32)(SZ_4M - SZ_4K));
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(6, SGX_LARGE_DENSE, &g), 0);
	KUNIT_EXPECT_EQ(test, g.size, (u32)SZ_4M);
	KUNIT_EXPECT_EQ(test, g.region, (u32)SZ_4M);
}

/* Each layout's table for codes 1-5: the entries, the decoy and its offset. */
static void sgx_mmu_large_test_layouts(struct kunit *test)
{
	struct sgx_mmu_large_geom g;
	u32 *pt = pt_alloc(test);
	unsigned int code, i;

	for (code = 1; code <= 5; code++) {
		u32 s = sgx_mmu_pde_page_size(code), n = SZ_4M / s;
		u32 trap;

		/* dense: block k at entry k, the trap everywhere else */
		KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(code, SGX_LARGE_DENSE, &g), 0);
		trap = R0 + g.trap_off;
		sgx_mmu_large_table(&g, R0, pt);
		KUNIT_EXPECT_EQ(test, sgx_mmu_large_pde(&g, R0), R0 | code << 1 | V);
		for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++) {
			u32 want = i < g.nblocks ? R0 + g.block_off[i] : trap;

			KUNIT_EXPECT_EQ_MSG(test, pt[i], want | V, "code %u dense %u", code, i);
		}
		for (i = 0; i < g.nblocks; i++)
			KUNIT_EXPECT_EQ(test, (R0 + g.block_off[i]) % s, 0u);
		KUNIT_EXPECT_EQ(test, g.nblocks, code == 5 ? 1u : 3u);

		/* pt4k: entry i is the 4 KiB at i * 4K of the large pages' blocks */
		KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(code, SGX_LARGE_PT4K, &g), 0);
		sgx_mmu_large_table(&g, R0, pt);
		KUNIT_EXPECT_EQ(test, sgx_mmu_large_pde(&g, R0), R0 | code << 1 | V);
		for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++) {
			u32 k = i * SZ_4K / s;
			u32 want = k < g.nblocks ? R0 + g.block_off[k] + i * SZ_4K % s : trap;

			KUNIT_EXPECT_EQ_MSG(test, pt[i], want | V, "code %u pt4k %u", code, i);
		}
		if (code == 5) {
			KUNIT_EXPECT_EQ(test, sgx_mmu_large_geom(5, SGX_LARGE_MISALIGN, &g), -EINVAL);
			KUNIT_EXPECT_EQ(test, sgx_mmu_large_geom(5, SGX_LARGE_PTOFF, &g), -EINVAL);
			continue;
		}

		/* misalign: two blocks, each entry 4 KiB past its block's base */
		KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(code, SGX_LARGE_MISALIGN, &g), 0);
		sgx_mmu_large_table(&g, R0, pt);
		KUNIT_EXPECT_EQ(test, g.nblocks, 2u);
		KUNIT_EXPECT_EQ(test, pt[0], (R0 + s + SZ_4K) | V);
		KUNIT_EXPECT_EQ(test, pt[1], (R0 + 2 * s + SZ_4K) | V);
		for (i = 2; i < SGX_LARGE_PT_ENTRIES; i++)
			KUNIT_EXPECT_EQ(test, pt[i], trap | V);
		/* the spill of block 1's last 4 KiB is the trap slab, in the region */
		KUNIT_EXPECT_LE(test, 3 * s + SZ_4K, g.region);

		/* ptoff: the real table at + 0x400, a decoy naming the blocks rotated */
		KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(code, SGX_LARGE_PTOFF, &g), 0);
		sgx_mmu_large_table(&g, R0, pt);
		KUNIT_EXPECT_EQ(test, sgx_mmu_large_pde(&g, R0), R0 | 0x400 | code << 1 | V);
		KUNIT_EXPECT_EQ(test, (sgx_mmu_large_pde(&g, R0) >> 4) & 0xff, 0x40u);
		KUNIT_EXPECT_LE(test, n, 256u);
		for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++) {
			u32 want = trap;

			if (i < 3)
				want = R0 + g.block_off[(i + 1) % 3];
			else if (i >= 256 && i < 256 + 3)
				want = R0 + g.block_off[i - 256];
			KUNIT_EXPECT_EQ_MSG(test, pt[i], want | V, "code %u ptoff %u", code, i);
		}
	}

	/* code 0: 4 KiB entries over three tagged pages */
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(0, SGX_LARGE_DENSE, &g), 0);
	sgx_mmu_large_table(&g, R0, pt);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_pde(&g, R0), sgx_mmu_pde(R0));
	for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++)
		KUNIT_EXPECT_EQ(test, pt[i], (i < 3 ? R0 + (i + 1) * SZ_4K : R0 + 7 * SZ_4K) | V);

	/* codes 6, 7: every 4 KiB entry its own page of one block, never the table */
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(7, SGX_LARGE_DENSE, &g), 0);
	sgx_mmu_large_table(&g, R0, pt);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_pde(&g, R0), R0 | 0xfu);
	KUNIT_EXPECT_EQ(test, pt[0], (R0 + SZ_4K) | V);
	for (i = 1; i < SGX_LARGE_PT_ENTRIES; i++)
		KUNIT_EXPECT_EQ(test, pt[i], (R0 + i * SZ_4K) | V);
}

/* The readings on a hand-made table: each takes its own index, mask and base. */
static void sgx_mmu_large_test_phys(struct kunit *test)
{
	u32 *pt = pt_alloc(test);
	u32 pde = 0x01000000 | 0x400 | 2 << 1 | V;	/* 64 KiB, table at + 0x400 */

	pt[0] = 0x02010000 | V;
	pt[1] = 0x02021000 | V;		/* 4 KiB past a 64 KiB boundary */
	pt[2] = 0x02030000;		/* not valid */
	pt[256] = 0x02050000 | V;
	pt[257] = 0x02061000 | V;
	pt[17] = 0x02070000 | V;

	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_DENSE, SZ_64K, pde, pt, 0x1234), 0x02011234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_DENSE, SZ_64K, pde, pt, 0x11234), 0x02021234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_MASK4K, SZ_64K, pde, pt, 0x11234), 0x02022234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_4K, SZ_64K, pde, pt, 0x11234), 0x02070234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_SECTION, SZ_64K, pde, pt, 0x11234), 0x01011234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_DENSE | SGX_RD_PTOFF, SZ_64K, pde, pt,
						 0x11234), 0x02061234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_MASK4K | SGX_RD_PTOFF, SZ_64K, pde, pt,
						 0x11234), 0x02062234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_4K | SGX_RD_PTOFF, SZ_64K, pde, pt,
						 0x234), 0x02050234ull);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_SECTION | SGX_RD_PTOFF, SZ_64K, pde, pt,
						 0x11234), 0x01011634ull);
	/* an entry without VALID faults; an index past the table page is outside it */
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_DENSE, SZ_64K, pde, pt, 0x20000),
			SGX_LARGE_FAULT);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_4K | SGX_RD_PTOFF, SZ_64K, pde, pt,
						 0x300000), SGX_LARGE_OUTSIDE);
	/* 4 MiB: one entry */
	pt[0] = 0x02400000 | V;
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_phys(SGX_RD_DENSE, SZ_4M, pde, pt, 0x3ff004),
			0x027ff004ull);
}

/*
 * The session's rows (Q109, r30-pde), the contract the safety rule holds
 * for: VA offsets in the 4 MiB slot that a copy or fill of each code may
 * touch, in any layout the code takes. Codes 6 and 7 are read only (a copy's
 * source), the others read and written.
 */
struct large_row {
	unsigned int code;
	u32 start, end;
	bool write;
};

static unsigned int large_rows(unsigned int code, struct large_row *r)
{
	u32 s = sgx_mmu_pde_page_size(code);

	if (code == 0) {
		r[0] = (struct large_row){ 0, 0, SZ_16K, true };
		r[1] = (struct large_row){ 0, SZ_4M - SZ_8K, SZ_4M, true };
		return 2;
	}
	if (code <= 4) {
		r[0] = (struct large_row){ code, s - SZ_8K, s + SZ_8K, true };
		r[1] = (struct large_row){ code, 2 * s - SZ_8K, 2 * s + SZ_8K, true };
		return 2;
	}
	if (code == 5) {
		r[0] = (struct large_row){ 5, SZ_4M - SZ_16K, SZ_4M, true };
		return 1;
	}
	r[0] = (struct large_row){ code, SZ_16K, SZ_32K, false };
	return 1;
}

/* The readings to check for a code, and the sizes each may assume. */
static bool large_reading_applies(unsigned int code, unsigned int rd)
{
	return code ? true : (rd & ~SGX_RD_PTOFF) == SGX_RD_4K;
}

static bool large_check(struct kunit *test, const struct sgx_mmu_large_geom *g, u32 r,
			const u32 *pt, const struct large_row *row)
{
	u32 pde = sgx_mmu_large_pde(g, r);
	u32 lo = row->write ? r + SZ_4K : r, hi = r + g->region;
	unsigned int rd, si, nsizes = g->code < SGX_MMU_PDE_NAMED ? 1 : SGX_MMU_PDE_NAMED;
	bool ok = true;
	u32 va;

	for (rd = 0; rd < 2 * SGX_RD_N; rd++) {
		unsigned int reading = rd % SGX_RD_N | (rd >= SGX_RD_N ? SGX_RD_PTOFF : 0);

		if (!large_reading_applies(g->code, reading))
			continue;
		for (si = 0; si < nsizes; si++) {
			u32 size = g->code < SGX_MMU_PDE_NAMED ? g->size : sgx_mmu_pde_page_size(si);

			for (va = row->start; va < row->end; va += SZ_4K) {
				u64 a = sgx_mmu_large_phys(reading, size, pde, pt, va);
				u64 b = sgx_mmu_large_phys(reading, size, pde, pt, va + SZ_4K - 4);

				if (a == SGX_LARGE_FAULT && b == SGX_LARGE_FAULT)
					continue;	/* no access at all */
				if (a < lo || a >= hi || b < lo || b + 4 > hi || b != a + SZ_4K - 4) {
					KUNIT_FAIL(test, "code %u layout %u region 0x%08x reading 0x%x size 0x%x va 0x%06x: 0x%llx..0x%llx outside 0x%08x..0x%08x",
						   g->code, g->layout, r, reading, size, va, a, b, lo, hi);
					ok = false;
				}
			}
		}
	}
	return ok;
}

/*
 * The safety rule: for every row of the session, under every reading (and
 * every named size for codes 6 and 7), in every layout a code takes and at
 * two region addresses, each 4 KiB the GPU reaches lies in the region, and a
 * written one is never the table page. Every entry of the table page is
 * valid and names the region (none the table page but code 5's one block,
 * whose base it is), so even a reading not foreseen here that walks the
 * table at 4 KiB granularity lands in owned memory.
 */
static void sgx_mmu_large_test_safety(struct kunit *test)
{
	static const u32 regions[] = { R0, R1 };
	struct sgx_mmu_large_geom g;
	struct large_row rows[2];
	u32 *pt = pt_alloc(test);
	unsigned int code, layout, ri, i, n, checked = 0;

	for (code = 0; code < SGX_MMU_PDE_CODES; code++) {
		n = large_rows(code, rows);
		for (layout = 0; layout < SGX_LARGE_LAYOUTS; layout++) {
			if (sgx_mmu_large_geom(code, layout, &g))
				continue;
			for (ri = 0; ri < ARRAY_SIZE(regions); ri++) {
				u32 r = regions[ri];

				KUNIT_ASSERT_EQ(test, r % g.region, 0u);
				sgx_mmu_large_table(&g, r, pt);
				for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++) {
					u32 e = pt[i] & ~0xfffu;

					KUNIT_EXPECT_TRUE(test, pt[i] & V);
					KUNIT_EXPECT_GE(test, e, r);
					KUNIT_EXPECT_LT(test, e, r + g.region);
					/* only code 5's block starts at the table page */
					if (code != 5)
						KUNIT_EXPECT_NE(test, e, r);
				}
				for (i = 0; i < n; i++)
					if (large_check(test, &g, r, pt, &rows[i]))
						checked++;
			}
		}
	}
	/* 2 + 4 x 2 x 4 + 2 x 1 + 2 x 1 rows per region, all of them */
	KUNIT_EXPECT_EQ(test, checked, 2u * (4 + 32 + 2 + 2));
}

/* A block's tags, and the read-out's scan of a page with three changed dwords. */
static void sgx_mmu_large_test_tags(struct kunit *test)
{
	struct sgx_mmu_large_geom g;
	struct sgx_mmu_large_diff d;
	u32 *page = pt_alloc(test);
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sgx_mmu_large_tag(0, 0, 0), 0xa0000000u);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_tag(3, 0x3ff, 0x3ff), 0xa33ff3ffu);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_tag(1, 2, 5), 0xa1002005u);

	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(2, SGX_LARGE_DENSE, &g), 0);
	/* slab 2, page 3 of a 64 KiB slab */
	sgx_mmu_large_fill(&g, 2 * SZ_64K + 3 * SZ_4K, page);
	for (i = 0; i < SGX_LARGE_PT_ENTRIES; i++)
		KUNIT_EXPECT_EQ(test, page[i], 0xa2003000u | i);
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_scan(&g, 2 * SZ_64K + 3 * SZ_4K, page, &d), 0u);
	KUNIT_EXPECT_EQ(test, d.changed, 0u);
	/* the same page read as another one: every dword differs */
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_scan(&g, 2 * SZ_64K + 4 * SZ_4K, page, &d), 1024u);
	KUNIT_EXPECT_EQ(test, d.first[7].off, 28u);

	page[0] = 0;
	page[0x100] = 0xdeadbeef;
	page[0x3ff] = 0xa2003000u;	/* dword 0's tag in the last dword */
	KUNIT_EXPECT_EQ(test, sgx_mmu_large_scan(&g, 2 * SZ_64K + 3 * SZ_4K, page, &d), 3u);
	KUNIT_EXPECT_EQ(test, d.changed, 3u);
	KUNIT_EXPECT_EQ(test, d.first[0].off, 0u);
	KUNIT_EXPECT_EQ(test, d.first[0].val, 0u);
	KUNIT_EXPECT_EQ(test, d.first[1].off, 0x400u);
	KUNIT_EXPECT_EQ(test, d.first[1].val, 0xdeadbeefu);
	KUNIT_EXPECT_EQ(test, d.first[2].off, 0xffcu);
	KUNIT_EXPECT_EQ(test, d.first[2].val, 0xa2003000u);

	/* codes 5-7: one 4 MiB slab, its pages 1 to 1023 */
	KUNIT_ASSERT_EQ(test, sgx_mmu_large_geom(5, SGX_LARGE_DENSE, &g), 0);
	sgx_mmu_large_fill(&g, SZ_4M - SZ_4K, page);
	KUNIT_EXPECT_EQ(test, page[1], 0xa03ff001u);
}

static struct kunit_case sgx_mmu_large_test_cases[] = {
	KUNIT_CASE(sgx_mmu_large_test_pde_sized),
	KUNIT_CASE(sgx_mmu_large_test_geom),
	KUNIT_CASE(sgx_mmu_large_test_layouts),
	KUNIT_CASE(sgx_mmu_large_test_phys),
	KUNIT_CASE(sgx_mmu_large_test_safety),
	KUNIT_CASE(sgx_mmu_large_test_tags),
	{}
};

static struct kunit_suite sgx_mmu_large_test_suite = {
	.name = "gma500_sgx_mmu_large",
	.test_cases = sgx_mmu_large_test_cases,
};

kunit_test_suite(sgx_mmu_large_test_suite);
