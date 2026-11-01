// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_xfer.c: the transfer command and the HW transfer
 * context as the builders lay them out (resgx545's doc/specs/transfer.md).
 * Addresses and values are made up; the offsets are the layout's.
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgx_xfer.h"

static u32 dw(const u8 *p, unsigned int off)
{
	return p[off] | p[off + 1] << 8 | p[off + 2] << 16 | (u32)p[off + 3] << 24;
}

static void sgx_xfer_test_dummy_cmd(struct kunit *test)
{
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int off;
	u32 size;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	size = sgx_xfer_dummy(cmd, 0x12340004, 0x1234, 0x12340000, 0xcafe, NULL);

	KUNIT_EXPECT_EQ(test, size, 0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_SIZE), 0x1c0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_FLAGS), (u32)SGX_XFER_FLAG_DUMMY);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_SRC), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_DST), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_STATUS), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x12340004u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS + 4), 0x1234u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_UPDATES), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_UPDATES), 0x12340000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_UPDATES + 4), 0xcafeu);
	/* Everything else is zero, including the register block. */
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4) {
		if (off == SGX_XFER_SIZE || off == SGX_XFER_FLAGS ||
		    off == SGX_XFER_NUM_STATUS || off == SGX_XFER_STATUS ||
		    off == SGX_XFER_STATUS + 4 || off == SGX_XFER_NUM_UPDATES ||
		    off == SGX_XFER_UPDATES || off == SGX_XFER_UPDATES + 4)
			continue;
		KUNIT_EXPECT_EQ_MSG(test, dw(cmd, off), 0u, "offset %#x", off);
	}
}

/* The destination sync: read pending, its counter, write pending, its counter. */
static void sgx_xfer_test_dst_sync(struct kunit *test)
{
	static const struct sgx_xfer_sync dst = {
		.read_pending = 3, .read_complete = 0x12340020,
		.write_pending = 6, .write_complete = 0x12340024,
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	sgx_xfer_dummy(cmd, 0x12340004, 0x1234, 0x12340000, 0xcafe, &dst);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_SRC), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_DST), 1u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_DST), 3u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_DST + 4), 0x12340020u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_DST + 8), 6u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_DST + 12), 0x12340024u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_NUM_STATUS), 1u);	/* kept */
}

static void sgx_xfer_test_ctx(struct kunit *test)
{
	u8 *ctx = kunit_kzalloc(test, SGX_XFER_CTX_SIZE, GFP_KERNEL);
	unsigned int off;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	memset(ctx, 0xa5, SGX_XFER_CTX_SIZE);
	sgx_xfer_ctx_init(ctx, 0x2468000, 0x32100000, 0x32120000);

	KUNIT_EXPECT_EQ(test, dw(ctx, SGX_XFER_CTX_FLAGS), 0u);
	KUNIT_EXPECT_EQ(test, dw(ctx, SGX_XFER_CTX_PRIORITY), 0u);
	KUNIT_EXPECT_EQ(test, dw(ctx, SGX_XFER_CTX_PD), 0x2468000u);
	KUNIT_EXPECT_EQ(test, dw(ctx, SGX_XFER_CTX_CCB), 0x32100000u);
	KUNIT_EXPECT_EQ(test, dw(ctx, SGX_XFER_CTX_CCB_CTL), 0x32120000u);
	for (off = SGX_XFER_CTX_COUNT; off < SGX_XFER_CTX_SIZE; off += 4)
		KUNIT_EXPECT_EQ_MSG(test, dw(ctx, off), 0u, "offset %#x", off);
}

/* Bits [off, off + n) of a dword array, least significant first. */
static u32 bits(const u32 *w, unsigned int off, unsigned int n)
{
	u64 v = w[off / 32] | (u64)w[off / 32 + 1] << 32;

	return (v >> (off % 32)) & ((1ull << n) - 1);
}

/* A vertex coordinate: 24-bit fixed point, 8 fraction bits, offset 4096. */
#define VTX(c)		(((c) + 4096) * 256)

/*
 * The three vertices from bit 0 of v, 80 bits each: Y, X (24 bits each),
 * then 32 zero bits. Top-left, top-right, bottom-left.
 */
static void expect_vertices(struct kunit *test, const u32 *v,
			    const struct sgx_xfer_rect *r)
{
	const u32 want[3][2] = {
		{ r->x0, r->y0 }, { r->x1, r->y0 }, { r->x0, r->y1 },
	};
	unsigned int i;

	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ_MSG(test, bits(v, 80 * i, 24), VTX(want[i][1]),
				    "vertex %u y", i);
		KUNIT_EXPECT_EQ_MSG(test, bits(v, 80 * i + 24, 24),
				    VTX(want[i][0]), "vertex %u x", i);
		KUNIT_EXPECT_EQ_MSG(test, bits(v, 80 * i + 48, 32), 0u,
				    "vertex %u z", i);
	}
}

/*
 * The background object of a fill: PDS state (offsets and data sizes in
 * 16-byte units, the primary's attribute count), padding, the vertices.
 */
