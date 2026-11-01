// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the kernel pokes of the debug group (sgxfive_kpoke.c):
 * parsing ta:N, rd:N and tq:N with = and ^=, the range of each space, arming
 * and the one-shot application to a transfer command and a render image
 * built by the driver's own builders, the debugfs text, the live guards and
 * the register allow-list.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "sgx_render.h"
#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_kpoke.h"
#include "sgxfive_priv.h"
#include "sgxfive_regdump.h"

static u32 dw(const u8 *p, unsigned int off)
{
	return get_unaligned_le32(p + off);
}

static void sgxfive_kpoke_test_words(struct kunit *test)
{
	/* The whole of each structure the builders write. */
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_TA),
			(u32)SGX_RENDER_CMD_STRIDE / 4);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_RD),
			(u32)SGX_RENDER_RD_SIZE / 4);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_TQ),
			(u32)SGX_XFER_CMD_SIZE / 4);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_TA), 176u);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_RD), 123u);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_TQ), 306u);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_words(SGXFIVE_KPOKE_SPACES), 0u);
	/* The render details follow the TA command in a render image. */
	KUNIT_EXPECT_GE(test, SGXFIVE_RENDER_IMG_RD, SGX_RENDER_CMD_STRIDE);
}

static void sgxfive_kpoke_test_parse(struct kunit *test)
{
	static const char *const bad[] = {
		"", "ta", "ta:", "ta:1", "ta:1=", "ta:=1", "ta:x=1", "ta:1=x",
		"ta:1~=1", "ta:1=0x100000000", "frob:1=1", "TA:1=1", "ta:1==1",
		"ta:1^1", "ta:^=1", "ta:1:2=3", "cmd:1=1", "ta:-1=1",
		"ta:1=0x1234567890123456789012345678901234567890",
	};
	struct sgxfive_kpoke p;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("ta:12=0x10", &p), 0);
	KUNIT_EXPECT_EQ(test, p.space, (u32)SGXFIVE_KPOKE_TA);
	KUNIT_EXPECT_EQ(test, p.index, 12u);
	KUNIT_EXPECT_EQ(test, p.value, 0x10u);
	KUNIT_EXPECT_FALSE(test, p.flip);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("rd:0x7a^=0x80000000", &p), 0);
	KUNIT_EXPECT_EQ(test, p.space, (u32)SGXFIVE_KPOKE_RD);
	KUNIT_EXPECT_EQ(test, p.index, 0x7au);
	KUNIT_EXPECT_EQ(test, p.value, 0x80000000u);
	KUNIT_EXPECT_TRUE(test, p.flip);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("tq:305=4294967295", &p), 0);
	KUNIT_EXPECT_EQ(test, p.space, (u32)SGXFIVE_KPOKE_TQ);
	KUNIT_EXPECT_EQ(test, p.index, 305u);
	KUNIT_EXPECT_EQ(test, p.value, 0xffffffffu);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_kpoke_parse(bad[i], &p), -EINVAL,
				    "\"%s\"", bad[i]);
}

/* An index past its structure is -ERANGE, the last word is fine. */
static void sgxfive_kpoke_test_range(struct kunit *test)
{
	struct sgxfive_kpoke p;

	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("ta:175=1", &p), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("ta:176=1", &p), -ERANGE);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("rd:122=1", &p), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("rd:123=1", &p), -ERANGE);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("tq:305=1", &p), 0);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("tq:306=1", &p), -ERANGE);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse("tq:0xffffffff=1", &p), -ERANGE);
}

static void sgxfive_kpoke_test_format(struct kunit *test)
{
	const struct sgxfive_kpoke a = { SGXFIVE_KPOKE_RD, 7, 0x10, true };
	const struct sgxfive_kpoke b = { SGXFIVE_KPOKE_TQ, 305, 0xffffffff, false };
	char line[SGXFIVE_KPOKE_LINE];
	struct sgxfive_kpoke p;

	sgxfive_kpoke_format(&a, line, sizeof(line));
	KUNIT_EXPECT_STREQ(test, line, "rd:7^=0x00000010");
	sgxfive_kpoke_format(&b, line, sizeof(line));
	KUNIT_EXPECT_STREQ(test, line, "tq:305=0xffffffff");
	/* What the file shows parses back. */
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_parse(line, &p), 0);
	KUNIT_EXPECT_EQ(test, p.space, b.space);
	KUNIT_EXPECT_EQ(test, p.index, b.index);
	KUNIT_EXPECT_EQ(test, p.value, b.value);
	KUNIT_EXPECT_EQ(test, p.flip, b.flip);
}

