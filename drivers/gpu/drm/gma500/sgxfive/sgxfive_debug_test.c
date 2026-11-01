// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for what the debug group's files change in the driver; built
 * only with CONFIG_DRM_GMA500_SGXFIVE_DEBUG, like the files. The parameter
 * buffer of new render contexts.
 * The copies of a render context's pages and the context they come from.
 * The first MiB VM_MAP may take.
 * The extra flags of new render contexts and the trace of their cleanup.
 * The timer clock the boot writes into HostCtl.
 * Recoveries held, and renders that keep their RT data set.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/dma-fence.h>
#include <linux/mm.h>

#include <drm/drm_print.h>

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

/* debugfs render_ctx_flags: extra bits of a new context's flags word, NEW kept. */
static void sgxfive_debug_test_ctx_flags(struct kunit *test)
{
	u32 *ctx = kunit_kzalloc(test, SGX_RENDER_CTX_SIZE, GFP_KERNEL);
	struct sgx_render_addrs a = addrs;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	sgx_render_ctx(ctx, &a);
	KUNIT_EXPECT_EQ(test, ctx[SGX_RENDER_CTX_FLAGS / 4], (u32)SGX_RENDER_CTX_NEW);
	a.ctx_flags = 0x20;
	sgx_render_ctx(ctx, &a);
	KUNIT_EXPECT_EQ(test, ctx[SGX_RENDER_CTX_FLAGS / 4], 0x20u | SGX_RENDER_CTX_NEW);
	KUNIT_EXPECT_EQ(test, ctx[SGX_RENDER_CTX_PBDESC / 4], addrs.pbdesc);
}

/* debugfs render_cleanup: the loaded-PB slots after each cleanup step. */
static void sgxfive_debug_test_cleanup(struct kunit *test)
{
	struct sgxfive_cleanup_trace *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	u32 *ta3d = kunit_kzalloc(test, SGXFIVE_TA3D_CTL_DWORDS * 4, GFP_KERNEL);
	char *buf = kunit_kzalloc(test, 512, GFP_KERNEL);
	struct drm_print_iterator it = { .data = buf, .remain = 511 };
	struct drm_printer p = drm_coredump_printer(&it);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, ta3d);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	sgxfive_cleanup_start(t, 0x10, 0x20, 0x30, 0x21);
	ta3d[SGXFIVE_TA3D_PB_TA] = 0x30;
	ta3d[SGXFIVE_TA3D_PB_3D] = 0x30;
	sgxfive_cleanup_note(t, ta3d);			/* before */
	sgxfive_cleanup_note(t, ta3d);			/* after RT */
	ta3d[SGXFIVE_TA3D_PB_TA] = 0;
	ta3d[SGXFIVE_TA3D_PB_3D] = 0;
	sgxfive_cleanup_note(t, ta3d);			/* after RC */
	for (i = 0; i < 3; i++)				/* PB, then nothing more */
		sgxfive_cleanup_note(t, ta3d);
	t->ret = 0;
	KUNIT_EXPECT_EQ(test, t->steps, SGXFIVE_CLEANUP_STEPS);

	sgxfive_cleanup_print(&p, t);
	KUNIT_EXPECT_STREQ(test, buf,
			   "rtds 0x00000010 ctx 0x00000020 pbdesc 0x00000030 flags 0x00000021 ret 0\n"
			   "before ta 0x00000030 3d 0x00000030\n"
			   "rt ta 0x00000030 3d 0x00000030\n"
			   "rc ta 0x00000000 3d 0x00000000\n"
			   "pb ta 0x00000000 3d 0x00000000\n");

	/* A cleanup that stopped early shows only the steps it took. */
	memset(buf, 0, 512);
	it = (struct drm_print_iterator){ .data = buf, .remain = 511 };
	sgxfive_cleanup_start(t, 1, 2, 3, 1);
	sgxfive_cleanup_note(t, ta3d);
	t->ret = -ETIMEDOUT;
	sgxfive_cleanup_print(&p, t);
	KUNIT_EXPECT_STREQ(test, buf,
			   "rtds 0x00000001 ctx 0x00000002 pbdesc 0x00000003 flags 0x00000001 ret -110\n"
			   "before ta 0x00000000 3d 0x00000000\n");
}

/* debugfs timer_clock: the boot writes it into HostCtl, 0 keeps the default. */
static void sgxfive_debug_test_timer_clock(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_timer_clock(sgx, 400000), 400000u);
	sgx->timer_clock = 100000;
	KUNIT_EXPECT_EQ(test, sgxfive_timer_clock(sgx, 400000), 100000u);
	sgx->timer_clock = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_timer_clock(sgx, 400000), 400000u);
}

/*
 * debugfs hold_recovery: the detectors' recoveries are held while it is set,
 * a reset asked for never is; the next reset ends the hold. rtds_keep reads
 * through.
 */
static void sgxfive_debug_test_hold_recovery(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, 3, SGXFIVE_HANG_FAULT));
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, 3, SGXFIVE_HANG_TIMEOUT));
	KUNIT_EXPECT_FALSE(test, sgx->recovery_held);

	sgx->hold_recovery = true;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, 3, SGXFIVE_HANG_MANUAL));
	KUNIT_EXPECT_FALSE(test, sgx->recovery_held);
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, 3, SGXFIVE_HANG_FAULT));
	KUNIT_EXPECT_TRUE(test, sgx->recovery_held);
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, 3, SGXFIVE_HANG_TIMEOUT));
	sgxfive_debug_reset(sgx);
	KUNIT_EXPECT_FALSE(test, sgx->recovery_held);

	KUNIT_EXPECT_FALSE(test, sgxfive_rtds_keep(sgx));
	sgx->rtds_keep = true;
	KUNIT_EXPECT_TRUE(test, sgxfive_rtds_keep(sgx));
}

static struct kunit_case sgxfive_debug_test_cases[] = {
	KUNIT_CASE(sgxfive_debug_test_pb_small),
	KUNIT_CASE(sgxfive_debug_test_pb_pages),
	KUNIT_CASE(sgxfive_debug_test_pages_copy),
	KUNIT_CASE(sgxfive_debug_test_render_newest),
	KUNIT_CASE(sgxfive_debug_test_vm_low),
	KUNIT_CASE(sgxfive_debug_test_ctx_flags),
	KUNIT_CASE(sgxfive_debug_test_cleanup),
	KUNIT_CASE(sgxfive_debug_test_timer_clock),
	KUNIT_CASE(sgxfive_debug_test_hold_recovery),
	{}
};

static struct kunit_suite sgxfive_debug_test_suite = {
	.name = "gma500_sgxfive_debug",
	.test_cases = sgxfive_debug_test_cases,
};

kunit_test_suite(sgxfive_debug_test_suite);
