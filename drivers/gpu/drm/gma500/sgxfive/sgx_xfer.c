// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Builders for the SGX545 transfer queue (resgx545's doc/specs/transfer.md).
 * A dummy transfer runs the EDM's whole queue path except the render: the
 * sync test, the switch to the context's page directory, the memory updates,
 * the status writes, the SW_EVENT interrupt and the client CCB advance. A
 * fill adds a render: a background object whose PDS programs come from the
 * firmware file's templates, and the command's register block. A copy's
 * object also carries one texture layer: the source's coordinates.
 */
#include <linux/math64.h>

#include "sgx_xfer.h"

static void put32(u8 *p, unsigned int off, u32 v)
{
	p[off] = v;
	p[off + 1] = v >> 8;
	p[off + 2] = v >> 16;
	p[off + 3] = v >> 24;
}

/*
 * Returns the command's size in the CCB: its used part in bytes, a
 * multiple of 64. With a destination sync the EDM starts the command only once both complete
 * counters equal their pending values, and moves the write counter past its
 * pending value when done.
 */
u32 sgx_xfer_dummy(u8 *cmd, u32 status_addr, u32 status_val, u32 update_addr,
		   u32 update_val, const struct sgx_xfer_sync *dst)
{
	const u32 size = (SGX_XFER_UPDATES + 8 + 0x3f) & ~0x3fu;
	unsigned int i;

	for (i = 0; i < SGX_XFER_CMD_SIZE; i++)
		cmd[i] = 0;
	put32(cmd, SGX_XFER_SIZE, size);
	put32(cmd, SGX_XFER_FLAGS, SGX_XFER_FLAG_DUMMY);
	put32(cmd, SGX_XFER_NUM_STATUS, 1);
	put32(cmd, SGX_XFER_STATUS, status_addr);
	put32(cmd, SGX_XFER_STATUS + 4, status_val);
	if (dst) {
		put32(cmd, SGX_XFER_NUM_DST, 1);
		put32(cmd, SGX_XFER_DST, dst->read_pending);
		put32(cmd, SGX_XFER_DST + 4, dst->read_complete);
		put32(cmd, SGX_XFER_DST + 8, dst->write_pending);
		put32(cmd, SGX_XFER_DST + 12, dst->write_complete);
	}
	put32(cmd, SGX_XFER_NUM_UPDATES, 1);
	put32(cmd, SGX_XFER_UPDATES, update_addr);
	put32(cmd, SGX_XFER_UPDATES + 4, update_val);
	return size;
}

/* A priority-0 context: the EDM scans only that queue. */
void sgx_xfer_ctx_init(u8 *ctx, u32 pd_phys, u32 ccb, u32 ccb_ctl)
{
	unsigned int i;

	for (i = 0; i < SGX_XFER_CTX_SIZE; i++)
		ctx[i] = 0;
	put32(ctx, SGX_XFER_CTX_PD, pd_phys);
	put32(ctx, SGX_XFER_CTX_CCB, ccb);
	put32(ctx, SGX_XFER_CTX_CCB_CTL, ccb_ctl);
}

/* Vertex coordinates: 24-bit fixed point, 8 fraction bits, offset 4096. */
static u32 vtx(u32 c)
{
	return ((c + 0x1000) << 8) & 0xffffff;
}

/*
 * The three vertices (top-left, top-right, bottom-left) in 8 dwords, 80 bits
 * each: Y, then X, then 32 bits of zero (Z = 0). So the second vertex starts
 * in the high half of v[2].
 */
static void put_vertices(u32 v[8], const struct sgx_xfer_rect *r)
{
	u32 x0 = vtx(r->x0), y0 = vtx(r->y0), x1 = vtx(r->x1), y1 = vtx(r->y1);

	v[0] = y0 | (x0 & 0xff) << 24;
	v[1] = x0 >> 8;
	v[2] = (y0 & 0xffff) << 16;
	v[3] = (y0 >> 16) | x1 << 8;
	v[4] = 0;
	v[5] = y1 | (x0 & 0xff) << 24;
	v[6] = x0 >> 8;
	v[7] = 0;
}

/*
 * The PDS state words: the secondary program's offset from the PDS code base
 * and its data size, the primary's attribute count, then the primary's
 * offset and data size. Offsets and sizes are in 16-byte units.
 */
static void put_pds_state(u32 bg[3], u32 pds_base, u32 pri, u32 pri_data,
			  u32 sec, u32 sec_data, u32 pri_attrs)
{
	bg[0] = ((sec - pds_base) >> 4) | (sec_data >> 4) << 26;
	bg[1] = (pri_attrs + 3) >> 2 | 3 << 16;
	bg[2] = ((pri - pds_base) >> 4) | (pri_data >> 4) << 26;
}

/*
 * The background object of a fast-scale render with no texture layer: the
 * PDS state for the secondary and primary programs, then the rectangle's
 * vertices. pri_data and sec_data are the programs' data sizes in bytes.
 */
