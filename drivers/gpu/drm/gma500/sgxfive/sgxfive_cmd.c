// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The firmware form of sgxfive jobs: the UAPI transfer register block (struct
 * drm_sgxfive_transfer_cmd) becomes a firmware transfer command (sgx_xfer.h)
 * and a render command (struct drm_sgxfive_render_cmd) a TA command and
 * render details (sgx_render.h),
 * with the fields the kernel owns filled in; and the checks on them.
 */
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "sgx_render.h"
#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

/*
 * The transfer command's register block: 41 words, the first 17 of them the
 * UAPI block's fixed registers at the same offsets, the rest its YUV, luma-key
 * and FIR coefficients, which the UAPI block has after its load flags.
 */
#define CMD_REGS_SIZE		0xa4
#define CMD_FIXED_REGS		17	/* BG_TAG .. the word after PDS_EXEC_BASE */
#define CMD_LOAD_YUV		0x188	/* the firmware loads each group if set */
#define CMD_LOAD_LUMAKEY	0x18c
#define CMD_LOAD_FIR		0x190
#define PIXEL_PARTITIONS	0x07c00000	/* 31 partitions */
#define CMD_SIZE_IN_CCB		0x1c0		/* used part, 64-byte multiple */

#define UAPI_OFF(field)	offsetof(struct drm_sgxfive_transfer_cmd, field)
static_assert(UAPI_OFF(isp_bgobjtag) == SGX_XFER_BG_TAG);
static_assert(UAPI_OFF(isp_bgobj) == SGX_XFER_BG_OBJ);
static_assert(UAPI_OFF(isp_bgobjdepth) == SGX_XFER_BG_DEPTH);
static_assert(UAPI_OFF(isp_render) == SGX_XFER_RENDER);
static_assert(UAPI_OFF(isp_ipfmisc) == SGX_XFER_IPF_MISC);
static_assert(UAPI_OFF(bif_3d_req_base) == SGX_XFER_REQ_BASE);
static_assert(UAPI_OFF(isp_multisamplectl) == SGX_XFER_MULTISAMP);
static_assert(UAPI_OFF(event_pixel_pds_exec) == SGX_XFER_PIXEV_CODE);
static_assert(UAPI_OFF(event_pixel_pds_data) == SGX_XFER_PIXEV_DATA);
static_assert(UAPI_OFF(event_pixel_pds_info) == SGX_XFER_PIXEV_INFO);
static_assert(UAPI_OFF(isp_render_box1) == SGX_XFER_BOX_MIN);
static_assert(UAPI_OFF(isp_render_box2) == SGX_XFER_BOX_MAX);
static_assert(UAPI_OFF(pds_exec_base) == SGX_XFER_PDS_EXEC_BASE);
static_assert(UAPI_OFF(load_flags) == CMD_FIXED_REGS * 4);
static_assert(CMD_FIXED_REGS * 4 + sizeof(struct drm_sgxfive_transfer_cmd) -
	      UAPI_OFF(csc0_coeff) == CMD_REGS_SIZE);

static void put32(u8 *p, unsigned int off, u32 v)
{
	memcpy(p + off, &v, 4);		/* little-endian host */
}

/*
 * Fill cmd (SGX_XFER_CMD_SIZE bytes) for one job; regs is NULL for a dummy.
 * The firmware writes value to status_addr on completion. Returns the size
 * the command takes in the client CCB.
 */
u32 sgxfive_job_build(u8 *cmd, const struct drm_sgxfive_transfer_cmd *regs,
		      bool dummy, u32 status_addr, u32 value)
{
	memset(cmd, 0, SGX_XFER_CMD_SIZE);
	if (regs) {
		/*
		 * The UAPI block is the register block with load_flags inserted
		 * after the fixed registers: copy around it.
		 */
		const u8 *src = (const u8 *)regs;

		memcpy(cmd, src, CMD_FIXED_REGS * 4);
		memcpy(cmd + CMD_FIXED_REGS * 4, src + UAPI_OFF(csc0_coeff),
		       CMD_REGS_SIZE - CMD_FIXED_REGS * 4);
		put32(cmd, CMD_LOAD_YUV,
		      !!(regs->load_flags & DRM_SGXFIVE_TRANSFER_LOAD_YUV));
		put32(cmd, CMD_LOAD_LUMAKEY,
		      !!(regs->load_flags & DRM_SGXFIVE_TRANSFER_LOAD_LUMAKEY));
		put32(cmd, CMD_LOAD_FIR,
		      !!(regs->load_flags & DRM_SGXFIVE_TRANSFER_LOAD_FIR));
		put32(cmd, SGX_XFER_PIXEL_PARTITIONS, PIXEL_PARTITIONS);
		put32(cmd, SGX_XFER_FENCE, value);
	}
	put32(cmd, SGX_XFER_SIZE, CMD_SIZE_IN_CCB);
	put32(cmd, SGX_XFER_FLAGS, dummy ? SGX_XFER_FLAG_DUMMY : 0);
	put32(cmd, SGX_XFER_NUM_STATUS, 1);
	put32(cmd, SGX_XFER_STATUS, status_addr);
	put32(cmd, SGX_XFER_STATUS + 4, value);
	return CMD_SIZE_IN_CCB;
}

/* addr lies in a heap clients may map (one page from it must fit). */
static bool in_user_heap(u32 addr)
{
	return !sgxfive_vm_check_range(sgxfive_rev1_heaps, DRM_SGXFIVE_HEAP_COUNT,
				       addr & PAGE_MASK, PAGE_SIZE);
}

