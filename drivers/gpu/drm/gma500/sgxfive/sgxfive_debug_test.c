// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for what the debug group's files change in the driver; built
 * only with CONFIG_DRM_GMA500_SGXFIVE_DEBUG, like the files. The parameter
 * buffer of new render contexts.
 * The copies of a render context's pages and the context they come from.
 * The first MiB VM_MAP may take.
 * The extra flags of new render contexts and the trace of their cleanup.
 * The timer clock the boot writes into HostCtl.
 * Recoveries held, the reset that ends a hold, and renders that keep their
 * RT data set. A render's run and the words render_words shows.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/dma-fence.h>
#include <linux/mm.h>

#include <drm/drm_print.h>

#include "sgx_mmu.h"
#include "sgx_render.h"
#include "sgxfive_ctx.h"
#include "sgxfive_kpoke.h"
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

/* What the last put of a context runs, as a mark: the context's release. */
static void donor_release(struct kref *ref)
{
	container_of(ref, struct sgxfive_ctx, ref)->closed = true;
}

/*
 * debugfs render_share_pb: the donor is the newest render context of the
 * sharing context's VM that has its render state (not another VM's, not a
 * transfer context), taken with a reference. The sharer's reference keeps a
 * donor that is closed first: its last put runs only when the sharer's does,
 * and a donor whose release has begun is not taken.
 */
static void sgxfive_debug_test_render_donor(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	void *vm_a = kunit_kzalloc(test, 8, GFP_KERNEL);
	void *vm_b = kunit_kzalloc(test, 8, GFP_KERNEL);
	void *render = kunit_kzalloc(test, 8, GFP_KERNEL);	/* only its address is used */
	struct sgxfive_ctx *c[5], *sharer, *d;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	for (i = 0; i < ARRAY_SIZE(c); i++) {
		c[i] = fake_ctx(test);
		c[i]->sgx = sgx;
		c[i]->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
		c[i]->vm = vm_a;
		c[i]->render = render;
		kref_init(&c[i]->ref);
		list_add_tail(&c[i]->link, &sgx->ctx_list);
	}
	c[2]->type = DRM_SGXFIVE_CTX_TYPE_TRANSFER;
	c[3]->vm = vm_b;
	c[4]->render = NULL;		/* no render state yet */
	sharer = fake_ctx(test);
	sharer->sgx = sgx;
	sharer->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	sharer->vm = vm_a;

	/* Not itself, not another VM's, not a transfer context: the newest of the rest. */
	d = sgxfive_render_donor(sharer);
	KUNIT_ASSERT_PTR_EQ(test, d, c[1]);
	KUNIT_EXPECT_EQ(test, kref_read(&c[1]->ref), 2u);
	KUNIT_EXPECT_EQ(test, kref_read(&c[0]->ref), 1u);

	/* The donor is closed first: its release waits for the sharer's put. */
	KUNIT_EXPECT_FALSE(test, kref_put(&c[1]->ref, donor_release));
	KUNIT_EXPECT_FALSE(test, c[1]->closed);
	KUNIT_EXPECT_EQ(test, kref_read(&c[1]->ref), 1u);
	KUNIT_EXPECT_TRUE(test, kref_put(&d->ref, donor_release));
	KUNIT_EXPECT_TRUE(test, c[1]->closed);

	/* A donor whose release has begun is skipped, the next older one taken. */
	d = sgxfive_render_donor(sharer);
	KUNIT_EXPECT_PTR_EQ(test, d, c[0]);
	KUNIT_EXPECT_EQ(test, kref_read(&c[0]->ref), 2u);
	KUNIT_EXPECT_EQ(test, kref_read(&c[1]->ref), 0u);

	/* None: the other contexts are in no usable state. */
	refcount_set(&c[0]->ref.refcount, 0);
	KUNIT_EXPECT_NULL(test, sgxfive_render_donor(sharer));
	sharer->vm = vm_b;
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_render_donor(sharer), c[3]);
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
 * a reset asked for never is; the first held one is reported and remembered,
 * and the detectors keep quiet until the next reset ends the hold. rtds_keep
 * reads through.
 */