/* Arming takes all of a write or none of it. */
static void sgxfive_kpoke_test_arm(struct kunit *test)
{
	struct sgxfive_kpokes *k = kunit_kzalloc(test, sizeof(*k), GFP_KERNEL);
	struct sgxfive_kpoke p[SGXFIVE_KPOKE_MAX] = { };
	const struct sgxfive_kpoke past = { SGXFIVE_KPOKE_RD, 123, 1, false };
	const struct sgxfive_kpoke nospace = { SGXFIVE_KPOKE_SPACES, 0, 1, false };

	KUNIT_ASSERT_NOT_NULL(test, k);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_arm(k, p, 3), 0);
	KUNIT_EXPECT_EQ(test, k->n, 3u);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_arm(k, p, SGXFIVE_KPOKE_MAX - 2), -ENOSPC);
	KUNIT_EXPECT_EQ(test, k->n, 3u);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_arm(k, p, SGXFIVE_KPOKE_MAX - 3), 0);
	KUNIT_EXPECT_EQ(test, k->n, (unsigned int)SGXFIVE_KPOKE_MAX);
	k->n = 0;
	p[1] = past;
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_arm(k, p, 2), -ERANGE);
	p[1] = nospace;
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_arm(k, p, 2), -ERANGE);
	KUNIT_EXPECT_EQ(test, k->n, 0u);
}

/*
 * A transfer command built as the render node builds it, the pokes applied
 * once: tq ones to a transfer, in order, then dropped; ta and rd ones stay
 * armed for the next render.
 */
static void sgxfive_kpoke_test_transfer(struct kunit *test)
{
	struct sgxfive_kpokes *k = kunit_kzalloc(test, sizeof(*k), GFP_KERNEL);
	struct drm_sgxfive_transfer_cmd *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	u8 *cmd = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	u8 *want = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	struct sgxfive_kpoke p[4], done[SGXFIVE_KPOKE_MAX];

	KUNIT_ASSERT_NOT_NULL(test, k);
	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_ASSERT_NOT_NULL(test, want);
	c->isp_bgobjtag = 0x3000;
	c->bif_3d_req_base = 0x3e100000;
	c->pds_exec_base = 0x2e000000;
	sgxfive_job_build(cmd, c, false, 0x32400200, 7);
	memcpy(want, cmd, SGX_XFER_CMD_SIZE);

	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("tq:0^=0x1", &p[0]), 0);
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("ta:1=5", &p[1]), 0);
	/* SGX_XFER_STATUS: the fence address, a kernel pointer, poked on purpose */
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("tq:90=0x12345678", &p[2]), 0);
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("tq:0^=0x100", &p[3]), 0);
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_arm(k, p, 4), 0);

	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_take(k, cmd, false, done), 3u);
	put_unaligned_le32(0x3000 ^ 0x101, want);
	put_unaligned_le32(0x12345678, want + 4 * 90);
	KUNIT_EXPECT_EQ(test, 4 * 90, SGX_XFER_STATUS);
	KUNIT_EXPECT_EQ(test, memcmp(cmd, want, SGX_XFER_CMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test, done[0].index, 0u);
	KUNIT_EXPECT_EQ(test, done[1].index, 90u);
	KUNIT_EXPECT_EQ(test, done[2].value, 0x100u);
	/* Only the render poke is left. */
	KUNIT_EXPECT_EQ(test, k->n, 1u);
	KUNIT_EXPECT_EQ(test, k->p[0].space, (u32)SGXFIVE_KPOKE_TA);
	/* Once: the next transfer is the builder's. */
	sgxfive_job_build(cmd, c, false, 0x32400200, 8);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_take(k, cmd, false, done), 0u);
	KUNIT_EXPECT_EQ(test, dw(cmd, 0), 0x3000u);
	KUNIT_EXPECT_EQ(test, dw(cmd, SGX_XFER_STATUS), 0x32400200u);
}

