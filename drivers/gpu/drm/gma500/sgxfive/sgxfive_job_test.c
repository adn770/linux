// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the pure parts of sgxfive contexts and jobs: the arguments
 * of a new context.
 * The UAPI transfer register block turned into the firmware's transfer
 * command, and its checks; the order of job numbers. SUBMIT_JOBS' copy of a
 * user array at any stride, the sync op encoding and which waits an earlier
 * job of the same call satisfies.
 * The client CCB a context is rebuilt with after a recovery reset, whom a
 * hang blames, the errors a recovery fails jobs with, the fault budget.
 */
#include <kunit/test.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

static u32 dw(const u8 *p, unsigned int off)
{
	return p[off] | p[off + 1] << 8 | p[off + 2] << 16 | (u32)p[off + 3] << 24;
}

/*
 * A transfer's register block in the REV1 layout: its addresses in the user
 * heaps, PDS_EXEC_BASE at the PDS_PIXEL heap base; the other registers hold
 * marker values.
 */
static void fill_regs(struct drm_sgxfive_transfer_cmd *c)
{
	memset(c, 0, sizeof(*c));
	c->isp_bgobjtag = 0x3000;
	c->isp_bgobj = 0x101;
	c->isp_bgobjdepth = 0x3f800000;		/* 1.0 */
	c->isp_render = 0x102;
	c->isp_ipfmisc = 0x103;
	c->bif_3d_req_base = 0x3e100000;
	c->aa_mode_3d = 0x104;
	c->isp_multisamplectl = 0x105;
	c->event_pixel_pds_exec = 0x2e000040;
	c->event_pixel_pds_data = 0x106;
	c->event_pixel_pds_info = 0x107;
	c->isp_render_box2 = 0x00010001;
	c->pixelbe = 0x108;
	c->pds_exec_base = 0x2e000000;
}

static void sgxfive_job_test_build(struct kunit *test)
{
	struct drm_sgxfive_transfer_cmd c;
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int off, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	fill_regs(&c);
	c.load_flags = DRM_SGXFIVE_TRANSFER_LOAD_FIR;
	c.csc0_coeff[0] = 0x11;			/* lands at 0x44 */
	c.fir_hfilter[8] = 0x22;		/* lands at 0xa0 */
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgxfive_job_build(cmd, &c, false, 0x32400200, 7),
			0x1c0u);

	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0x3000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x018), 0x3e100000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x024), 0x2e000040u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x03c), 0x2e000000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x040), 0u);	/* double pixel partitions */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x044), 0x11u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x0a0), 0x22u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_SIZE), 0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_STATUS), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 7u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x188), 0u);	/* load the YUV coefficients */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x18c), 0u);	/* ...the luma-key ones */
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x190), 1u);	/* ...the FIR ones */
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_PIXEL_PARTITIONS), 0x07c00000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FENCE), 7u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_UPDATES), 0u);
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4)
		nonzero += dw(cmd, off) != 0;
	/* 14 register words + 2 coefficients + size, status x3, FIR, parts, id */
	KUNIT_EXPECT_EQ(test, nonzero, 23u);
}

static void sgxfive_job_test_dummy(struct kunit *test)
{
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_EXPECT_EQ(test, sgxfive_job_build(cmd, NULL, true, 0x32400200, 3),
			0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), (u32)SGX_XFER_FLAG_DUMMY);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 3u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0x000), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_PIXEL_PARTITIONS), 0u);
}

/* Addresses the pixel pipe fetches or executes must be in a user heap. */
static void sgxfive_job_test_check(struct kunit *test)
{
	struct drm_sgxfive_transfer_cmd c;

	fill_regs(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), 0);

	c.pds_exec_base = 0x30000000;		/* the reserved firmware code */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	/* Render jobs rely on PDS_EXEC_BASE = the PDS_PIXEL heap base. */
	c.pds_exec_base = 0x2e001000;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.event_pixel_pds_exec = 0x32000000;	/* the reserved firmware data */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.bif_3d_req_base = 0x3e180000;		/* not 1 MiB aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.isp_rgn_base = 0x10000000;		/* outside every heap */
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.double_pixel_partitions = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
	fill_regs(&c);
	c.load_flags = 1u << 3;
	KUNIT_EXPECT_EQ(test, sgxfive_job_check(&c), -EINVAL);
}

