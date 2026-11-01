/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The kernel-owned side of an SGX545 TA + 3D job: the firmware structures of
 * one render context and the command of one scene, built from the register
 * words userspace supplies (struct drm_sgxfive_render_cmd). The render node
 * (sgxfive_render.c) builds its contexts and jobs with them.
 */
#ifndef _GMA500_SGX_RENDER_H_
#define _GMA500_SGX_RENDER_H_

#include <linux/types.h>

#include <drm/sgxfive_drm.h>

/* Structure sizes in bytes. */
#define SGX_RENDER_CMD_SIZE	0x2b0	/* the TA command */
#define SGX_RENDER_CMD_STRIDE	0x2c0	/* its size word: 64-byte units */
#define SGX_RENDER_CTX_SIZE	0x44	/* the HW render context */
#define SGX_RENDER_RD_SIZE	0x1ec	/* hw_render_details */
#define SGX_RENDER_RTDS_SIZE	0x50	/* hw_rt_data_set, one RT data */
#define SGX_RENDER_PBDESC_SIZE	0x3c	/* hw_pb_desc */
#define SGX_RENDER_PBBLOCK_SIZE	0x10	/* hw_pb_block */
#define SGX_RENDER_CCB_SIZE	0x10000	/* the EDM masks the read offset */

/* The TA command. */
#define SGX_RENDER_CMD_SIZE_W	0x000
#define SGX_RENDER_CMD_FLAGS	0x004
#define SGX_RENDER_CMD_SPEC	0x008	/* spec_object[3] */
#define SGX_RENDER_CMD_VPART	0x014
#define SGX_RENDER_CMD_SPM_VPART 0x018
#define SGX_RENDER_CMD_RFLAGS	0x01c
#define SGX_RENDER_CMD_FRAME	0x020
#define SGX_RENDER_CMD_PBDESC	0x024
#define SGX_RENDER_CMD_RD	0x038
#define SGX_RENDER_CMD_DSTSYNC	0x03c
#define SGX_RENDER_CMD_RTDS	0x040
#define SGX_RENDER_CMD_RTDATA	0x044
#define SGX_RENDER_CMD_NUM3DREGS 0x054
#define SGX_RENDER_CMD_SHARED	0x058	/* shared block: sync counts, status words */
#define SGX_RENDER_SH_CTRL	(SGX_RENDER_CMD_SHARED + 0x000)
#define SGX_RENDER_SH_NUM_TA	(SGX_RENDER_CMD_SHARED + 0x004)
#define SGX_RENDER_SH_NUM_3D	(SGX_RENDER_CMD_SHARED + 0x008)
#define SGX_RENDER_SH_TATQ	(SGX_RENDER_CMD_SHARED + 0x00c)
#define SGX_RENDER_SH_TA_STATUS	(SGX_RENDER_CMD_SHARED + 0x0c0) /* {addr, val}[32] */
#define SGX_RENDER_SH_3D_STATUS	(SGX_RENDER_CMD_SHARED + 0x1c0) /* {addr, val}[4] */
#define SGX_RENDER_CMD_TAREGS	0x238	/* hw_ta_registers */
#define SGX_RENDER_CMD_3DREGUPD	0x2a8

/* The TA registers, from SGX_RENDER_CMD_TAREGS. */
#define SGX_RENDER_TA_TEAA	0x00
#define SGX_RENDER_TA_TEMTILE1	0x04
#define SGX_RENDER_TA_TEMTILE2	0x08
#define SGX_RENDER_TA_TESCREEN	0x0c
#define SGX_RENDER_TA_TEMTILESTRIDE 0x10
#define SGX_RENDER_TA_TEPSG	0x14
#define SGX_RENDER_TA_VDM	0x18
#define SGX_RENDER_TA_MTECTRL	0x1c
#define SGX_RENDER_TA_PSGRGN	0x20
#define SGX_RENDER_TA_TPC	0x24
#define SGX_RENDER_TA_WCOMPARE	0x28
#define SGX_RENDER_TA_WCLAMP	0x2c
#define SGX_RENDER_TA_MTESCREEN	0x30
#define SGX_RENDER_TA_REQBASE	0x3c
#define SGX_RENDER_TA_MTEMSCTRL	0x6c

/* The TA command's flags word and render flags. */
#define SGX_RENDER_TA_FIRST	0x01
#define SGX_RENDER_TA_LAST	0x02
#define SGX_RENDER_TA_INVAL	0x70	/* USE, PDS and data caches */
#define SGX_RENDER_SCENE	0x400	/* render flag: render the scene */
#define SGX_RENDER_TEPSG_END	0x400000	/* the scene's last kick */

/* The HW render context. */
#define SGX_RENDER_CTX_FLAGS	0x00
#define SGX_RENDER_CTX_PRIORITY	0x04
#define SGX_RENDER_CTX_PD	0x08
#define SGX_RENDER_CTX_CCB	0x0c
#define SGX_RENDER_CTX_CCB_CTL	0x10
#define SGX_RENDER_CTX_TACOUNT	0x14
#define SGX_RENDER_CTX_PBDESC	0x18
#define SGX_RENDER_CTX_PRIM	0x1c
#define SGX_RENDER_CTX_PARTIAL	0x20	/* list heads: partial, complete */
#define SGX_RENDER_CTX_COMPLETE	0x28
#define SGX_RENDER_CTX_PID	0x40
#define SGX_RENDER_CTX_NEW	0x1