/* A render image: ta words from its start, rd words in its render details. */
static void sgxfive_kpoke_test_render(struct kunit *test)
{
	struct sgxfive_kpokes *k = kunit_kzalloc(test, sizeof(*k), GFP_KERNEL);
	struct drm_sgxfive_render_cmd *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	u8 *img = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	u8 *want = kunit_kzalloc(test, SGX_XFER_CMD_SIZE, GFP_KERNEL);
	struct sgxfive_kpoke p[4], done[SGXFIVE_KPOKE_MAX];
	struct sgx_render_addrs a;
	struct sgx_render_tadata td;

	KUNIT_ASSERT_NOT_NULL(test, k);
	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_NOT_NULL(test, img);
	KUNIT_ASSERT_NOT_NULL(test, want);
	c->mte_screen = 0x003f003f;
	c->pixelbe = 0x80;
	KUNIT_ASSERT_EQ(test, sgx_render_tadata_layout(&td, 64, 64), 0);
	sgxfive_render_addrs(&a, 0x32500000, 0x01234000, 0, &td);
	sgxfive_render_build(img, &a, c, 9, true, true);
	memcpy(want, img, SGX_XFER_CMD_SIZE);

	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("ta:8^=0x100", &p[0]), 0);	/* frame */
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("rd:2=0x55", &p[1]), 0);	/* pixelbe */
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("tq:2=1", &p[2]), 0);
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_parse("rd:122^=0xffffffff", &p[3]), 0);
	KUNIT_ASSERT_EQ(test, sgxfive_kpoke_arm(k, p, 4), 0);

	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_take(k, img, true, done), 3u);
	KUNIT_EXPECT_EQ(test, dw(img, SGX_RENDER_CMD_FRAME), 9u ^ 0x100);
	KUNIT_EXPECT_EQ(test, dw(img, SGXFIVE_RENDER_IMG_RD + SGX_RENDER_RD_PIXELBE), 0x55u);
	KUNIT_EXPECT_EQ(test, dw(img, SGXFIVE_RENDER_IMG_RD + 4 * 122), 0xffffffffu);
	put_unaligned_le32(9u ^ 0x100, want + SGX_RENDER_CMD_FRAME);
	put_unaligned_le32(0x55, want + SGXFIVE_RENDER_IMG_RD + SGX_RENDER_RD_PIXELBE);
	put_unaligned_le32(0xffffffff, want + SGXFIVE_RENDER_IMG_RD + 4 * 122);
	KUNIT_EXPECT_EQ(test, memcmp(img, want, SGX_XFER_CMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test, k->n, 1u);
	KUNIT_EXPECT_EQ(test, k->p[0].space, (u32)SGXFIVE_KPOKE_TQ);
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_take(k, img, true, done), 0u);
}

/*
 * The debugfs file: a write arms its lines (or none, on any error), "clear"
 * drops what is armed; arming needs the firmware running, clearing does not.
 */
static void sgxfive_kpoke_test_write(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	char buf[96];

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	spin_lock_init(&sgx->kpoke_lock);
	strscpy(buf, "ta:1=2\n", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), -ENODEV);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.n, 0u);
	strscpy(buf, "clear\n", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), 0);

	sgx->live = true;
	strscpy(buf, "ta:1=2\nrd:3^=0x4\n\n", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), 0);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.n, 2u);
	strscpy(buf, "tq:5=1\nrd:123=1\n", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), -ERANGE);
	strscpy(buf, "tq:5=1\nfrob\n", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.n, 2u);
	strscpy(buf, "clear\ntq:5=1\n", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), 0);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.n, 1u);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.p[0].space, (u32)SGXFIVE_KPOKE_TQ);
	strscpy(buf, "clear", sizeof(buf));
	KUNIT_EXPECT_EQ(test, sgxfive_kpoke_write(sgx, buf), 0);
	KUNIT_EXPECT_EQ(test, sgx->kpokes.n, 0u);
}