void sgx_xfer_bgobj(u32 bg[SGX_XFER_BGOBJ_DWORDS], u32 pds_base, u32 pri,
		    u32 pri_data, u32 sec, u32 sec_data,
		    const struct sgx_xfer_rect *r)
{
	unsigned int i;

	for (i = 0; i < SGX_XFER_BGOBJ_DWORDS; i++)
		bg[i] = 0;
	put_pds_state(bg, pds_base, pri, pri_data, sec, sec_data, 0);
	put_vertices(&bg[9], r);
}

/*
 * x / n as an IEEE single, rounded to nearest: a normalised texture
 * coordinate. 0 for x = 0, 1.0 for x >= n. Sources are at most 4096 pixels
 * a side, so the scaled quotient fits 64 bits.
 */
static u32 coord(u32 x, u32 n)
{
	unsigned int k = 1;
	u32 rem;
	u64 q;

	if (!x || !n)
		return 0;
	if (x >= n)
		return 0x3f800000;
	while ((u64)x << k < n)		/* x / n in [2^-k, 2^-(k-1)) */
		k++;
	q = div_u64_rem((u64)x << (k + 23), n, &rem);
	if (2 * (u64)rem >= n)
		q++;
	if (q >> 24) {			/* rounded up to the next power of 2 */
		q >>= 1;
		k--;
	}
	return (127 - k) << 23 | ((u32)q & 0x7fffff);
}

/*
 * The background object of a one-layer copy: the PDS state (one primary
 * attribute: the texel), the source rectangle's texture coordinates per
 * vertex (U0 V0, U1 V0, U0 V1, normalised to the source surface), the layer
 * format word (two dwords per layer, in two fields), then the destination
 * rectangle's vertices.
 */
void sgx_xfer_bgobj_copy(u32 bg[SGX_XFER_BGOBJ1_DWORDS], u32 pds_base,
			 u32 pri, u32 pri_data, u32 sec, u32 sec_data,
			 const struct sgx_xfer_rect *dst,
			 const struct sgx_xfer_rect *src, u32 src_w, u32 src_h)
{
	u32 u0 = coord(src->x0, src_w), u1 = coord(src->x1, src_w);
	u32 v0 = coord(src->y0, src_h), v1 = coord(src->y1, src_h);
	unsigned int i;

	for (i = 0; i < SGX_XFER_BGOBJ1_DWORDS; i++)
		bg[i] = 0;
	put_pds_state(bg, pds_base, pri, pri_data, sec, sec_data, 1);
	bg[3] = u0;
	bg[4] = v0;
	bg[5] = u1;
	bg[6] = v0;
	bg[7] = u0;
	bg[8] = v1;
	bg[10] = 2 << 8 | 2;
	put_vertices(&bg[15], dst);
}

/*
 * A fill's or a copy's command: the register block, the size, one status
 * write (the fence) and the fence count. They differ only in the layer count
 * of the background object's tag. The fixed words are those the firmware
 * file's TQ_CMD list holds; the debug checks compare them. Returns the
 * command's size in the CCB.
 */
u32 sgx_xfer_fill_cmd(u8 *cmd, const struct sgx_xfer_fill *f)
{
	const struct sgx_xfer_rect *r = &f->rect;
	const u32 size = 0x1c0;
	unsigned int i;

	for (i = 0; i < SGX_XFER_CMD_SIZE; i++)
		cmd[i] = 0;
	put32(cmd, SGX_XFER_BG_TAG,
	      (f->bg - f->req_base) >> 4 | (f->layers & 0x3f) << 25);
	put32(cmd, SGX_XFER_BG_OBJ, 0x200);		/* background enabled */
	put32(cmd, SGX_XFER_BG_DEPTH, 0x3f800000);	/* 1.0f */
	put32(cmd, SGX_XFER_RENDER, 3);			/* fast scale */
	put32(cmd, SGX_XFER_IPF_MISC, 0x100);		/* process empty tiles */
	put32(cmd, SGX_XFER_REQ_BASE, f->req_base);
	put32(cmd, SGX_XFER_REG_01C, 0x80000000);
	put32(cmd, SGX_XFER_MULTISAMP, 0x88);
	put32(cmd, SGX_XFER_PIXEV_CODE, f->pixev_exec & ~0xfu);
	put32(cmd, SGX_XFER_PIXEV_DATA, 5);		/* 0x50 bytes */
	put32(cmd, SGX_XFER_PIXEV_INFO, 1);
	put32(cmd, SGX_XFER_BOX_MIN, (r->y0 >> 4) | (r->x0 >> 5) << 16);
	put32(cmd, SGX_XFER_BOX_MAX, (((r->y1 + 15) >> 4) - 1) |
				     (((r->x1 + 31) >> 5) - 1) << 16);
	put32(cmd, SGX_XFER_REG_038, 0x80);
	put32(cmd, SGX_XFER_PDS_EXEC_BASE, f->pds_exec_base);
	put32(cmd, SGX_XFER_SIZE, size);
	put32(cmd, SGX_XFER_NUM_STATUS, 1);
	put32(cmd, SGX_XFER_STATUS, f->status_addr);
	put32(cmd, SGX_XFER_STATUS + 4, f->fence_id + 1);
	put32(cmd, SGX_XFER_PIXEL_PARTITIONS, 0x07c00000);
	put32(cmd, SGX_XFER_FENCE, f->fence_id);
	return size;
}