static void sgxfive_debug_test_hold_recovery(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	enum sgxfive_hang why;
	u64 s;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	s = 3;
	why = SGXFIVE_HANG_FAULT;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	why = SGXFIVE_HANG_TIMEOUT;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_held(sgx));

	sgx->hold_recovery = true;
	s = U64_MAX;
	why = SGXFIVE_HANG_MANUAL;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_held(sgx));
	KUNIT_EXPECT_EQ(test, s, U64_MAX);
	KUNIT_EXPECT_EQ(test, why, SGXFIVE_HANG_MANUAL);
	s = 3;
	why = SGXFIVE_HANG_FAULT;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_held(sgx));
	KUNIT_EXPECT_EQ(test, sgx->held_suspect, 3ull);
	KUNIT_EXPECT_EQ(test, sgx->held_why, SGXFIVE_HANG_FAULT);
	/* Later rounds, of either detector, change nothing. */
	s = 4;
	why = SGXFIVE_HANG_TIMEOUT;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, sgx->held_suspect, 3ull);
	KUNIT_EXPECT_EQ(test, sgx->held_why, SGXFIVE_HANG_FAULT);
	KUNIT_EXPECT_TRUE(test, sgxfive_debug_reset(sgx));
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_held(sgx));
	/* Nothing held: nothing is ended. */
	KUNIT_EXPECT_FALSE(test, sgxfive_debug_reset(sgx));

	KUNIT_EXPECT_FALSE(test, sgxfive_rtds_keep(sgx));
	sgx->rtds_keep = true;
	KUNIT_EXPECT_TRUE(test, sgxfive_rtds_keep(sgx));
}

/*
 * The reset a render context's failed cleanup asks for, as sgxfive_recover
 * decides it under the device lock and reports it: reached, blame-free, when
 * nothing is held (the device here has no firmware, so the reset ends in
 * -ENODEV); dropped while a recovery is held, which stays as it was, the held
 * job unblamed, and ends with the reset that ends the hold. The warning it
 * logs is not captured: KUnit has no log to read it from.
 */
static void sgxfive_debug_test_cleanup_reset(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	enum sgxfive_hang why = SGXFIVE_HANG_TIMEOUT;
	bool dropped = true;
	u64 s = 10;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	mutex_init(&sgx->lock);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	KUNIT_EXPECT_EQ(test, __sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_CLEANUP, &dropped),
			-ENODEV);
	KUNIT_EXPECT_FALSE(test, dropped);
	KUNIT_EXPECT_TRUE(test, sgxfive_cleanup_reset(sgx));
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_held(sgx));

	sgx->hold_recovery = true;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, __sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_CLEANUP, &dropped), 0);
	KUNIT_EXPECT_TRUE(test, dropped);
	KUNIT_EXPECT_FALSE(test, sgxfive_cleanup_reset(sgx));
	KUNIT_EXPECT_FALSE(test, sgxfive_cleanup_reset(sgx));
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_held(sgx));
	KUNIT_EXPECT_EQ(test, sgx->held_suspect, 10ull);
	KUNIT_EXPECT_EQ(test, sgx->held_why, SGXFIVE_HANG_TIMEOUT);

	sgxfive_debug_reset(sgx);
	KUNIT_EXPECT_TRUE(test, sgxfive_cleanup_reset(sgx));
	/* Without a caller that wants to know, the same. */
	KUNIT_EXPECT_EQ(test, sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_CLEANUP), -ENODEV);
}

/* A job of order on ctx, unfinished, its fence returned. */
static struct dma_fence *fake_job(struct kunit *test, struct sgxfive_ctx *ctx, u32 seqno,
				  u64 order)
{
	struct sgxfive_fence *hw = sgxfive_fence_alloc();

	KUNIT_ASSERT_NOT_NULL(test, hw);
	return sgxfive_fence_init(hw, ctx, seqno, order, SGX_XFER_CMD_SIZE);
}