/*
 * CREATE_CONTEXT's arguments: a known type, no flags, the one priority, zero
 * padding; a transfer context leaves both target sizes at 0.
 */
static void sgxfive_job_test_ctx_args(struct kunit *test)
{
	const struct drm_sgxfive_create_context_args xfer = {
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER, .vm_context_handle = 1,
	};
	struct drm_sgxfive_create_context_args a;

	KUNIT_EXPECT_EQ(test, sizeof(a), (size_t)32);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&xfer), 0);
	a = xfer;
	a.max_width = 64;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.max_height = 64;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a._padding_1c = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.type = 2;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.flags = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.priority = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	/* Render contexts are not supported yet. */
	a = xfer;
	a.type = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
}

/* A fence is done once the context's fence word has reached its number. */
static void sgxfive_job_test_seqno(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, sgxfive_seqno_passed(5, 5));
	KUNIT_EXPECT_TRUE(test, sgxfive_seqno_passed(6, 5));
	KUNIT_EXPECT_FALSE(test, sgxfive_seqno_passed(4, 5));
	KUNIT_EXPECT_FALSE(test, sgxfive_seqno_passed(0, 1));	/* nothing yet */
	/* Across the 32-bit wrap. */
	KUNIT_EXPECT_TRUE(test, sgxfive_seqno_passed(2, 0xfffffffe));
	KUNIT_EXPECT_FALSE(test, sgxfive_seqno_passed(0xfffffffe, 2));
}

#define SYNC_SIGNAL	DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL
#define SYNC_TIMELINE	DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ

/* Copy n bytes into a fresh user page at its start; returns the page. */
static u64 user_bytes(struct kunit *test, const void *src, size_t n)
{
	unsigned long m = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					MAP_ANONYMOUS | MAP_PRIVATE, 0);
	u64 u = m;

	if (IS_ERR_VALUE(m))
		kunit_skip(test, "no user memory");
	KUNIT_ASSERT_EQ(test, copy_to_user(u64_to_user_ptr(u), src, n), 0);
	return u;
}

/* Two sync ops copied at a stride, the tail of each element as given. */
static int copy_ops(struct kunit *test, struct drm_sgxfive_sync_op *out, u32 stride,
		    u8 tail)
{
	const struct drm_sgxfive_sync_op ops[2] = {
		{ 1, SYNC_SIGNAL | SYNC_TIMELINE, 5 }, { 2, 0, 0 },
	};
	u8 *buf = kunit_kzalloc(test, 2 * stride, GFP_KERNEL);
	struct drm_sgxfive_obj_array a = { stride, 2, 0 };
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	for (i = 0; i < 2; i++) {
		memcpy(buf + i * stride, &ops[i], min_t(u32, stride, sizeof(ops[i])));
		if (stride > sizeof(ops[i]))
			buf[i * stride + stride - 1] = tail;
	}
	a.array = user_bytes(test, buf, 2 * stride);
	memset(out, 0xff, 2 * sizeof(*out));
	return sgxfive_copy_array(out, sizeof(*out), &a, SGXFIVE_MAX_SYNC_OPS);
}

/*
 * A user array: elements of the size the kernel knows; shorter, zero-extended;
 * longer, with a zero tail; longer with a non-zero tail, refused.
 */
