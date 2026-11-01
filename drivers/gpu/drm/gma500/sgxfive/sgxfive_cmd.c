// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The firmware form of sgxfive jobs: the UAPI transfer register block (struct
 * drm_sgxfive_transfer_cmd) becomes a firmware transfer command (sgx_xfer.h)
 * with the fields the kernel owns filled in; and the checks on them.
 */
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/string.h>

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
