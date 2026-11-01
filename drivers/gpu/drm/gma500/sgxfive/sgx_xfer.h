/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * SGX545 transfer-queue structures: the transfer command (SGXMKIF_TRANSFERCMD,
 * 0x4c8 bytes) and the HW transfer context (SGXMKIF_HWTRANSFERCONTEXT, 0xb4),
 * laid out as resgx545's doc/specs/transfer.md gives them.
 */
#ifndef _GMA500_SGX_XFER_H_
#define _GMA500_SGX_XFER_H_

#include "sgx_fw_types.h"

#define SGX_XFER_CMD_SIZE	0x4c8
#define SGX_XFER_CTX_SIZE	0xb4
#define SGX_XFER_CCB_SIZE	0x10000		/* the EDM masks the read offset */

/* The HW transfer context. */
#define SGX_XFER_CTX_FLAGS	0x00
#define SGX_XFER_CTX_PRIORITY	0x04
#define SGX_XFER_CTX_PD		0x08		/* page directory, physical */
#define SGX_XFER_CTX_CCB	0x0c		/* client CCB base */
#define SGX_XFER_CTX_CCB_CTL	0x10		/* its control block */
#define SGX_XFER_CTX_COUNT	0x14		/* commands queued, EDM-kept */

/* The transfer command. */
#define SGX_XFER_SIZE		0x0a4		/* used size in the CCB */
#define SGX_XFER_FLAGS		0x0a8
#define SGX_XFER_SHARED		0x0ac		/* the kernel-written part */
#define SGX_XFER_NUM_SRC	(SGX_XFER_SHARED + 0x00)
#define SGX_XFER_NUM_DST	(SGX_XFER_SHARED + 0x84)
#define SGX_XFER_DST		(SGX_XFER_SHARED + 0x88)	/* one sync */
#define SGX_XFER_NUM_STATUS	(SGX_XFER_SHARED + 0xb8)
#define SGX_XFER_STATUS		(SGX_XFER_SHARED + 0xbc)	/* {addr, val}[4] */
#define SGX_XFER_NUM_UPDATES	0x19c
#define SGX_XFER_UPDATES	0x1a0				/* {addr, val}[101] */
#define SGX_XFER_MAX_UPDATES	101
#define SGX_XFER_FLAG_DUMMY	0x20		/* no render, only the queue work */
#define SGX_XFER_PIXEL_PARTITIONS 0x194
#define SGX_XFER_FENCE		0x198		/* the context's fence count */

/*
 * The register block at the start of the command: words the EDM loads into
 * the 3D pipe's registers before it starts the render.
 */
#define SGX_XFER_BG_TAG		0x000	/* background object: address, layers */
#define SGX_XFER_BG_OBJ		0x004
#define SGX_XFER_BG_DEPTH	0x008
#define SGX_XFER_RENDER		0x00c	/* render mode */
#define SGX_XFER_IPF_MISC	0x014
#define SGX_XFER_REQ_BASE	0x018	/* the 3D requester's 1 MiB window */
#define SGX_XFER_REG_01C	0x01c
#define SGX_XFER_MULTISAMP	0x020
#define SGX_XFER_PIXEV_CODE	0x024	/* the end-of-tile PDS program */
#define SGX_XFER_PIXEV_DATA	0x028	/* ...its data size, 16-byte units */
#define SGX_XFER_PIXEV_INFO	0x02c	/* ...its attribute count */
#define SGX_XFER_BOX_MIN	0x030	/* render box in 32 x 16 tiles: y | x << 16 */
#define SGX_XFER_BOX_MAX	0x034	/* ...inclusive */
#define SGX_XFER_REG_038	0x038
#define SGX_XFER_PDS_EXEC_BASE	0x03c

/*
 * A device sync object: the pending values a command waits for and the
 * device addresses of the matching complete counters.
 */
struct sgx_xfer_sync {
	u32 read_pending, read_complete;
	u32 write_pending, write_complete;
};

/* A transfer rectangle in pixels; x1 and y1 are exclusive. */
struct sgx_xfer_rect {
	u32 x0, y0, x1, y1;
};

#define SGX_XFER_BGOBJ_DWORDS	17	/* the background object, no layers */
#define SGX_XFER_BGOBJ1_DWORDS	23	/* ... with one texture layer (a copy) */

/* What a fill's or a copy's command needs besides its programs. */
struct sgx_xfer_fill {
	struct sgx_xfer_rect rect;
	u32 bg;			/* background object address */
	u32 req_base;		/* the 1 MiB window holding bg */
	u32 pixev_exec;		/* the end-of-tile PDS program */
	u32 pds_exec_base;	/* the PDS code base */
	u32 status_addr;	/* the fence word */
	u32 fence_id;		/* written there as fence_id + 1 */
	u32 layers;		/* texture layers in the background object */
};

void sgx_xfer_bgobj(u32 bg[SGX_XFER_BGOBJ_DWORDS], u32 pds_base, u32 pri,
		    u32 pri_data, u32 sec, u32 sec_data,
		    const struct sgx_xfer_rect *r);
void sgx_xfer_bgobj_copy(u32 bg[SGX_XFER_BGOBJ1_DWORDS], u32 pds_base,
			 u32 pri, u32 pri_data, u32 sec, u32 sec_data,
			 const struct sgx_xfer_rect *dst,
			 const struct sgx_xfer_rect *src, u32 src_w, u32 src_h);
u32 sgx_xfer_fill_cmd(u8 *cmd, const struct sgx_xfer_fill *f);

u32 sgx_xfer_dummy(u8 *cmd, u32 status_addr, u32 status_val, u32 update_addr,
		   u32 update_val, const struct sgx_xfer_sync *dst);
void sgx_xfer_ctx_init(u8 *ctx, u32 pd_phys, u32 ccb, u32 ccb_ctl);
void sgx_xfer_ctx_pd(u8 *ctx, u32 pd_phys);
u32 sgx_xfer_add_update(u8 *cmd, u32 addr, u32 val);

#endif