/*
 * A job of a closed context stays queued in the firmware while a recovery is
 * held. The reset that ends the hold cancels it, as sgxfive_reset does with
 * what sgxfive_debug_reset reports; a reset with no hold runs it again, so a
 * context that is gone for good costs nothing unless a hold was on.
 */
static void sgxfive_debug_test_held_reset_closed(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	struct sgxfive_ctx *gone = fake_ctx(test), *open = fake_ctx(test);
	struct dma_fence *f[2];
	enum sgxfive_hang why = SGXFIVE_HANG_FAULT;
	unsigned int cancelled = 0;
	u64 s = 20;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	list_add_tail(&gone->link, &sgx->ctx_list);
	list_add_tail(&open->link, &sgx->ctx_list);
	WRITE_ONCE(gone->closed, true);
	f[0] = fake_job(test, gone, 1, 21);
	f[1] = fake_job(test, open, 1, 22);

	/* No hold: the reset ends none, the closed context's job runs again. */
	if (sgxfive_debug_reset(sgx))
		cancelled = sgxfive_fence_cancel_closed(&sgx->ctx_list);
	KUNIT_EXPECT_EQ(test, cancelled, 0u);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[0]));
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(gone), 1);

	/* Held: the reset that ends it cancels the closed context's job only. */
	sgx->hold_recovery = true;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	if (sgxfive_debug_reset(sgx))
		cancelled = sgxfive_fence_cancel_closed(&sgx->ctx_list);
	KUNIT_EXPECT_EQ(test, cancelled, 1u);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(gone), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(open), 1);
	sgxfive_fence_fail_all(open, -EIO);
	dma_fence_put(f[0]);
	dma_fence_put(f[1]);
}

/*
 * A reset asked for while a recovery is held is that recovery: it blames the
 * held job with its detector's error, so the job fails once instead of
 * running again and hanging again. Not once the job has finished, nor a
 * reset that names its own suspect, nor one without a hold. A failed render
 * context cleanup's reset is dropped while a recovery is held, blaming no one.
 */
static void sgxfive_debug_test_held_reset(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	struct sgxfive_ctx *ctx = fake_ctx(test);
	struct dma_fence *f[2];
	enum sgxfive_hang why;
	u64 s;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	list_add_tail(&ctx->link, &sgx->ctx_list);
	f[0] = fake_job(test, ctx, 1, 10);
	f[1] = fake_job(test, ctx, 2, 11);

	/* No hold: the reset blames no one, nor a failed cleanup's. */
	s = U64_MAX;
	why = SGXFIVE_HANG_MANUAL;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, s, U64_MAX);
	KUNIT_EXPECT_EQ(test, why, SGXFIVE_HANG_MANUAL);
	why = SGXFIVE_HANG_CLEANUP;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, s, U64_MAX);

	sgx->hold_recovery = true;
	s = 10;
	why = SGXFIVE_HANG_TIMEOUT;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	s = U64_MAX;
	why = SGXFIVE_HANG_MANUAL;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, s, 10ull);
	KUNIT_EXPECT_EQ(test, why, SGXFIVE_HANG_TIMEOUT);
	/* A reset with its own suspect keeps it. */
	s = 11;
	why = SGXFIVE_HANG_MANUAL;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, s, 11ull);
	KUNIT_EXPECT_EQ(test, why, SGXFIVE_HANG_MANUAL);
	/* A failed cleanup's reset is dropped: the hold stays, no one is blamed. */
	s = U64_MAX;
	why = SGXFIVE_HANG_CLEANUP;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, s, U64_MAX);
	KUNIT_EXPECT_EQ(test, why, SGXFIVE_HANG_CLEANUP);
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_held(sgx));
	KUNIT_EXPECT_EQ(test, sgx->held_suspect, 10ull);
	KUNIT_EXPECT_EQ(test, sgx->held_why, SGXFIVE_HANG_TIMEOUT);
	sgxfive_debug_reset(sgx);

	/* Held on a fault; the job then finishes: the reset blames no one. */
	s = 10;
	why = SGXFIVE_HANG_FAULT;
	KUNIT_EXPECT_TRUE(test, sgxfive_recovery_hold(sgx, &s, &why));
	*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF) = 1;
	sgxfive_fence_signal(ctx);
	s = U64_MAX;
	why = SGXFIVE_HANG_MANUAL;
	KUNIT_EXPECT_FALSE(test, sgxfive_recovery_hold(sgx, &s, &why));
	KUNIT_EXPECT_EQ(test, s, U64_MAX);
	KUNIT_EXPECT_EQ(test, why, SGXFIVE_HANG_MANUAL);

	sgxfive_fence_fail_all(ctx, -EIO);
	dma_fence_put(f[0]);
	dma_fence_put(f[1]);
}