static void sgxfive_job_test_copy_array(struct kunit *test)
{
	const u32 size = sizeof(struct drm_sgxfive_sync_op);
	struct drm_sgxfive_sync_op out[2];

	KUNIT_ASSERT_EQ(test, copy_ops(test, out, size, 0), 0);
	KUNIT_EXPECT_EQ(test, out[0].handle, 1u);
	KUNIT_EXPECT_EQ(test, out[0].flags, (u32)(SYNC_SIGNAL | SYNC_TIMELINE));
	KUNIT_EXPECT_EQ(test, out[0].value, 5u);
	KUNIT_EXPECT_EQ(test, out[1].handle, 2u);

	/* Handle and flags only: the value is zero, not left over. */
	KUNIT_ASSERT_EQ(test, copy_ops(test, out, 8, 0), 0);
	KUNIT_EXPECT_EQ(test, out[0].flags, (u32)(SYNC_SIGNAL | SYNC_TIMELINE));
	KUNIT_EXPECT_EQ(test, out[0].value, 0u);
	KUNIT_EXPECT_EQ(test, out[1].handle, 2u);
	KUNIT_EXPECT_EQ(test, out[1].value, 0u);

	KUNIT_ASSERT_EQ(test, copy_ops(test, out, size + 8, 0), 0);
	KUNIT_EXPECT_EQ(test, out[0].value, 5u);
	KUNIT_EXPECT_EQ(test, out[1].handle, 2u);
	KUNIT_EXPECT_EQ(test, copy_ops(test, out, size + 8, 1), -EINVAL);

	/* Up to the cap, with a zero tail. */
	KUNIT_ASSERT_EQ(test, copy_ops(test, out, DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE, 0), 0);
	KUNIT_EXPECT_EQ(test, out[1].handle, 2u);
	KUNIT_EXPECT_EQ(test, copy_ops(test, out, DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE, 1),
			-EINVAL);
}

/* Strides and counts refused before any element is read. */
static void sgxfive_job_test_copy_array_limits(struct kunit *test)
{
	static const u32 bad[] = { 0, DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE + 1, U32_MAX };
	const u32 size = sizeof(struct drm_sgxfive_sync_op);
	struct drm_sgxfive_sync_op out[SGXFIVE_MAX_SYNC_OPS] = {};
	struct drm_sgxfive_obj_array a = { size, 1, 0 };
	u32 i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		a.stride = bad[i];
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_copy_array(out, size, &a, 1), -EINVAL,
				    "stride %u", bad[i]);
	}
	a.stride = size;
	a.count = SGXFIVE_MAX_SYNC_OPS + 1;
	KUNIT_EXPECT_EQ(test, sgxfive_copy_array(out, size, &a, SGXFIVE_MAX_SYNC_OPS),
			-EINVAL);
	/* Nothing to copy needs no stride. */
	a.count = 0;
	a.stride = U32_MAX;
	KUNIT_EXPECT_EQ(test, sgxfive_copy_array(out, size, &a, 1), 0);
	/* An unmapped array faults. */
	user_bytes(test, out, size);
	a.count = 1;
	a.stride = size;
	KUNIT_EXPECT_EQ(test, sgxfive_copy_array(out, size, &a, 1), -EFAULT);
}

/* A sync op: known flags and handle types; a point only on a timeline. */
static void sgxfive_job_test_sync_op(struct kunit *test)
{
	static const struct {
		struct drm_sgxfive_sync_op op;
		int ret;
	} cases[] = {
		{ { 1, 0, 0 }, 0 },
		{ { 1, SYNC_SIGNAL, 0 }, 0 },
		{ { 1, SYNC_TIMELINE, 7 }, 0 },
		{ { 1, SYNC_TIMELINE | SYNC_SIGNAL, 7 }, 0 },
		{ { 1, SYNC_TIMELINE, 0 }, 0 },
		{ { 1, 0, 7 }, -EINVAL },		/* a point on a binary syncobj */
		{ { 1, 2, 0 }, -EINVAL },		/* an unknown handle type */
		{ { 1, 0xf, 0 }, -EINVAL },
		{ { 1, BIT(4), 0 }, -EINVAL },		/* a reserved flag */
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_sync_op_check(&cases[i].op), cases[i].ret,
				    "case %u", i);
}

/*
 * The waits an earlier job's signals satisfy: the same binary syncobj, or the
 * same timeline at the wait's point or later; not another handle, not a wait
 * of the earlier job, not a timeline point before the wait's.
 */
