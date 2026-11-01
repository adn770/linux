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

static struct kunit_case sgx_xfer_test_cases[] = {
	KUNIT_CASE(sgx_xfer_test_dummy_cmd),
	KUNIT_CASE(sgx_xfer_test_dst_sync),
	KUNIT_CASE(sgx_xfer_test_ctx),
	{}
};

static struct kunit_suite sgx_xfer_test_suite = {
	.name = "gma500_sgx_xfer",
	.test_cases = sgx_xfer_test_cases,
};

kunit_test_suite(sgx_xfer_test_suite);
