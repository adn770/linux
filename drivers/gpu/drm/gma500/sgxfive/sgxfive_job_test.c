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
 * A render command turned into a TA command and render details, its checks,
 * a render context's addresses, the scene a render job kicks, and a render
 * job's error when its scene overflowed the parameter buffer.
 */
#include <kunit/test.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "sgx_render.h"
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
 * A render command for a 64 x 64 target: its control stream and pixel-event
 * program in the user heaps, the tiling words of that size (2 x 4 tiles, two
 * macrotiles of one 4 x 4-tile block each way), background bits 8 and 9; the
 * other registers hold marker values.
 */
static void test_cmd(struct drm_sgxfive_render_cmd *c)
{
	memset(c, 0, sizeof(*c));
	c->vdm_ctrl_stream_base = 0x3e001000;
	c->te_aa = 0x201;
	c->te_mtile1 = 1 << 24 | 4 << 12 | 4;
	c->te_mtile2 = 1 << 24 | 4 << 12 | 4;
	c->te_screen = (4 - 1) << 12 | (2 - 1);
	c->te_mtile_stride = 0x10;
	c->mte_ctrl = 0x202;
	c->mte_wcompare = 0x203;
	c->mte_wclamp = 0x204;
	c->mte_screen = (64 - 1) << 16 | (64 - 1);
	c->mte_multisamplectl = 0x205;
	c->pixelbe = 0x206;
	c->isp_ipfmisc = 0x207;
	c->aa_mode_3d = 0x208;
	c->isp_bgobjdepth = 0x3f800000;		/* 1.0 */
	c->isp_bgobj = 0x300;
	c->isp_multisamplectl = 0x209;
	c->tsp_config = 0x20a;
	c->event_pixel_pds_exec = 0x2e001080;
	c->event_pixel_pds_data = 0x20b;
	c->event_pixel_pds_info = 0x20c;
	c->isp_fpuctrl = 0x20d;
	c->isp_depthsort = 0x20e;
	c->bgobj_words[0] = 0x20f;
	c->bgobj_words[1] = 0x210;
	c->bgobj_words[2] = 0x211;
}

/*
 * CREATE_CONTEXT's arguments: a known type, no flags, the one priority, zero
 * padding; a render context states its largest target, 1 to 4096 each way
 * (0 is no size), a transfer context leaves both at 0.
 */
static void sgxfive_job_test_ctx_args(struct kunit *test)
{
	const struct drm_sgxfive_create_context_args xfer = {
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER, .vm_context_handle = 1,
	};
	const struct drm_sgxfive_create_context_args render = {
		.type = DRM_SGXFIVE_CTX_TYPE_RENDER, .vm_context_handle = 1,
		.max_width = 1024, .max_height = 600,
	};
	struct drm_sgxfive_create_context_args a;

	KUNIT_EXPECT_EQ(test, sizeof(a), (size_t)32);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&xfer), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&render), 0);
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
	/* Render: a struct without the sizes reads as 0 x 0, which is refused. */
	a = render;
	a.max_width = 0;
	a.max_height = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_width = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_height = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_width = 4097;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_height = 4097;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = render;
	a.max_width = 4096;
	a.max_height = 4096;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), 0);
	a.max_width = 1;
	a.max_height = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), 0);
	a._padding_1c = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
}

/*
 * The checks on a render command: the addresses the TA and the pixel pipe
 * fetch from or run code at must be in the client's heaps (render VMs hold
 * the firmware alias), aligned; the render target fits the kernel's buffers.
 */
static void sgxfive_job_test_render_check(struct kunit *test)
{
	struct drm_sgxfive_render_cmd c;

	test_cmd(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), 0);
	c.vdm_ctrl_stream_base = 0x32000000;	/* the reserved firmware data */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.vdm_ctrl_stream_base = 0x10001000;	/* a render context's PB */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.vdm_ctrl_stream_base = 0x3e001002;	/* not 4-byte aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.vdm_ctrl_stream_base = 0;
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	test_cmd(&c);
	c.event_pixel_pds_exec = 0x30000000;	/* the reserved firmware code */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	c.event_pixel_pds_exec = 0x2e001088;	/* not 16-byte aligned */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	test_cmd(&c);
	c.mte_screen = 0x01ff01ff;		/* 512 x 512 */
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
	/* The context's maximum: 64 x 64 fits 64 x 64, not 63 or 32 across. */
	test_cmd(&c);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 64, 64), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 63, 64), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 64, 32), -EINVAL);
	test_cmd(&c);
	c.isp_bgobj = 0x400;
	KUNIT_EXPECT_EQ(test, sgxfive_render_check(&c, 4096, 4096), -EINVAL);
}