/*
 * The checks on a job's register block: the addresses the pixel pipe
 * fetches from or runs code at must be in the client's heaps, and fields
 * this kernel does not support must be clear. PDS_EXEC_BASE must be the
 * PDS_PIXEL heap base: the 3D pipe never sets it, so a transfer that moved it
 * would move the pixel programs of every later render.
 */
int sgxfive_job_check(const struct drm_sgxfive_transfer_cmd *regs)
{
	if (regs->load_flags & ~DRM_SGXFIVE_TRANSFER_LOAD_FLAGS_MASK ||
	    regs->double_pixel_partitions)
		return -EINVAL;
	if (regs->bif_3d_req_base & (SZ_1M - 1) ||
	    !in_user_heap(regs->bif_3d_req_base) ||
	    regs->pds_exec_base != sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_PDS_PIXEL].base ||
	    !in_user_heap(regs->event_pixel_pds_exec))
		return -EINVAL;
	if (regs->isp_rgn_base && !in_user_heap(regs->isp_rgn_base))
		return -EINVAL;
	return 0;
}

/*
 * The checks on a render command: the VDM stream and the pixel-event program
 * in the client's heaps, aligned (a render VM aliases the firmware, so a
 * pointer into the kernel heaps would reach it), the render target within the
 * context's maximum, max_w x max_h, for which its tiling buffers are sized, and
 * a multisampled TE_AA only where the context may take one (ms: made for 4x
 * targets, or with its region-header arrays forced by debugfs).
 */
int sgxfive_render_check(const struct drm_sgxfive_render_cmd *c, u32 max_w,
			 u32 max_h, bool ms)
{
	if (c->vdm_ctrl_stream_base & 3 || !in_user_heap(c->vdm_ctrl_stream_base) ||
	    c->event_pixel_pds_exec & 0xf || !in_user_heap(c->event_pixel_pds_exec))
		return -EINVAL;
	if (!ms && sgx_render_te_aa_ms(c->te_aa))
		return -EINVAL;
	return sgx_render_check_size(c, max_w, max_h);
}

/*
 * A render with a one-layer background object (debugfs render_bg_layer): its
 * source (debugfs render_bg_src, which the user's PDS program samples) must
 * be named and mapped in the render's VM (pte, its PTE there), or the render
 * is refused. Without the layer, nothing to check.
 */
int sgxfive_render_check_bg(bool layer, u32 src, u32 pte)
{
	if (layer && (!src || !(pte & SGX_MMU_VALID)))
		return -EINVAL;
	return 0;
}

/*
 * Where a render context's firmware structures are: in its kernel memory at
 * mem (the layout of sgxfive_ctx.h), and its slots in the reserved heaps,
 * the TA-data slot holding the buffers of td.
 */
void sgxfive_render_addrs(struct sgx_render_addrs *a, u32 mem, u32 pd_phys,
			  unsigned int slot, const struct sgx_render_tadata *td)
{
	*a = (struct sgx_render_addrs) {
		.pd_phys = pd_phys,
		.param_base = sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_PARAMETERS].base +
			      slot * SGXFIVE_RENDER_PARAM_SLOT,
		.tadata = sgxfive_rev1_heaps[DRM_SGXFIVE_HEAP_TA_DATA].base +
			  slot * SGXFIVE_RENDER_TD_SLOT,
		.td = *td,
		.ctx = mem + SGXFIVE_CTX_HW_OFF, .ccb = mem + SGXFIVE_CTX_CCB_OFF,
		.ccb_ctl = mem + SGXFIVE_CTX_CTL_OFF, .rtds = mem + SGXFIVE_RCTX_RTDS_OFF,
		.pbdesc = mem + SGXFIVE_RCTX_PBDESC_OFF,
		.pbblock = mem + SGXFIVE_RCTX_PBBLOCK_OFF, .rd = mem + SGXFIVE_RCTX_RD_OFF,
		.ta_status = mem + SGXFIVE_CTX_TA_OFF, .td_status = mem + SGXFIVE_CTX_FENCE_OFF,
		.access = mem + SGXFIVE_RCTX_ACCESS_OFF,
		.pending = mem + SGXFIVE_RCTX_PENDING_OFF,
		.rt_status = mem + SGXFIVE_RCTX_RTSTATUS_OFF,
		.bgpri = mem + SGXFIVE_RCTX_BGPRI_OFF,
	};
}

/*
 * A render job's firmware image (SGX_XFER_CMD_SIZE bytes, its hardware
 * fence's copy): the TA command of one kick of a scene (the whole scene with
 * first and last) with seqno as its frame and status values, then at
 * SGXFIVE_RENDER_IMG_RD its render details. Returns the TA command's size in
 * the TA CCB.
 */
u32 sgxfive_render_build(u8 *img, const struct sgx_render_addrs *a,
			 const struct drm_sgxfive_render_cmd *c, u32 seqno, bool first,
			 bool last)
{
	struct sgx_render_kick k;
	u32 size;

	memset(img, 0, SGX_XFER_CMD_SIZE);
	sgx_render_scene_kick(&k, seqno, seqno, first, last);
	size = sgx_render_cmd(img, a, c, &k);
	sgx_render_rd((u32 *)(img + SGXFIVE_RENDER_IMG_RD), a, c);
	return size;
}