static void sgx_xfer_test_bgobj(struct kunit *test)
{
	static const struct sgx_xfer_rect r = { 8, 4, 40, 36 };
	u32 bg[SGX_XFER_BGOBJ_DWORDS + 1];
	unsigned int i;

	memset(bg, 0xa5, sizeof(bg));
	bg[SGX_XFER_BGOBJ_DWORDS] = 0;		/* bits() reads one dword ahead */
	sgx_xfer_bgobj(bg, 0x2e000000, 0x2e000100, 0x20, 0x2e000040, 0x10, &r);
	KUNIT_EXPECT_EQ(test, bg[0] & 0x03ffffff, 0x40u >> 4);	/* secondary */
	KUNIT_EXPECT_EQ(test, bg[0] >> 26, 0x10u >> 4);
	KUNIT_EXPECT_EQ(test, bg[1] & 0xffff, 0u);		/* no attribute */
	KUNIT_EXPECT_EQ(test, bg[1] >> 16, 3u);
	KUNIT_EXPECT_EQ(test, bg[2] & 0x03ffffff, 0x100u >> 4);	/* primary */
	KUNIT_EXPECT_EQ(test, bg[2] >> 26, 0x20u >> 4);
	for (i = 3; i < 9; i++)
		KUNIT_EXPECT_EQ_MSG(test, bg[i], 0u, "dword %u", i);
	expect_vertices(test, &bg[9], &r);
	KUNIT_EXPECT_EQ(test, bg[16], 0u);
}

/*
 * A fill's command: every word it sets, by field; the rest stays zero. The
 * object is 0x30000 into its request window.
 */
static void sgx_xfer_test_fill_cmd(struct kunit *test)
{
	static const struct sgx_xfer_fill f = {
		.rect = { 0, 0, 64, 32 }, .bg = 0x32130000, .req_base = 0x32100000,
		.pixev_exec = 0x2e000048, .pds_exec_base = 0x2e000000,
		.status_addr = 0x32122004, .fence_id = 7,
	};
	static const u32 want[][2] = {
		{ SGX_XFER_BG_TAG, 0x30000 >> 4 },
		{ SGX_XFER_BG_OBJ, 0x200 },
		{ SGX_XFER_BG_DEPTH, 0x3f800000 },		/* 1.0f */
		{ SGX_XFER_RENDER, 3 },
		{ SGX_XFER_IPF_MISC, 0x100 },
		{ SGX_XFER_REQ_BASE, 0x32100000 },
		{ SGX_XFER_REG_01C, 0x80000000 },
		{ SGX_XFER_MULTISAMP, 0x88 },
		{ SGX_XFER_PIXEV_CODE, 0x2e000040 },		/* 16-byte aligned */
		{ SGX_XFER_PIXEV_DATA, 0x50 >> 4 },
		{ SGX_XFER_PIXEV_INFO, 1 },
		{ SGX_XFER_BOX_MAX, 1 | 1 << 16 },		/* tiles 0..1 x 0..1 */
		{ SGX_XFER_REG_038, 0x80 },
		{ SGX_XFER_PDS_EXEC_BASE, 0x2e000000 },
		{ SGX_XFER_SIZE, 0x1c0 },
		{ SGX_XFER_NUM_STATUS, 1 },
		{ SGX_XFER_STATUS, 0x32122004 },
		{ SGX_XFER_STATUS + 4, 7 + 1 },
		{ SGX_XFER_PIXEL_PARTITIONS, 31 << 22 },
		{ SGX_XFER_FENCE, 7 },
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	unsigned int i, off, n = 0;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	memset(cmd, 0xa5, SGX_XFER_CMD_SIZE);
	KUNIT_EXPECT_EQ(test, sgx_xfer_fill_cmd(cmd, &f), 0x1c0u);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(cmd, want[i][0]), want[i][1],
				    "offset %#x", want[i][0]);
	for (off = 0; off < SGX_XFER_CMD_SIZE; off += 4)
		n += dw(cmd, off) != 0;
	KUNIT_EXPECT_EQ(test, n, (unsigned int)ARRAY_SIZE(want));	/* rest 0 */
}

/* The render box in 32 x 16 tiles, inclusive: y | x << 16. */
static void sgx_xfer_test_render_box(struct kunit *test)
{
	static const struct sgx_xfer_fill f = {
		.rect = { 40, 20, 100, 33 },
	};
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	sgx_xfer_fill_cmd(cmd, &f);
	/* from y 20 / 16 = 1, x 40 / 32 = 1 */
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_BOX_MIN), 1u | 1u << 16);
	/* to y (33 + 15) / 16 - 1 = 2, x (100 + 31) / 32 - 1 = 3 */
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_BOX_MAX), 2u | 3u << 16);
}

static struct kunit_case sgx_xfer_test_cases[] = {
	KUNIT_CASE(sgx_xfer_test_dummy_cmd),
	KUNIT_CASE(sgx_xfer_test_dst_sync),
	KUNIT_CASE(sgx_xfer_test_ctx),
	KUNIT_CASE(sgx_xfer_test_bgobj),
	KUNIT_CASE(sgx_xfer_test_fill_cmd),
	KUNIT_CASE(sgx_xfer_test_render_box),
	{}
};

static struct kunit_suite sgx_xfer_test_suite = {
	.name = "gma500_sgx_xfer",
	.test_cases = sgx_xfer_test_cases,
};

kunit_test_suite(sgx_xfer_test_suite);