/*
 * The registers a tester may write: sorted, dword aligned. Those the firmware
 * never reaches are none of the registers the snapshot lists (the firmware's,
 * the host driver's); those only a 3D render writes (its render details load
 * them; the transfer kick does not) are among them.
 */
static void sgxfive_kpoke_test_kreg_allowed(struct kunit *test)
{
	static const u32 never[] = { 0x0810, 0x0814, 0x0818, 0x0a78, 0x0abc };
	static const u32 render[] = { 0x0418, 0x04dc, 0x04e8, 0x0520, 0x0800 };
	static const u32 want[] = {
		0x0418, 0x04dc, 0x04e8, 0x0520, 0x0800,
		0x0810, 0x0814, 0x0818, 0x0a78, 0x0abc,
	};
	unsigned int i, j;
	bool listed;

	KUNIT_ASSERT_EQ(test, ARRAY_SIZE(want), (size_t)SGXFIVE_KREG_COUNT);
	for (i = 0; i < SGXFIVE_KREG_COUNT; i++) {
		KUNIT_EXPECT_EQ(test, sgxfive_kreg_allowed[i], want[i]);
		KUNIT_EXPECT_TRUE(test, sgxfive_kreg_ok(want[i]));
	}
	for (i = 0; i < ARRAY_SIZE(never); i++)
		for (j = 0; j < SGXFIVE_REGDUMP_NREGS; j++)
			KUNIT_EXPECT_NE(test, (u32)sgxfive_regdump_regs[j].off, never[i]);
	for (i = 0; i < ARRAY_SIZE(render); i++) {
		listed = false;
		for (j = 0; j < SGXFIVE_REGDUMP_NREGS; j++)
			listed |= sgxfive_regdump_regs[j].off == render[i];
		KUNIT_EXPECT_TRUE(test, listed);
	}
	KUNIT_EXPECT_FALSE(test, sgxfive_kreg_ok(0x0000));
	KUNIT_EXPECT_FALSE(test, sgxfive_kreg_ok(0x080c));
	KUNIT_EXPECT_FALSE(test, sgxfive_kreg_ok(0x0812));
	KUNIT_EXPECT_FALSE(test, sgxfive_kreg_ok(0x081c));
	KUNIT_EXPECT_FALSE(test, sgxfive_kreg_ok(0x0ab8));	/* PDS_EXEC_BASE */
	KUNIT_EXPECT_FALSE(test, sgxfive_kreg_ok(0x0a78 + 0x4000));
}

static void sgxfive_kpoke_test_kreg_parse(struct kunit *test)
{
	static const char *const bad[] = {
		"", "0x0810", "0x0810=", "=1", "0x0810~=1", "0x0810=0x100000000",
		"reg:0x0810=1", "0x0810==1",
	};
	u32 off, val;
	bool flip;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sgxfive_kreg_parse("0x0a78=0x2c", &off, &flip, &val), 0);
	KUNIT_EXPECT_EQ(test, off, 0x0a78u);
	KUNIT_EXPECT_EQ(test, val, 0x2cu);
	KUNIT_EXPECT_FALSE(test, flip);
	KUNIT_EXPECT_EQ(test, sgxfive_kreg_parse("0x0810^=0x80000000", &off, &flip, &val), 0);
	KUNIT_EXPECT_EQ(test, off, 0x0810u);
	KUNIT_EXPECT_EQ(test, val, 0x80000000u);
	KUNIT_EXPECT_TRUE(test, flip);
	/* Off the allow-list. */
	KUNIT_EXPECT_EQ(test, sgxfive_kreg_parse("0x0ab8=1", &off, &flip, &val), -EPERM);
	KUNIT_EXPECT_EQ(test, sgxfive_kreg_parse("0x0812=1", &off, &flip, &val), -EPERM);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_kreg_parse(bad[i], &off, &flip, &val),
				    -EINVAL, "\"%s\"", bad[i]);
}