/* The render details: the 52 3D registers, then the rest. */
#define SGX_RENDER_RD_REQBASE	0x000
#define SGX_RENDER_RD_PIXELBE	0x008
#define SGX_RENDER_RD_IPFMISC	0x00c
#define SGX_RENDER_RD_AAMODE	0x010
#define SGX_RENDER_RD_RGNBASE	0x020
#define SGX_RENDER_RD_ZLSEXTZ	0x028
#define SGX_RENDER_RD_ZLSCTL	0x02c
#define SGX_RENDER_RD_SLOAD	0x038
#define SGX_RENDER_RD_SSTORE	0x03c
#define SGX_RENDER_RD_BGDEPTH	0x040
#define SGX_RENDER_RD_BGOBJ	0x044
#define SGX_RENDER_RD_BGTAG	0x048
#define SGX_RENDER_RD_MSCTL	0x050
#define SGX_RENDER_RD_TSPCONFIG	0x058
#define SGX_RENDER_RD_PIXEV_EXEC 0x05c
#define SGX_RENDER_RD_PIXEV_DATA 0x060
#define SGX_RENDER_RD_PIXEV_INFO 0x064
#define SGX_RENDER_RD_FPUCTRL	0x068
#define SGX_RENDER_RD_DEPTHSORT	0x0cc
#define SGX_RENDER_RD_ACCESS	0x0d0
#define SGX_RENDER_RD_RFLAGS	0x0d4
#define SGX_RENDER_RD_RTDS	0x0d8
#define SGX_RENDER_RD_RTDATA	0x0dc
#define SGX_RENDER_RD_TQSYNC	0x0e4
#define SGX_RENDER_RD_NUMRTS	0x104
#define SGX_RENDER_RD_NEXTRT	0x108
#define SGX_RENDER_RD_PIXEV_UPD	0x10c
#define SGX_RENDER_RD_BGPRI_LIST 0x114
#define SGX_RENDER_RD_TESTATE	0x124
#define SGX_RENDER_RD_SPEC	0x130
#define SGX_RENDER_RD_NUMPIXPART 0x13c
#define SGX_RENDER_RD_NUM3DSTATUS 0x144

/* The RT data set and its RT data, at SGX_RENDER_RTDS_RT. */
#define SGX_RENDER_RTDS_PENDING	0x00
#define SGX_RENDER_RTDS_COMPLETE 0x04
#define SGX_RENDER_RTDS_MTILEW	0x08
#define SGX_RENDER_RTDS_OOM	0x0c	/* the EDM's: TA out of pages */
#define SGX_RENDER_RTDS_SPM	0x10	/* the EDM's: partial renders run */
#define SGX_RENDER_RTDS_NUMRT	0x14
#define SGX_RENDER_RTDS_RT	0x18
#define SGX_RENDER_RT_CTX	0x00
#define SGX_RENDER_RT_RD	0x04
#define SGX_RENDER_RT_BLOCKS	0x08
#define SGX_RENDER_RT_RGNARRAY	0x0c
#define SGX_RENDER_RT_BG	0x10
#define SGX_RENDER_RT_STATE	0x14
#define SGX_RENDER_RT_CONTROL	0x18
#define SGX_RENDER_RT_OTPM	0x1c
#define SGX_RENDER_RT_TAIL	0x20
#define SGX_RENDER_RT_TAILSIZE	0x24
#define SGX_RENDER_RT_NUMRTS	0x28
#define SGX_RENDER_RT_RGNSTRIDE	0x2c
#define SGX_RENDER_RT_STATUS	0x30
#define SGX_RENDER_RT_COMMON	0x34
#define SGX_RENDER_RT_FREE	0x10	/* CommonStatus: no render outstanding */

/* The PB descriptor and PB block. */
#define SGX_RENDER_PB_FLAGS	0x00
#define SGX_RENDER_PB_NUMPAGES	0x04
#define SGX_RENDER_PB_INIT_HT	0x08
#define SGX_RENDER_PB_INIT_PREV	0x0c
#define SGX_RENDER_PB_PT	0x10
#define SGX_RENDER_PB_ALLOC	0x14
#define SGX_RENDER_PB_HT	0x18
#define SGX_RENDER_PB_PREV	0x1c
#define SGX_RENDER_PB_TA_THRESH	0x28
#define SGX_RENDER_PB_BLOCKS	0x38

/* The parameter buffer: the pages the parameter slot holds. */
#define SGX_RENDER_PB_PAGES	0x422
#define SGX_RENDER_PT_BYTES	0x40000	/* one entry per page of the heap */

/*
 * A render context's parameter slot (BIF_TA/3D_REQ_BASE at its base): the PB
 * from the second page, so page index 0 is never handed out, then its page
 * table, the scratch primitive block and the background object (whose tag is
 * relative to the slot base).
 */
