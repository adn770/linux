// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for what the debug group's files change in the driver; built
 * only with CONFIG_DRM_GMA500_SGXFIVE_DEBUG, like the files. The parameter
 * buffer of new render contexts.
 * The copies of a render context's pages and the context they come from.
 * The first MiB VM_MAP may take.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/dma-fence.h>
#include <linux/mm.h>

#include "sgx_render.h"
#include "sgxfive_ctx.h"
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

/* A context with its memory in a plain buffer, as far as the dumps need one. */
static struct sgxfive_ctx *fake_ctx(struct kunit *test)
{
	struct sgxfive_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->mem.va = kunit_kzalloc(test, SGXFIVE_CTX_MEM_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->mem.va);
	ctx->fence_context = dma_fence_context_alloc(1);
	spin_lock_init(&ctx->fence_lock);
	INIT_LIST_HEAD(&ctx->pending);
	return ctx;
}

/*
 * The debugfs dumps of a render context's pages: a range copies across page
 * boundaries, stops at the end of the pages, and gives nothing past them.
 */
static void sgxfive_debug_test_pages_copy(struct kunit *test)
{
	struct page *pages[3];
	u8 *buf = kunit_kzalloc(test, PAGE_SIZE + 16, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
		memset(page_address(pages[i]), 0x10 * (i + 1), PAGE_SIZE);
	}
	KUNIT_EXPECT_EQ(test, sgxfive_pages_copy(pages, 3, buf, 16, PAGE_SIZE - 8), 16);
	KUNIT_EXPECT_EQ(test, buf[7], 0x10);
	KUNIT_EXPECT_EQ(test, buf[8], 0x20);
	KUNIT_EXPECT_EQ(test, sgxfive_pages_copy(pages, 3, buf, PAGE_SIZE + 16,
						 2 * PAGE_SIZE + 8), PAGE_SIZE - 8);
	KUNIT_EXPECT_EQ(test, buf[0], 0x30);
	KUNIT_EXPECT_EQ(test, buf[PAGE_SIZE - 9], 0x30);
	KUNIT_EXPECT_EQ(test, sgxfive_pages_copy(pages, 3, buf, 16, 3 * PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_pages_copy(pages, 3, buf, 16, -1), 0);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * The context the render dumps read: the newest render context, with a
 * reference; transfer contexts and one already being released are skipped.
 */
static void sgxfive_debug_test_render_newest(struct kunit *test)
{
	static const u32 types[] = {
		DRM_SGXFIVE_CTX_TYPE_TRANSFER, DRM_SGXFIVE_CTX_TYPE_RENDER,
		DRM_SGXFIVE_CTX_TYPE_RENDER, DRM_SGXFIVE_CTX_TYPE_TRANSFER,
	};
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	struct sgxfive_ctx *c[ARRAY_SIZE(types)];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	KUNIT_EXPECT_NULL(test, sgxfive_render_newest(sgx));
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		c[i] = fake_ctx(test);
		c[i]->type = types[i];
		kref_init(&c[i]->ref);
		list_add_tail(&c[i]->link, &sgx->ctx_list);
	}
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_render_newest(sgx), c[2]);
	KUNIT_EXPECT_EQ(test, kref_read(&c[2]->ref), 2u);
	refcount_set(&c[2]->ref.refcount, 0);	/* being released */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_render_newest(sgx), c[1]);
	KUNIT_EXPECT_EQ(test, kref_read(&c[1]->ref), 2u);
}

/* debugfs vm_low: the first MiB, page 0 excepted, opened to VM_MAP. */
static void sgxfive_debug_test_vm_low(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0x1000, 0x1000), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0xff000, 0x1000), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0x1000, 0xff000), 0);
	/* page 0, past the first MiB, misaligned, empty */
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0, 0x1000), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0xff000, 0x2000), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0x1800, 0x1000), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0x1000, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_vm_check_low(0xfffff000, 0x100002000ull), -EINVAL);
}

static struct kunit_case sgxfive_debug_test_cases[] = {
	KUNIT_CASE(sgxfive_debug_test_pb_small),
	KUNIT_CASE(sgxfive_debug_test_pb_pages),
	KUNIT_CASE(sgxfive_debug_test_pages_copy),
	KUNIT_CASE(sgxfive_debug_test_render_newest),
	KUNIT_CASE(sgxfive_debug_test_vm_low),
	{}
};

static struct kunit_suite sgxfive_debug_test_suite = {
	.name = "gma500_sgxfive_debug",
	.test_cases = sgxfive_debug_test_cases,
};

kunit_test_suite(sgxfive_debug_test_suite);