static void sgxfive_job_test_sync_signals(struct kunit *test)
{
	const struct drm_sgxfive_sync_op earlier[] = {
		{ 1, SYNC_SIGNAL, 0 },
		{ 2, SYNC_TIMELINE | SYNC_SIGNAL, 10 },
		{ 3, 0, 0 },
	};
	static const struct {
		struct drm_sgxfive_sync_op wait;
		bool signalled;
	} cases[] = {
		{ { 1, 0, 0 }, true },
		{ { 2, SYNC_TIMELINE, 10 }, true },
		{ { 2, SYNC_TIMELINE, 3 }, true },
		{ { 2, SYNC_TIMELINE, 11 }, false },
		{ { 2, 0, 0 }, false },			/* the timeline as a binary one */
		{ { 1, SYNC_TIMELINE, 1 }, false },
		{ { 3, 0, 0 }, false },			/* only waited on */
		{ { 4, 0, 0 }, false },
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_sync_signals(earlier, ARRAY_SIZE(earlier),
							       &cases[i].wait),
				    cases[i].signalled, "case %u", i);
	KUNIT_EXPECT_FALSE(test, sgxfive_sync_signals(earlier, 0, &cases[0].wait));
}

/* A context with its memory in a plain buffer, as far as fences need one. */
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
 * After a reset the client CCB holds the context's unfinished commands from
 * offset 0, in order, and the fence word the last job it finished.
 */
static void sgxfive_job_test_rewind(struct kunit *test)
{
	struct sgxfive_ctx *ctx = fake_ctx(test);
	u8 *ccb = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF);
	u32 *ctl = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CTL_OFF);
	u32 *done = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF);
	struct dma_fence *f[3];
	u8 *cmd[3];
	u64 order;
	int i;

	for (i = 0; i < 3; i++) {
		struct sgxfive_fence *hw = sgxfive_fence_alloc();

		KUNIT_ASSERT_NOT_NULL(test, hw);
		cmd[i] = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, cmd[i]);
		memset(cmd[i], 0x11 * (i + 1), SGX_XFER_CMD_SIZE);
		memcpy(hw->cmd, cmd[i], SGX_XFER_CMD_SIZE);
		f[i] = sgxfive_fence_init(hw, ctx, i + 1, 10 + i, 0x100);
	}
	KUNIT_EXPECT_TRUE(test, sgxfive_ctx_oldest(ctx, &order));
	KUNIT_EXPECT_EQ(test, order, 10);

	/* The first job completes. */
	*done = 1;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_TRUE(test, dma_fence_is_signaled(f[0]));
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	KUNIT_EXPECT_TRUE(test, sgxfive_ctx_oldest(ctx, &order));
	KUNIT_EXPECT_EQ(test, order, 11);

	/* Stale state from before the reset. */
	ctx->woff = 0x300;
	ctl[0] = 0x300;
	ctl[1] = 0x200;
	memset(ccb, 0xee, 0x400);
	*done = 0xdead;

	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(ctx), 2);
	KUNIT_EXPECT_EQ(test, ctx->woff, 0x200);
	KUNIT_EXPECT_EQ(test, ctl[0], 0x200);
	KUNIT_EXPECT_EQ(test, ctl[1], 0);
	KUNIT_EXPECT_EQ(test, *done, 1);
	KUNIT_EXPECT_EQ(test, memcmp(ccb, cmd[1], 0x100), 0);
	KUNIT_EXPECT_EQ(test, memcmp(ccb + 0x100, cmd[2], SGX_XFER_CMD_SIZE), 0);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[2]));

	/* A guilty context's jobs fail instead. */
	sgxfive_fence_fail_all(ctx, -EIO);
	for (i = 1; i < 3; i++) {
		KUNIT_EXPECT_TRUE(test, dma_fence_is_signaled(f[i]));
		KUNIT_EXPECT_EQ(test, f[i]->error, -EIO);
	}
	KUNIT_EXPECT_FALSE(test, sgxfive_ctx_oldest(ctx, &order));
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(ctx), 0);
	KUNIT_EXPECT_EQ(test, ctx->woff, 0);
	for (i = 0; i < 3; i++)
		dma_fence_put(f[i]);
}