#define SGX_RENDER_PARAM_PB	0x1000
#define SGX_RENDER_PARAM_PT	0x423000
#define SGX_RENDER_PARAM_PRIM	0x463000
#define SGX_RENDER_PARAM_BG	0x464000
#define SGX_RENDER_PARAM_SIZE	0x465000

/*
 * The render targets the kernel takes: at most 4096 x 4096. A render context
 * states its own largest, at most this, when it is created.
 */
#define SGX_RENDER_MAX_W	4096
#define SGX_RENDER_MAX_H	4096

/* The TA data of a 4096 x 4096 maximum (struct sgx_render_tadata). */
#define SGX_RENDER_TD_MAX_SIZE	0x103000

/*
 * How the tiling engine splits a render target: 32 x 16-pixel tiles, grouped
 * into two or four macrotiles across and down, each a whole number of 4 x
 * 4-tile blocks. The TA registers, the RT data set's and RT data's size words
 * and the region-header and tail-pointer sizes all follow from the target's
 * size.
 */
struct sgx_render_layout {
	u32 te_screen, te_mtile1, te_mtile2, te_mtile_stride;	/* TA registers */
	u32 mtile_width;	/* RT data set: tiles across all macrotiles */
	u32 blocks_per_mt;	/* RT data: 4 x 4-tile blocks per macrotile */
	u32 rgn_array_size;	/* RT data: region headers of one macrotile */
	u32 tail_size;		/* RT data: the tail pointers */
	u32 rgn_size;		/* the region-header buffer, every macrotile */
};

/*
 * The RT data's zeroed buffers in a render context's TA-data slot, as
 * offsets from the slot base, for the largest target the context takes: the
 * tail pointers and the two region-header buffers of that target's layout
 * (sgx_render_layout(), which no smaller target exceeds), the DPM context
 * control, OTPM and DPM context state a page each; every buffer whole pages,
 * packed in that order.
 */
struct sgx_render_tadata {
	u32 tail;		/* tail pointers */
	u32 control;		/* DPM context control */
	u32 otpm;
	u32 state;		/* DPM context state */
	u32 rgn;		/* region headers */
	u32 zls;		/* ZLS external-Z region headers */
	u32 size;		/* all of it */
};

/*
 * Where one render context's kernel-owned items are, by device address. The
 * parameter and TA-data slots are mapped in the context's page directory, the
 * rest in the EDM's (and aliased read-only in the context's).
 */
struct sgx_render_addrs {
	u32 pd_phys;		/* the context's page directory */
	u32 param_base;		/* the parameter slot: SGX_RENDER_PARAM_* */
	u32 tadata;		/* the TA-data slot */
	struct sgx_render_tadata td;	/* its buffers, from tadata */
	u32 ctx;		/* the HW render context */
	u32 ccb, ccb_ctl;	/* the TA client CCB and {write, read} offsets */
	u32 rtds;		/* the RT data set, its one RT data after it */
	u32 pbdesc, pbblock;
	u32 rd;			/* the render details */
	u32 ta_status;		/* written at the TA end */
	u32 td_status;		/* written at the render end: the fence */
	u32 access;		/* the render details' access word */
	u32 pending;		/* the RT data set's pending count */
	u32 rt_status;		/* the render target's status word */
	u32 bgpri;		/* the background primary-PDS update list */
};

/* What one kick sets in the command besides the user's registers. */
struct sgx_render_kick {
	u32 flags;		/* SGX_RENDER_TA_* */
	u32 render_flags;
	u32 te_psg;		/* the TE_PSG word */
	u32 frame;
	u32 ta_val, td_val;	/* written to ta_status and td_status when done */
};

void sgx_render_ctx(u32 *ctx, const struct sgx_render_addrs *a);
void sgx_render_pb(u32 *desc, u32 *block, u32 *pt, const struct sgx_render_addrs *a);
void sgx_render_layout(struct sgx_render_layout *l, u32 width, u32 height);
int sgx_render_check_max(u32 max_w, u32 max_h);
int sgx_render_tadata_layout(struct sgx_render_tadata *t, u32 max_w, u32 max_h);
void sgx_render_rtds(u32 *rtds, const struct sgx_render_addrs *a, u32 mte_screen);
void sgx_render_rtds_resize(u32 *rtds, u32 mte_screen);
void sgx_render_rtds_job(u32 *rtds);
int sgx_render_rtds_error(const u32 *rtds);
void sgx_render_rd(u32 *rd, const struct sgx_render_addrs *a,
		   const struct drm_sgxfive_render_cmd *c);
u32 sgx_render_cmd(u8 *cmd, const struct sgx_render_addrs *a,
		   const struct drm_sgxfive_render_cmd *c,
		   const struct sgx_render_kick *k);
void sgx_render_bgobj(u32 *bg, const struct drm_sgxfive_render_cmd *c);
void sgx_render_scene_kick(struct sgx_render_kick *k, u32 seqno, u32 status_val,
			   bool first, bool last);
int sgx_render_check_size(const struct drm_sgxfive_render_cmd *c, u32 max_w,
			  u32 max_h);

#endif