/*
 * A render context's addresses: its firmware structures in its kernel memory
 * (struct sgxfive_ctx's page after the TA CCB), its slots in the reserved
 * PARAMETERS and TA_DATA heaps.
 */
static void sgxfive_job_test_render_addrs(struct kunit *test)
{
	struct sgx_render_addrs a;
	struct sgx_render_tadata td;
	const u32 m = 0x32500000, pg = m + SGXFIVE_CTX_PAGE_OFF;

	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 1024, 600, 1), 0);
	sgxfive_render_addrs(&a, m, 0x01234000, 3, &td);
	KUNIT_EXPECT_EQ(test, a.pd_phys, 0x01234000u);
	KUNIT_EXPECT_EQ(test, a.param_base, 0x10000000u + 3 * SZ_8M);
	KUNIT_EXPECT_EQ(test, a.tadata, 0x20c00000u);	/* slot 3 of 4 MiB */
	/* The TA data of the context's maximum, inside its slot. */
	KUNIT_EXPECT_EQ(test, a.td.rgn, td.rgn);
	KUNIT_EXPECT_EQ(test, a.td.zls, td.zls);
	KUNIT_EXPECT_EQ(test, a.td.size, 0x11000u);
	KUNIT_EXPECT_LE(test, SGX_RENDER_TD_MAX_SIZE, SGXFIVE_RENDER_TD_SLOT);
	KUNIT_EXPECT_EQ(test, a.ccb, m + SGXFIVE_CTX_CCB_OFF);
	KUNIT_EXPECT_EQ(test, a.ctx, m + SGXFIVE_CTX_HW_OFF);
	KUNIT_EXPECT_EQ(test, a.ccb_ctl, m + SGXFIVE_CTX_CTL_OFF);
	KUNIT_EXPECT_EQ(test, a.td_status, m + SGXFIVE_CTX_FENCE_OFF);
	KUNIT_EXPECT_EQ(test, a.ta_status, m + SGXFIVE_CTX_TA_OFF);
	KUNIT_EXPECT_EQ(test, a.rtds, m + SGXFIVE_RCTX_RTDS_OFF);
	KUNIT_EXPECT_EQ(test, a.pbdesc, m + SGXFIVE_RCTX_PBDESC_OFF);
	KUNIT_EXPECT_EQ(test, a.pbblock, m + SGXFIVE_RCTX_PBBLOCK_OFF);
	KUNIT_EXPECT_EQ(test, a.rd, m + SGXFIVE_RCTX_RD_OFF);
	/* Everything fits the context's one page, without overlaps. */
	KUNIT_EXPECT_LE(test, SGXFIVE_CTX_HW_OFF + SGX_RENDER_CTX_SIZE,
			SGXFIVE_CTX_CTL_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_BGPRI_OFF + 4, SGXFIVE_RCTX_RTDS_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_RTDS_OFF + SGX_RENDER_RTDS_SIZE,
			SGXFIVE_RCTX_PBDESC_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_PBDESC_OFF + SGX_RENDER_PBDESC_SIZE,
			SGXFIVE_RCTX_PBBLOCK_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_PBBLOCK_OFF + SGX_RENDER_PBBLOCK_SIZE,
			SGXFIVE_RCTX_RD_OFF);
	KUNIT_EXPECT_LE(test, SGXFIVE_RCTX_RD_OFF + SGX_RENDER_RD_SIZE, pg + PAGE_SIZE - m);
	KUNIT_EXPECT_EQ(test, a.access, m + SGXFIVE_RCTX_ACCESS_OFF);
	KUNIT_EXPECT_EQ(test, a.bgpri, m + SGXFIVE_RCTX_BGPRI_OFF);
	/* The TA CCB has a command's room past its 64 KiB. */
	KUNIT_EXPECT_GE(test, SGXFIVE_CTX_PAGE_OFF,
			SGX_RENDER_CCB_SIZE + SGX_RENDER_CMD_STRIDE);
}

/*
 * A render job's firmware image, kept in its hardware fence: the TA command (one
 * whole scene, the seqno as frame and as both status values) and, after it,
 * the render details.
 */
