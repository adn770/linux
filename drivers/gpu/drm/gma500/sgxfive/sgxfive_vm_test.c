// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgxfive_vm.c's range rules: a VM_MAP must be page aligned,
 * non-empty, and lie inside one heap that is present and not reserved. And
 * for the deferred release of what the GPU may still reach: freed only once
 * the kernel-CCB command carrying the BIF invalidation has been processed.
 */
#include <kunit/test.h>
#include <linux/list.h>
#include <linux/module.h>

#include "sgx_render.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

static const struct drm_sgxfive_heap heaps[3] = {
	{ 0x3e000000, 0x10000000, 0, 12 },			/* user */
	{ 0x30000000, 0x00080000, DRM_SGXFIVE_HEAP_RESERVED, 12 },
	{ 0xf0000000, 0, 0, 12 },				/* absent */
};

static void sgxfive_vm_test_range(struct kunit *test)
{
	/* Inside a user heap, at its start and up to its end. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000, 0x1000), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x4dfff000, 0x1000), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000,
						     0x10000000), 0);
	/* Misaligned, empty, crossing the heap end, outside every heap. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000800, 0x1000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000, 0x800),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x3e000000, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x4dfff000, 0x2000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x10000000, 0x1000),
			-EINVAL);
	/* The reserved firmware heap and an absent heap are not mappable. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0x30000000, 0x1000),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0xf0000000, 0x1000),
			-EINVAL);
	/* No wrap-around past 4 GiB. */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(heaps, 3, 0xfffff000,
						     0x100002000ull), -EINVAL);
}

/*
 * The client heap table: render contexts' slots live in two reserved REV1
 * heaps no client can map, 32 parameter slots of 8 MiB, TA-data slots of
 * 4 MiB below the sync heap, each large enough for the RT data's buffers of
 * the largest render target with four region-header arrays, and each the span
 * of one page table.
 */
static void sgxfive_vm_test_render_heaps(struct kunit *test)
{
	const struct drm_sgxfive_heap *p = &sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_PARAMETERS];
	const struct drm_sgxfive_heap *t = &sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_TA_DATA];

	KUNIT_EXPECT_EQ(test, DRM_SGXFIVE_HEAP_COUNT, 10);
	KUNIT_EXPECT_EQ(test, p->base, 0x10000000ull);
	KUNIT_EXPECT_EQ(test, p->size, 32ull * SZ_8M);
	KUNIT_EXPECT_EQ(test, p->flags, DRM_SGXFIVE_HEAP_RESERVED);
	KUNIT_EXPECT_EQ(test, t->base, 0x20000000ull);
	KUNIT_EXPECT_GE(test, t->size, 32ull * SGXFIVE_RENDER_TD_SLOT);
	KUNIT_EXPECT_EQ(test, SGXFIVE_RENDER_TD_SLOT, SZ_4M);
	KUNIT_EXPECT_LE(test, SGX_RENDER_TD_MAX_SIZE, SGXFIVE_RENDER_TD_SLOT);
	KUNIT_EXPECT_LE(test, SGX_RENDER_TD_MAX4_SIZE, SGXFIVE_RENDER_TD_SLOT);
	KUNIT_EXPECT_EQ(test, t->base % SGXFIVE_RENDER_TD_SLOT, 0ull);	/* a page table each */
	KUNIT_EXPECT_LE(test, t->base + 32ull * SGXFIVE_RENDER_TD_SLOT, 0x28000000ull);
	KUNIT_EXPECT_LE(test, t->base + t->size, 0x2d000000ull);
	KUNIT_EXPECT_EQ(test, t->flags, DRM_SGXFIVE_HEAP_RESERVED);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(sgxfive_rev1_heaps,
						     DRM_SGXFIVE_HEAP_COUNT,
						     0x10001000, 0x1000), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_range(sgxfive_rev1_heaps,
						     DRM_SGXFIVE_HEAP_COUNT,
						     0x20000000, 0x1000), -EINVAL);
}

/* Commands done: those sent minus those still between read and write offset. */
static void sgxfive_vm_test_ccb_done(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(10, 10, 10), 10);
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(10, 12, 10), 8);
	/* The offsets count 256 entries and wrap. */
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(300, 1, 255), 298);
	KUNIT_EXPECT_EQ(test, sgxfive_ccb_done(0, 0, 0), 0);
}