/* No register write before the firmware runs. */
static void sgxfive_kpoke_test_kreg_needs_live(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_kreg_write(sgx, 0x0810, false, 1), -ENODEV);
	KUNIT_EXPECT_EQ(test, sgxfive_kreg_write(sgx, 0x0a78, true, 1), -ENODEV);
}

/* debugfs hwperf: the registers the firmware's HWPerf routine reads, and its control. */
static void sgxfive_kpoke_test_hwperf_regs(struct kunit *test)
{
	unsigned int i;
	u32 off;

	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_regs[0], 0x0040u);	/* the control register */
	for (off = 0x0044; off <= 0x0064; off += 4)
		KUNIT_EXPECT_EQ(test, sgxfive_hwperf_regs[1 + (off - 0x0044) / 4], off);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_regs[10], 0x0e84u);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_regs[13], 0x0e90u);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_regs[14], 0x0eb4u);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_regs[SGXFIVE_HWPERF_NREGS - 1], 0x0ec0u);
	for (i = 1; i < SGXFIVE_HWPERF_NREGS; i++) {
		KUNIT_EXPECT_LT(test, sgxfive_hwperf_regs[i - 1], sgxfive_hwperf_regs[i]);
		/* Never the soft reset nor the pulse, whose write starts something. */
		KUNIT_EXPECT_NE(test, sgxfive_hwperf_regs[i], 0x0080u);
		KUNIT_EXPECT_NE(test, sgxfive_hwperf_regs[i], 0x0e80u);
	}
}

static void sgxfive_kpoke_test_hwperf_parse(struct kunit *test)
{
	static const char *const bad[] = {
		"", "5", "5 ", "x 0x10", "5 y", "5 0x10 1", "-1 0x10",
	};
	u32 group, word;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_parse("5 0x11", &group, &word), 0);
	KUNIT_EXPECT_EQ(test, group, 5u);
	KUNIT_EXPECT_EQ(test, word, 0x11u);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_parse("0  1", &group, &word), 0);
	KUNIT_EXPECT_EQ(test, group, 0u);
	KUNIT_EXPECT_EQ(test, word, 1u);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_parse("15 0xffffffff", &group, &word), 0);
	/* The firmware keeps four bits of the group (control bits 12:9). */
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_parse("16 0x11", &group, &word), -ERANGE);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_hwperf_parse(bad[i], &group, &word),
				    -EINVAL, "\"%s\"", bad[i]);
}

/* No command before the firmware runs. */
static void sgxfive_kpoke_test_hwperf_needs_live(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_hwperf_set(sgx, 5, 0x11), -ENODEV);
}

static struct kunit_case sgxfive_kpoke_test_cases[] = {
	KUNIT_CASE(sgxfive_kpoke_test_words),
	KUNIT_CASE(sgxfive_kpoke_test_parse),
	KUNIT_CASE(sgxfive_kpoke_test_range),
	KUNIT_CASE(sgxfive_kpoke_test_format),
	KUNIT_CASE(sgxfive_kpoke_test_arm),
	KUNIT_CASE(sgxfive_kpoke_test_transfer),
	KUNIT_CASE(sgxfive_kpoke_test_render),
	KUNIT_CASE(sgxfive_kpoke_test_write),
	KUNIT_CASE(sgxfive_kpoke_test_kreg_allowed),
	KUNIT_CASE(sgxfive_kpoke_test_kreg_parse),
	KUNIT_CASE(sgxfive_kpoke_test_kreg_needs_live),
	KUNIT_CASE(sgxfive_kpoke_test_hwperf_regs),
	KUNIT_CASE(sgxfive_kpoke_test_hwperf_parse),
	KUNIT_CASE(sgxfive_kpoke_test_hwperf_needs_live),
	{}
};

static struct kunit_suite sgxfive_kpoke_test_suite = {
	.name = "gma500_sgxfive_kpoke",
	.test_cases = sgxfive_kpoke_test_cases,
};

kunit_test_suite(sgxfive_kpoke_test_suite);