static void sgxfive_job_test_render_build(struct kunit *test)
{
	struct drm_sgxfive_render_cmd c;
	struct sgx_render_addrs a;
	struct sgx_render_tadata td;
	u8 *img = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	u32 *rd = kunit_kzalloc(test, SGX_RENDER_RD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, img);
	KUNIT_ASSERT_NOT_NULL(test, rd);
	KUNIT_EXPECT_LE(test, SGXFIVE_RENDER_IMG_RD + SGX_RENDER_RD_SIZE,
			SGX_XFER_CMD_SIZE);
	test_cmd(&c);
	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 64, 64, 1), 0);
	sgxfive_render_addrs(&a, 0x32500000, 0x01234000, 0, &td);
	memset(img, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgxfive_render_build(img, &a, &c, 9, true, true),
			(u32)SGX_RENDER_CMD_STRIDE);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_FLAGS),
			(u32)(SGX_RENDER_TA_FIRST | SGX_RENDER_TA_LAST | SGX_RENDER_TA_INVAL));
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_RFLAGS), (u32)SGX_RENDER_SCENE);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_FRAME), 9u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_TA_STATUS), a.ta_status);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_TA_STATUS + 4), 9u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_3D_STATUS), a.td_status);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_SH_3D_STATUS + 4), 9u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_TAREGS + SGX_RENDER_TA_TEPSG),
			(u32)SGX_RENDER_TEPSG_END);
	sgx_render_rd(rd, &a, &c);
	KUNIT_EXPECT_EQ(test, memcmp(img + SGXFIVE_RENDER_IMG_RD, rd, SGX_RENDER_RD_SIZE),
			0);
	/* A geometry job opening a scene: no last kick, no end of scene. */
	sgxfive_render_build(img, &a, &c, 10, true, false);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_FLAGS),
			(u32)(SGX_RENDER_TA_FIRST | SGX_RENDER_TA_INVAL));
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_TAREGS + SGX_RENDER_TA_TEPSG), 0u);
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

/*
 * At the reset that ends a held recovery the unfinished jobs of a closed
 * context, which its client gave up, fail with -ECANCELED instead of being
 * rewound to run again; an open context's jobs stay pending, and so do the
 * jobs of every context while nothing is cancelled.
 */
static void sgxfive_job_test_cancel_closed(struct kunit *test)
{
	struct sgxfive_ctx *a = fake_ctx(test), *b = fake_ctx(test), *c = fake_ctx(test);
	struct dma_fence *f[4];
	LIST_HEAD(ctxs);
	u64 order;
	int i;

	list_add_tail(&a->link, &ctxs);
	list_add_tail(&b->link, &ctxs);
	list_add_tail(&c->link, &ctxs);
	f[0] = fake_job(test, a, 1, 10);
	f[1] = fake_job(test, a, 2, 11);
	f[2] = fake_job(test, b, 1, 12);
	f[3] = fake_job(test, b, 2, 13);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_NOT_NULL(test, f[i]);

	/* Nothing is closed: nothing is cancelled. */
	KUNIT_EXPECT_EQ(test, sgxfive_fence_cancel_closed(&ctxs), 0u);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[i]));

	/* A closed context, with jobs and without, and an open one. */
	WRITE_ONCE(a->closed, true);
	WRITE_ONCE(c->closed, true);
	KUNIT_EXPECT_EQ(test, sgxfive_fence_cancel_closed(&ctxs), 2u);
	KUNIT_EXPECT_TRUE(test, dma_fence_is_signaled(f[0]));
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -ECANCELED);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[1]), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, sgxfive_ctx_oldest(a, &order));
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(a), 0);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[2]));
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[3]));
	KUNIT_EXPECT_TRUE(test, sgxfive_ctx_oldest(b, &order));
	KUNIT_EXPECT_EQ(test, order, 12ull);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(b), 2);
	/* Again: nothing left to cancel. */
	KUNIT_EXPECT_EQ(test, sgxfive_fence_cancel_closed(&ctxs), 0u);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[2]));

	/* A job the guilty context already failed keeps its error. */
	WRITE_ONCE(b->closed, true);
	sgxfive_fence_fail_guilty(b, -EIO);
	KUNIT_EXPECT_EQ(test, sgxfive_fence_cancel_closed(&ctxs), 0u);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[2]), -EIO);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[3]), -ECANCELED);
	for (i = 0; i < 4; i++)
		dma_fence_put(f[i]);
}

