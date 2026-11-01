// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for what the debug group's files change in the driver; built
 * only with CONFIG_DRM_GMA500_SGXFIVE_DEBUG, like the files. The parameter
 * buffer of new render contexts.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_render.h"
#include "sgxfive_priv.h"

static const struct sgx_render_addrs addrs = {
	.param_base = 0x10000000, .pbdesc = 0x32152300, .pbblock = 0x32152340,
};

/*
 * debugfs render_pb_pages: a small parameter buffer of 0x62 pages has the
 * thresholds of its size (TA = pages + 0x10, ZLS = global = pages - 0x22,
 * PDS = global - 0x100, here none); the page table chains only its pages.
 */
static void sgxfive_debug_test_pb_small(struct kunit *test)
{
	u32 *desc = kunit_kzalloc(test, SGX_RENDER_PBDESC_SIZE, GFP_KERNEL);
	u32 *block = kunit_kzalloc(test, SGX_RENDER_PBBLOCK_SIZE, GFP_KERNEL);
	u32 *pt = kunit_kzalloc(test, SGX_RENDER_PT_BYTES, GFP_KERNEL);
	const u32 ht = 0x62 << 16 | 1;
	unsigned int i, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, pt);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_ASSERT_NOT_NULL(test, block);
	memset(pt, 0xa5, SGX_RENDER_PT_BYTES);
	sgx_render_pb(desc, block, pt, &addrs, 0x62);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_NUMPAGES / 4], 0x62u);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_INIT_HT / 4], ht);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_INIT_PREV / 4], 0x61u << 16);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_HT / 4], ht);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_TA_THRESH / 4], 0x72u);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_TA_THRESH / 4 + 1], 0x40u);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_TA_THRESH / 4 + 2], 0x40u);
	KUNIT_EXPECT_EQ(test, desc[SGX_RENDER_PB_TA_THRESH / 4 + 3], 0u);
	KUNIT_EXPECT_EQ(test, block[0], 0x62u);
	KUNIT_EXPECT_EQ(test, block[1], ht);
	KUNIT_EXPECT_EQ(test, pt[1], 0xffff0002u);
	KUNIT_EXPECT_EQ(test, pt[0x62], 0x0061ffffu);
	for (i = 0; i < SGX_RENDER_PT_BYTES / 4; i++)
		nonzero += pt[i] != 0;
	KUNIT_EXPECT_EQ(test, nonzero, 0x62u);
}

/* A page count is taken from the smallest with a ZLS threshold to the slot's. */
static void sgxfive_debug_test_pb_pages(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(SGX_RENDER_PB_PAGES), 0x422u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0x62), 0x62u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(SGX_RENDER_PB_MIN_PAGES), 0x23u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0x22), 0x422u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0), 0x422u);
	KUNIT_EXPECT_EQ(test, sgx_render_pb_pages(0x423), 0x422u);
}

static struct kunit_case sgxfive_debug_test_cases[] = {
	KUNIT_CASE(sgxfive_debug_test_pb_small),
	KUNIT_CASE(sgxfive_debug_test_pb_pages),
	{}
};

static struct kunit_suite sgxfive_debug_test_suite = {
	.name = "gma500_sgxfive_debug",
	.test_cases = sgxfive_debug_test_cases,
};

kunit_test_suite(sgxfive_debug_test_suite);