/*
 * A render's run, before its kick: the kernel's words (render details,
 * background object, the RT data set's size words and cleared counters, the
 * pending count, the TA command, the CCB's write offset), then the armed pokes
 * of the context's structures, once, on top of them. With rtds_keep the RT
 * data set stays as the previous job left it, the pending count still counts.
 */
static void sgxfive_debug_test_render_run(struct kunit *test)
{
	static const char *const lines[] = { "kw:1=0x55", "rtds:2=0x77", "rtds:3=0x9" };
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	struct sgxfive_ctx *ctx = fake_ctx(test);
	u32 *bg = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *img = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	u32 *rtds = sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_RTDS_OFF);
	u32 *pending = sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_PENDING_OFF);
	u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	u32 want[SGX_RENDER_RTDS_SIZE / 4], kept[SGX_RENDER_RTDS_SIZE / 4];
	struct sgxfive_kpoke p[ARRAY_SIZE(lines)];
	struct drm_sgxfive_render_cmd c = { };
	struct sgx_render_tadata td;
	struct sgx_render_addrs a;
	unsigned int i;
	u32 size;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_ASSERT_NOT_NULL(test, bg);
	KUNIT_ASSERT_NOT_NULL(test, img);
	spin_lock_init(&sgx->kpoke_lock);
	ctx->sgx = sgx;
	ctx->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	c.mte_screen = (200 - 1) << 16 | (300 - 1);
	c.bgobj_words[0] = 0x101;
	c.bgobj_words[1] = 0x102;
	c.bgobj_words[2] = 0x103;
	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 300, 200), 0);
	sgxfive_render_addrs(&a, 0x32500000, 0x01234000, 0, &td);
	size = sgxfive_render_build(img, &a, &c, 1, true, true);
	sgx_render_rtds(rtds, &a, 0);		/* as at creation */
	memcpy(want, rtds, sizeof(want));
	sgx_render_rtds_resize(want, c.mte_screen);
	sgx_render_rtds_job(want);
	for (i = 0; i < ARRAY_SIZE(lines); i++)
		KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse(lines[i], &p[i]), 0);
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_arm(&sgx->kpokes, p, ARRAY_SIZE(p)), 0);

	sgxfive_render_write(ctx, bg, &c, img, size);
	KUNIT_EXPECT_EQ(test, memcmp(sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_RD_OFF),
				     img + SGXFIVE_RENDER_IMG_RD, SGX_RENDER_RD_SIZE), 0);
	KUNIT_EXPECT_EQ(test, bg[2], 0x103u);
	KUNIT_EXPECT_EQ(test, *(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_BGPRI_OFF), 0x103u);
	KUNIT_EXPECT_EQ(test, memcmp(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF), img,
				     SGX_RENDER_CMD_STRIDE), 0);
	KUNIT_EXPECT_EQ(test, ctx->woff, size);
	KUNIT_EXPECT_EQ(test, ctl[0], size);
	/* The pokes land on what the kernel wrote: its count of 1, its size, its 0. */
	KUNIT_EXPECT_EQ(test, *pending, 0x55u);
	KUNIT_EXPECT_EQ(test, rtds[2], 0x77u);
	KUNIT_EXPECT_EQ(test, rtds[3], 0x9u);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.n, 0u);
	KUNIT_EXPECT_EQ(test, memcmp(&rtds[4], &want[4], sizeof(want) - 16), 0);

	/* Once: the next run has the kernel's words alone. */
	sgxfive_render_write(ctx, bg, &c, img, size);
	KUNIT_EXPECT_EQ(test, *pending, 0x56u);
	KUNIT_EXPECT_EQ(test, memcmp(rtds, want, sizeof(want)), 0);
	KUNIT_EXPECT_EQ(test, ctx->woff, 2 * size);
	KUNIT_EXPECT_EQ(test, ctl[0], 2 * size);

	/* rtds_keep: the size words and the counters as the last job left them. */
	rtds[2] = 0x123;
	rtds[3] = 4;
	rtds[4] = 5;
	memcpy(kept, rtds, sizeof(kept));
	sgx->rtds_keep = true;
	sgxfive_render_write(ctx, bg, &c, img, size);
	KUNIT_EXPECT_EQ(test, memcmp(rtds, kept, sizeof(kept)), 0);
	KUNIT_EXPECT_EQ(test, *pending, 0x57u);
	sgx->rtds_keep = false;
	sgxfive_render_write(ctx, bg, &c, img, size);
	KUNIT_EXPECT_EQ(test, memcmp(rtds, want, sizeof(want)), 0);
}