/* A job of order on ctx, unfinished, its fence returned. */
static struct dma_fence *fake_job(struct kunit *test, struct sgxfive_ctx *ctx, u32 seqno,
				  u64 order)
{
	struct sgxfive_fence *hw = sgxfive_fence_alloc();

	if (!hw)
		return NULL;
	return sgxfive_fence_init(hw, ctx, seqno, order, SGX_XFER_CMD_SIZE);
}

/*
 * Whom a hang blames. A BIF fault names a page directory: the context to blame
 * is the one with the oldest unfinished job among the contexts on that
 * directory (one VM). Without a directory (the EDM's own list, a directory no
 * context uses, or one whose contexts have nothing unfinished) it is the owner
 * of the oldest unfinished job of all, as before.
 */
static void sgxfive_job_test_suspect(struct kunit *test)
{
	struct sgxfive_ctx *a = fake_ctx(test), *b = fake_ctx(test), *c = fake_ctx(test);
	struct dma_fence *f[4];
	LIST_HEAD(ctxs);
	u64 order;
	int i;

	a->pd_phys = 0x01234000;	/* a render context and ... */
	c->pd_phys = 0x01234000;	/* ... a transfer context in one VM */
	b->pd_phys = 0x02345000;	/* a transfer context in another */
	list_add_tail(&a->link, &ctxs);
	list_add_tail(&b->link, &ctxs);
	list_add_tail(&c->link, &ctxs);

	order = 1;
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_suspect(&ctxs, 0, &order));
	KUNIT_EXPECT_EQ(test, order, U64_MAX);
	f[0] = fake_job(test, a, 1, 10);	/* the render, queued first */
	f[1] = fake_job(test, b, 1, 11);	/* the faulting transfer */
	f[2] = fake_job(test, c, 1, 12);
	f[3] = fake_job(test, a, 2, 13);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_NOT_NULL(test, f[i]);

	/* By age: the render. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);
	/* By the fault's directory: the transfer. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x02345000, &order), b);
	KUNIT_EXPECT_EQ(test, order, 11);
	/* Two contexts on the directory: the older job of the two. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x01234000, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);
	/* A directory no context uses: by age. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x03456000, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);

	/* The job a suspect order names, while it is its context's oldest. */
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_of_job(&ctxs, 11), b);
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_of_job(&ctxs, 10), a);
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_of_job(&ctxs, 12), c);
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, 13));	/* a has 10 first */
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, 14));
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, U64_MAX));

	/* Once the transfer is failed (recovered), its directory has nothing left. */
	sgxfive_fence_fail_all(b, -EIO);
	KUNIT_EXPECT_NULL(test, sgxfive_ctx_of_job(&ctxs, 11));
	KUNIT_EXPECT_PTR_EQ(test, sgxfive_ctx_suspect(&ctxs, 0x02345000, &order), a);
	KUNIT_EXPECT_EQ(test, order, 10);
	sgxfive_fence_fail_all(a, -EIO);
	sgxfive_fence_fail_all(c, -EIO);
	for (i = 0; i < 4; i++)
		dma_fence_put(f[i]);
}

/* The error a hang's guilty job fails with names the detector that found it. */
static void sgxfive_job_test_hang_error(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgxfive_hang_error(SGXFIVE_HANG_FAULT), -EIO);
	KUNIT_EXPECT_EQ(test, sgxfive_hang_error(SGXFIVE_HANG_TIMEOUT), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, sgxfive_hang_error(SGXFIVE_HANG_MANUAL), -EIO);
}

/*
 * A recovery fails the guilty context's oldest unfinished job, the one that
 * faulted or hung, with the hang's error and its later jobs with -ECANCELED.
 * Another context's unfinished jobs are rewound, re-run and signal without
 * an error.
 */