/*
 * A render job whose scene overflowed the parameter buffer: the EDM ran a
 * partial render, which the driver does not support, so the job's fence
 * fails with -ENOSPC when it completes. The context is not lost: its next
 * job, with the counters cleared when it runs, signals without an error. A
 * transfer context's fence ignores the words a render context keeps there.
 */
static void sgxfive_job_test_overflow(struct kunit *test)
{
	struct sgxfive_ctx *ctx = fake_ctx(test), *xfer = fake_ctx(test);
	u32 *rtds = sgxfive_ctx_cpu(ctx, SGXFIVE_RCTX_RTDS_OFF);
	u32 *done = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF);
	struct dma_fence *f[3];
	int i;

	ctx->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	xfer->type = DRM_SGXFIVE_CTX_TYPE_TRANSFER;
	f[0] = fake_job(test, ctx, 1, 10);
	KUNIT_ASSERT_NOT_NULL(test, f[0]);
	rtds[SGX_RENDER_RTDS_OOM / 4] = 4;
	rtds[SGX_RENDER_RTDS_SPM / 4] = 4;
	sgxfive_fence_signal(ctx);			/* not done yet */
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[0]));
	*done = 1;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), -ENOSPC);
	KUNIT_EXPECT_FALSE(test, ctx->lost);

	/* The next job runs with the counters cleared. */
	f[1] = fake_job(test, ctx, 2, 11);
	KUNIT_ASSERT_NOT_NULL(test, f[1]);
	sgx_render_rtds_job(rtds);
	*done = 2;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[1]), 1);

	/* A transfer context has no RT data set. */
	rtds = sgxfive_ctx_cpu(xfer, SGXFIVE_RCTX_RTDS_OFF);
	rtds[SGX_RENDER_RTDS_SPM / 4] = 1;
	f[2] = fake_job(test, xfer, 1, 12);
	KUNIT_ASSERT_NOT_NULL(test, f[2]);
	*(u32 *)sgxfive_ctx_cpu(xfer, SGXFIVE_CTX_FENCE_OFF) = 1;
	sgxfive_fence_signal(xfer);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[2]), 1);
	for (i = 0; i < 3; i++)
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

/*
 * The scene a render job kicks: a geometry job (DRM_SGXFIVE_SUBMIT_JOB_RENDER_GEOMETRY)
 * leaves it open for the next job of the context; the first job after a closed scene
 * opens one. Jobs in submission order: geometry, geometry, whole, whole.
 */
static void sgxfive_job_test_render_scene(struct kunit *test)
{
	static const struct { bool geometry, first, last; } seq[] = {
		{ true, true, false }, { true, false, false },
		{ false, false, true }, { false, true, true },
	};
	struct sgxfive_ctx *ctx = fake_ctx(test);
	struct sgxfive_job job;
	unsigned int i;

	ctx->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		sgxfive_render_job_scene(ctx, &job, seq[i].geometry);
		KUNIT_EXPECT_EQ(test, job.first, seq[i].first);
		KUNIT_EXPECT_EQ(test, job.last, seq[i].last);
	}
}

/*
 * A geometry job's fence is done once the context's TA word has reached its
 * number (no render writes the fence word for it); a whole job's waits for the
 * fence word, whatever the TA word says.
 */
static void sgxfive_job_test_geometry_fence(struct kunit *test)
{
	struct sgxfive_ctx *ctx = fake_ctx(test);
	u32 *done = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF);
	u32 *ta = sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_TA_OFF);
	struct dma_fence *f[2];

	ctx->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	f[0] = fake_job(test, ctx, 1, 10);
	f[1] = fake_job(test, ctx, 2, 11);
	KUNIT_ASSERT_NOT_NULL(test, f[0]);
	KUNIT_ASSERT_NOT_NULL(test, f[1]);
	container_of(f[0], struct sgxfive_fence, base)->ta_only = true;
	*ta = 1;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[0]), 1);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	*ta = 2;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_FALSE(test, dma_fence_is_signaled(f[1]));
	*done = 2;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(f[1]), 1);
	dma_fence_put(f[0]);
	dma_fence_put(f[1]);
}