/*
 * debugfs render_words: every word of the context's HW context, RT data set,
 * PB descriptor, kernel words and render details, in that order, a line each,
 * spelt as the kpoke that would set it.
 */
static void sgxfive_debug_test_render_words(struct kunit *test)
{
	static const u32 spaces[] = {
		SGXFIVE_KPOKE_CTX, SGXFIVE_KPOKE_RTDS, SGXFIVE_KPOKE_PB, SGXFIVE_KPOKE_KW,
		SGXFIVE_KPOKE_RD,
	};
	static const char *const first[] = {
		"ctx:0=0x00000011\n", "\nrtds:0=0x00000000\n", "\npb:0=0x00000000\n",
		"\nkw:0=0x00000000\n", "\nrd:0=0x00000000\n",
	};
	struct sgxfive_ctx *ctx = fake_ctx(test);
	char *buf = kunit_kzalloc(test, SZ_8K, GFP_KERNEL);
	struct drm_print_iterator it = { .data = buf, .remain = SZ_8K - 1 };
	struct drm_printer p = drm_coredump_printer(&it);
	unsigned int i, lines = 0, want = 0;
	const char *at, *prev = buf;
	u32 last = SGX_RENDER_RD_SIZE / 4 - 1;
	char tail[32];

	KUNIT_ASSERT_NOT_NULL(test, buf);
	*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_HW_OFF) = 0x11;
	((u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_RTDS_OFF))[1] = 0x22;
	((u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_PBDESC_OFF))[14] = 0x33;
	*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_PENDING_OFF) = 3;
	((u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_RD_OFF))[last] = 0x44;

	sgxfive_render_words(ctx, &p);
	for (i = 0; i < ARRAY_SIZE(spaces); i++)
		want += sgxfive_kpoke_words(spaces[i]);
	for (at = buf; *at; at++)
		lines += *at == '\n';
	KUNIT_EXPECT_EQ(test, lines, want);
	KUNIT_EXPECT_EQ(test, strncmp(buf, first[0], strlen(first[0])), 0);
	for (i = 1; i < ARRAY_SIZE(first); i++) {
		at = strstr(buf, first[i]);
		KUNIT_EXPECT_TRUE_MSG(test, at && at > prev, "%s", first[i] + 1);
		if (at)
			prev = at;
	}
	KUNIT_EXPECT_NOT_NULL(test, strstr(buf, "\nrtds:1=0x00000022\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(buf, "\npb:14=0x00000033\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(buf, "\nkw:1=0x00000003\n"));
	snprintf(tail, sizeof(tail), "\nrd:%u=0x00000044\n", last);
	KUNIT_EXPECT_EQ(test, strcmp(buf + strlen(buf) - strlen(tail), tail), 0);
}