struct fake_item {
	struct sgxfive_deferred d;
	int *freed;
};

static void fake_free(struct sgxfive_deferred *d)
{
	(*container_of(d, struct fake_item, d)->freed)++;
}

/* In order, every item whose command is done, and only those. */
static void sgxfive_vm_test_reap(struct kunit *test)
{
	struct fake_item it[3];
	LIST_HEAD(list);
	LIST_HEAD(due);
	int i, freed = 0;

	for (i = 0; i < 3; i++) {
		it[i] = (struct fake_item){ .d = { .after = 5 + i, .free = fake_free },
					    .freed = &freed };
		list_add_tail(&it[i].d.link, &list);
	}
	KUNIT_EXPECT_EQ(test, sgxfive_reap(&list, 4, &due), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&due));
	KUNIT_EXPECT_EQ(test, sgxfive_reap(&list, 6, &due), 2);
	KUNIT_EXPECT_EQ(test, freed, 0);		/* moved, not freed yet */
	sgxfive_free_all(&due);
	KUNIT_EXPECT_EQ(test, freed, 2);
	KUNIT_EXPECT_TRUE(test, list_empty(&due));
	KUNIT_EXPECT_FALSE(test, list_empty(&list));
	KUNIT_EXPECT_EQ(test, sgxfive_reap(&list, U64_MAX, &due), 1);	/* halted: all */
	sgxfive_free_all(&due);
	KUNIT_EXPECT_EQ(test, freed, 3);
	KUNIT_EXPECT_TRUE(test, list_empty(&list));
}

/*
 * debugfs vm_pte_flags and vm_pte_addr: the PTE flags of a VM_MAP are READONLY
 * as the user asks, and the knob's flags only for the mapping that starts at
 * the knob's address; at address 0 (off) or flags 0 every mapping is as
 * before. RO and WO together are taken (a reading row). Only the knob's bits
 * (WO, CACHED, EDMPROT) pass, whatever it holds.
 */
static void sgxfive_vm_test_pte_flags(struct kunit *test)
{
	const u32 a = 0x3e100000;

	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, 0, 0), 0u);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(true, a, 0, 0), (u32)SGX_MMU_RO);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, 0), 0u);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(true, a, a, 0), (u32)SGX_MMU_RO);
	/* off: address 0, whatever the flags */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, 0, SGX_MMU_WO), 0u);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, 0, 0, SGX_MMU_WO), 0u);
	/* only the mapping that starts there */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, SGX_MMU_WO), (u32)SGX_MMU_WO);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a + 0x1000, a, SGX_MMU_WO), 0u);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(true, a - 0x1000, a, SGX_MMU_WO),
			(u32)SGX_MMU_RO);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, SGX_MMU_CACHED),
			(u32)SGX_MMU_CACHED);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, SGX_MMU_EDMPROT),
			(u32)SGX_MMU_EDMPROT);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, 0x1a), 0x1au);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(true, a, a, SGX_MMU_WO),
			(u32)(SGX_MMU_RO | SGX_MMU_WO));
	/* VALID, READONLY and the bits above EDMPROT never come from the knob */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, ~0u), 0x1au);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_pte_flags(false, a, a, SGX_MMU_RO), 0u);
	KUNIT_EXPECT_EQ(test, (u32)SGXFIVE_VM_PTE_KNOB, 0x1au);
}

static struct kunit_case sgxfive_vm_test_cases[] = {
	KUNIT_CASE(sgxfive_vm_test_range),
	KUNIT_CASE(sgxfive_vm_test_render_heaps),
	KUNIT_CASE(sgxfive_vm_test_pte_flags),
	KUNIT_CASE(sgxfive_vm_test_ccb_done),
	KUNIT_CASE(sgxfive_vm_test_reap),
	{}
};

static struct kunit_suite sgxfive_vm_test_suite = {
	.name = "gma500_sgxfive_vm",
	.test_cases = sgxfive_vm_test_cases,
};

kunit_test_suite(sgxfive_vm_test_suite);