/*
 * A recovery reset starts the RT data set over with the pending count the
 * jobs it runs again add to the executed count, kick by kick: a geometry kick
 * one, a render with render flag 0x400 one, a render without it (a debug poke
 * cleared it) none. One per job would leave the cleanup waiting for a count
 * the firmware never reaches.
 */
static void sgxfive_job_test_render_replay(struct kunit *test)
{
	static const struct { bool first, last, scene; } kicks[] = {
		{ true, false, true }, { false, true, true }, { true, true, false },
	};
	struct sgxfive_ctx *ctx = fake_ctx(test);
	struct drm_sgxfive_render_cmd c;
	struct sgx_render_addrs a;
	struct sgx_render_tadata td;
	struct dma_fence *f[ARRAY_SIZE(kicks)];
	unsigned int i;

	ctx->type = DRM_SGXFIVE_CTX_TYPE_RENDER;
	test_cmd(&c);
	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 64, 64, 1), 0);
	sgxfive_render_addrs(&a, 0x32500000, 0x01234000, 0, &td);
	KUNIT_EXPECT_EQ(test, sgxfive_render_replay_pending(ctx), 0u);
	for (i = 0; i < ARRAY_SIZE(kicks); i++) {
		struct sgxfive_fence *hw = sgxfive_fence_alloc();
		u32 rflags;

		KUNIT_ASSERT_NOT_NULL(test, hw);
		sgxfive_render_build(hw->cmd, &a, &c, i + 1, kicks[i].first, kicks[i].last);
		rflags = dw(hw->cmd, SGX_RENDER_CMD_RFLAGS);
		if (!kicks[i].scene)
			rflags &= ~SGX_RENDER_SCENE;
		memcpy(hw->cmd + SGX_RENDER_CMD_RFLAGS, &rflags, 4);
		f[i] = sgxfive_fence_init(hw, ctx, i + 1, 10 + i, SGX_RENDER_CMD_STRIDE);
	}
	/* Three jobs run again: the geometry kick and the closing render add one each. */
	KUNIT_EXPECT_EQ(test, sgxfive_render_replay_pending(ctx), 2u);

	/* The scene's two jobs done, the render without 0x400 alone runs again. */
	*(u32 *)sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_FENCE_OFF) = 2;
	sgxfive_fence_signal(ctx);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_rewind(ctx), 1u);
	KUNIT_EXPECT_EQ(test, sgxfive_render_replay_pending(ctx), 0u);

	sgxfive_fence_fail_all(ctx, -EIO);
	KUNIT_EXPECT_EQ(test, sgxfive_render_replay_pending(ctx), 0u);
	for (i = 0; i < ARRAY_SIZE(kicks); i++)
		dma_fence_put(f[i]);
}

static struct kunit_case sgxfive_job_test_cases[] = {
	KUNIT_CASE(sgxfive_job_test_build),
	KUNIT_CASE(sgxfive_job_test_dummy),
	KUNIT_CASE(sgxfive_job_test_check),
	KUNIT_CASE(sgxfive_job_test_ctx_args),
	KUNIT_CASE(sgxfive_job_test_render_check),
	KUNIT_CASE(sgxfive_job_test_render_addrs),
	KUNIT_CASE(sgxfive_job_test_render_build),
	KUNIT_CASE(sgxfive_job_test_render_scene),
	KUNIT_CASE(sgxfive_job_test_geometry_fence),
	KUNIT_CASE(sgxfive_job_test_seqno),
	KUNIT_CASE(sgxfive_job_test_copy_array),
	KUNIT_CASE(sgxfive_job_test_copy_array_limits),
	KUNIT_CASE(sgxfive_job_test_sync_op),
	KUNIT_CASE(sgxfive_job_test_sync_signals),
	KUNIT_CASE(sgxfive_job_test_rewind),
	KUNIT_CASE(sgxfive_job_test_render_replay),
	KUNIT_CASE(sgxfive_job_test_suspect),
	KUNIT_CASE(sgxfive_job_test_hang_error),
	KUNIT_CASE(sgxfive_job_test_guilty),
	KUNIT_CASE(sgxfive_job_test_cancel_closed),
	KUNIT_CASE(sgxfive_job_test_overflow),
	KUNIT_CASE(sgxfive_job_test_budget),
	{}
};

static struct kunit_suite sgxfive_job_test_suite = {
	.name = "gma500_sgxfive_job",
	.test_cases = sgxfive_job_test_cases,
};

kunit_test_suite(sgxfive_job_test_suite);