/*
 * How a render VM's alias treats each firmware buffer: read-only, the TA/3D
 * control block writable unless alias_ta3d_ro, and a buffer whose bit is set
 * in alias_skip left out.
 */
static void sgxfive_debug_test_alias_flags(struct kunit *test)
{
	const u32 skip = BIT(BUF_TA3D_CTL) | BIT(BUF_MISC) | BIT(0);
	unsigned int id;

	for (id = 0; id < SGX_FW_MAX_BUFFERS; id++) {
		const int plain = id == BUF_TA3D_CTL ? 0 : SGX_MMU_RO;
		const bool left_out = skip & BIT(id);

		KUNIT_EXPECT_EQ_MSG(test, sgxfive_alias_flags(id, false, 0), plain, "id %u", id);
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_alias_flags(id, true, 0), (int)SGX_MMU_RO,
				    "id %u", id);
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_alias_flags(id, false, skip),
				    left_out ? -ENOENT : plain, "id %u", id);
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_alias_flags(id, true, skip),
				    left_out ? -ENOENT : (int)SGX_MMU_RO, "id %u", id);
	}
	KUNIT_EXPECT_EQ(test, sgxfive_alias_flags(BUF_CCB, false, ~0u), -ENOENT);
}

static void sgxfive_debug_test_free_page(void *page)
{
	__free_page(page);
}

static void sgxfive_debug_test_destroy_pd(void *pd)
{
	sgx_mmu_pd_destroy(pd);
}

/*
 * The alias of the firmware's buffers in a VM's page directory: all of them
 * without alias_skip, all but the ones it names with it, and the unmap takes
 * every one that was mapped whatever alias_skip says by then. The pages and
 * the directory go when the test ends, a failed assertion included.
 */
static void sgxfive_debug_test_alias_skip(struct kunit *test)
{
	static const unsigned int ids[] = { BUF_CCB, BUF_CCB_CTL, BUF_KICKER, BUF_TA3D_CTL,
					    BUF_HOST_CTL, BUF_MISC };
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	struct sgx_mmu_pd *pd = sgx_mmu_pd_create();
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_ASSERT_NOT_NULL(test, pd);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sgxfive_debug_test_destroy_pd, pd),
			0);
	mutex_init(&sgx->lock);
	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		struct sgxfive_buf *b = &sgx->buf[ids[i]];

		b->pages = kunit_kcalloc(test, 1, sizeof(*b->pages), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, b->pages);
		b->pages[0] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, b->pages[0]);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sgxfive_debug_test_free_page,
								b->pages[0]), 0);
		b->npages = 1;
		b->addr = 0x30000000 + 0x1000 * ids[i];
		b->pd = pd;
	}

	KUNIT_ASSERT_EQ(test, sgxfive_alias_firmware(sgx, pd, true), 0);
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		KUNIT_EXPECT_NE_MSG(test, sgx_mmu_lookup(pd, sgx->buf[ids[i]].addr), 0u,
				    "id %u", ids[i]);
	KUNIT_ASSERT_EQ(test, sgxfive_alias_firmware(sgx, pd, false), 0);
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		KUNIT_EXPECT_EQ(test, sgx_mmu_lookup(pd, sgx->buf[ids[i]].addr), 0u);

	sgx->alias_skip = BIT(BUF_TA3D_CTL) | BIT(BUF_KICKER);
	KUNIT_ASSERT_EQ(test, sgxfive_alias_firmware(sgx, pd, true), 0);
	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		const bool left_out = sgx->alias_skip & BIT(ids[i]);

		KUNIT_EXPECT_EQ_MSG(test, sgx_mmu_lookup(pd, sgx->buf[ids[i]].addr) != 0, !left_out,
				    "id %u", ids[i]);
	}
	sgx->alias_skip = 0;			/* changed since the map: all the same unmapped */
	KUNIT_ASSERT_EQ(test, sgxfive_alias_firmware(sgx, pd, false), 0);
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		KUNIT_EXPECT_EQ(test, sgx_mmu_lookup(pd, sgx->buf[ids[i]].addr), 0u);

	sgx->alias_skip = ~0u;			/* all left out */
	KUNIT_ASSERT_EQ(test, sgxfive_alias_firmware(sgx, pd, true), 0);
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		KUNIT_EXPECT_EQ(test, sgx_mmu_lookup(pd, sgx->buf[ids[i]].addr), 0u);
}