static void sgxfive_job_test_guilty(struct kunit *test)
{
	struct sgxfive_ctx *a = fake_ctx(test), *b = fake_ctx(test);
	u32 *done = sgxfive_ctx_cpu(b, SGXFIVE_CTX_FENCE_OFF);
	struct dma_fence *f[5];
	u64 order;
	int i;

	f[0] = fake_job(test, a, 1, 10);	/* faults */
	f[1] = fake_job(test, b, 1, 11);	/* innocent, queued behind it */
	f[2] = fake_job(test, a, 2, 12);
	f[3] = fake_job(test, a, 3, 13);
	f[4] = fake_job(test, b, 2, 14);
	for (i = 0; i < 5; i++)
		KUNIT_ASSERT_NOT_NULL(test, f[i]);

	sgxfive_fence_fail_guilty(a, sgxfive_hang_error(SGXFIVE_HANG_FAULT));
	KUNIT_EXPECT_TRUE(test, dma_fence_is_signaled(f[0]));
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -EIO);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[2]), -ECANCELED);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[3]), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, sgxfive_ctx_oldest(a, &order));
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[1]), 0);	/* still pending */
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[4]), 0);

	/* The innocent context re-runs both jobs after the reset. */
	*done = 0xdead;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(b), 2);
	KUNIT_EXPECT_EQ(test, *done, 0);
	*done = 2;
	sgxfive_fence_signal(b);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[1]), 1);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[4]), 1);

	/* A hang found by the job timeout, on a context with one job. */
	dma_fence_put(f[0]);
	f[0] = fake_job(test, a, 4, 15);
	KUNIT_ASSERT_NOT_NULL(test, f[0]);
	sgxfive_fence_fail_guilty(a, sgxfive_hang_error(SGXFIVE_HANG_TIMEOUT));
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -ETIMEDOUT);
	/* Nothing unfinished: nothing to fail. */
	sgxfive_fence_fail_guilty(a, -EIO);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -ETIMEDOUT);
	for (i = 0; i < 5; i++)
		dma_fence_put(f[i]);
}

/* Three faults within a minute ban the file; older faults do not count. */
static void sgxfive_job_test_budget(struct kunit *test)
{
	struct sgxfive_client c = { }, w = { };
	unsigned long t = 1000, u = ULONG_MAX - HZ;

	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&c, t));
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&c, t + 61 * HZ));
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&c, t + 62 * HZ));
	KUNIT_EXPECT_FALSE(test, c.banned);
	KUNIT_EXPECT_TRUE(test, sgxfive_client_fault(&c, t + 63 * HZ));
	KUNIT_EXPECT_TRUE(test, c.banned);

	/* Across the jiffies wrap. */
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&w, u));
	KUNIT_EXPECT_FALSE(test, sgxfive_client_fault(&w, u + HZ));
	KUNIT_EXPECT_TRUE(test, sgxfive_client_fault(&w, u + 2 * HZ));
}

static struct kunit_case sgxfive_job_test_cases[] = {
	KUNIT_CASE(sgxfive_job_test_build),
	KUNIT_CASE(sgxfive_job_test_dummy),
	KUNIT_CASE(sgxfive_job_test_check),
	KUNIT_CASE(sgxfive_job_test_ctx_args),
	KUNIT_CASE(sgxfive_job_test_seqno),
	KUNIT_CASE(sgxfive_job_test_copy_array),
	KUNIT_CASE(sgxfive_job_test_copy_array_limits),
	KUNIT_CASE(sgxfive_job_test_sync_op),
	KUNIT_CASE(sgxfive_job_test_sync_signals),
	KUNIT_CASE(sgxfive_job_test_rewind),
	KUNIT_CASE(sgxfive_job_test_suspect),
	KUNIT_CASE(sgxfive_job_test_hang_error),
	KUNIT_CASE(sgxfive_job_test_guilty),
	KUNIT_CASE(sgxfive_job_test_budget),
	{}
};

static struct kunit_suite sgxfive_job_test_suite = {
	.name = "gma500_sgxfive_job",
	.test_cases = sgxfive_job_test_cases,
};

kunit_test_suite(sgxfive_job_test_suite);