/*
 * debugfs alias_skip takes the ids that exist (0-15) and refuses a bit past
 * them, leaving the mask as it was.
 */
static void sgxfive_debug_test_alias_skip_set(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, 0x28), 0);
	KUNIT_EXPECT_EQ(test, sgx->alias_skip, 0x28u);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, 0xffff), 0);
	KUNIT_EXPECT_EQ(test, sgx->alias_skip, 0xffffu);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, BIT(16)), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, 0x80000001), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, BIT_ULL(32)), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, ~0ull), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx->alias_skip, 0xffffu);
	KUNIT_EXPECT_EQ(test, sgxfive_alias_skip_set(sgx, 0), 0);
	KUNIT_EXPECT_EQ(test, sgx->alias_skip, 0u);
}

/*
 * A recovery reset (and a boot) leave alias_skip and alias_ta3d_ro as they
 * were: they are modes that stay until written, not one-shot controls.
 */
static void sgxfive_debug_test_alias_skip_reset(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->kpoke_lock);
	sgxfive_hostirq_init(&sgx->hostirq);
	sgx->alias_skip = 0x28;
	sgx->alias_ta3d_ro = true;
	sgx->alias_ctx_rw = true;
	KUNIT_EXPECT_FALSE(test, sgxfive_debug_reset(sgx));
	KUNIT_EXPECT_EQ(test, sgx->alias_skip, 0x28u);
	KUNIT_EXPECT_TRUE(test, sgx->alias_ta3d_ro);
	KUNIT_EXPECT_FALSE(test, sgx->alias_ctx_rw);	/* the one-shot is dropped */
}

static struct kunit_case sgxfive_debug_test_cases[] = {
	KUNIT_CASE(sgxfive_debug_test_pb_small),
	KUNIT_CASE(sgxfive_debug_test_pb_pages),
	KUNIT_CASE(sgxfive_debug_test_pages_copy),
	KUNIT_CASE(sgxfive_debug_test_render_newest),
	KUNIT_CASE(sgxfive_debug_test_render_donor),
	KUNIT_CASE(sgxfive_debug_test_vm_low),
	KUNIT_CASE(sgxfive_debug_test_ctx_flags),
	KUNIT_CASE(sgxfive_debug_test_cleanup),
	KUNIT_CASE(sgxfive_debug_test_timer_clock),
	KUNIT_CASE(sgxfive_debug_test_hold_recovery),
	KUNIT_CASE(sgxfive_debug_test_held_reset),
	KUNIT_CASE(sgxfive_debug_test_held_reset_closed),
	KUNIT_CASE(sgxfive_debug_test_cleanup_reset),
	KUNIT_CASE(sgxfive_debug_test_render_run),
	KUNIT_CASE(sgxfive_debug_test_render_words),
	KUNIT_CASE(sgxfive_debug_test_alias_flags),
	KUNIT_CASE(sgxfive_debug_test_alias_skip),
	KUNIT_CASE(sgxfive_debug_test_alias_skip_set),
	KUNIT_CASE(sgxfive_debug_test_alias_skip_reset),
	{}
};

static struct kunit_suite sgxfive_debug_test_suite = {
	.name = "gma500_sgxfive_debug",
	.test_cases = sgxfive_debug_test_cases,
};

kunit_test_suite(sgxfive_debug_test_suite);
