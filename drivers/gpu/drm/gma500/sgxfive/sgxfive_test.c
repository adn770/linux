// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgxfive.c's paths that need no device: what they refuse
 * before the firmware runs, and how the firmware's identity reply is taken.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgxfive_priv.h"

/*
 * A power command needs the firmware running: before the boot there is no
 * HostCtl buffer, so IDLE and POWEROFF must fail, not write through NULL.
 */
static void sgxfive_test_power_needs_live(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_power(sgx, POWERCMD_IDLE,
					    POWMAN_IDLE_COMPLETE), -ENODEV);
	KUNIT_EXPECT_EQ(test, sgxfive_power(sgx, POWERCMD_POWEROFF,
					    POWMAN_POWEROFF_COMPLETE), -ENODEV);
}

/* So must any kernel CCB command. */
static void sgxfive_test_command_needs_live(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_command(sgx, SGX_CMD_GETMISCINFO, NULL, 0),
			-ENODEV);
}

/*
 * A render context's cleanup asks the firmware to drop its RT data set, its
 * context and its PB, in that order; a zero PB descriptor (a PB shared with a
 * context that stays) sends no PB command.
 */
static void sgxfive_test_cleanup_steps(struct kunit *test)
{
	struct sgxfive_cleanup_step s[SGXFIVE_CLEANUP_CMDS];

	memset(s, 0xff, sizeof(s));
	KUNIT_EXPECT_EQ(test, sgxfive_render_cleanup_steps(0x10, 0x20, 0x30, s), 3u);
	KUNIT_EXPECT_EQ(test, s[0].type, (u32)CLEANUP_RT);
	KUNIT_EXPECT_EQ(test, s[0].addr, 0x10u);
	KUNIT_EXPECT_EQ(test, s[1].type, (u32)CLEANUP_RC);
	KUNIT_EXPECT_EQ(test, s[1].addr, 0x20u);
	KUNIT_EXPECT_EQ(test, s[2].type, (u32)CLEANUP_PB);
	KUNIT_EXPECT_EQ(test, s[2].addr, 0x30u);
	memset(s, 0xff, sizeof(s));
	KUNIT_EXPECT_EQ(test, sgxfive_render_cleanup_steps(0x10, 0x20, 0, s), 2u);
	KUNIT_EXPECT_EQ(test, s[0].type, (u32)CLEANUP_RT);
	KUNIT_EXPECT_EQ(test, s[1].type, (u32)CLEANUP_RC);
	KUNIT_EXPECT_EQ(test, s[2].type, 0xffffffffu);	/* no third command */
}

/* A device that read these from its registers, and a reply that agrees. */
#define TEST_CORE_ID	0x01150000
#define TEST_CORE_REV	0x00010001

static struct sgxfive *ident_dev(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	sgx->core_id = TEST_CORE_ID;
	sgx->core_revision = TEST_CORE_REV;
	return sgx;
}

static void ident_reply(u32 *mi)
{
	memset(mi, 0, MISCINFO_DWORDS * sizeof(*mi));
	mi[0] = MISCINFO_READY;
	mi[MISCINFO_CORE_REV] = TEST_CORE_REV;
	mi[MISCINFO_CORE_ID] = TEST_CORE_ID;
	mi[MISCINFO_DDK_VERSION] = 0x00010711;
	mi[MISCINFO_DDK_BUILD] = 862890;
	mi[MISCINFO_CORE_ID_SW] = 545;
	mi[MISCINFO_CORE_REV_SW] = TEST_CORE_REV;
}

/*
 * An answer is the firmware's identity: DEV_QUERY's own values and flag, in
 * every build, and no warning for a reply that names the registers' core.
 */
static void sgxfive_test_ident_answer(struct kunit *test)
{
	struct sgxfive *sgx = ident_dev(test);
	u32 mi[MISCINFO_DWORDS];

	ident_reply(mi);
	KUNIT_EXPECT_FALSE(test, sgx->sw_valid);
	KUNIT_EXPECT_TRUE(test, sgxfive_ident_core_ok(sgx, mi));
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, 0, mi), 0u);
	KUNIT_EXPECT_TRUE(test, sgx->sw_valid);
	KUNIT_EXPECT_EQ(test, sgx->core_id_sw, 545u);
	KUNIT_EXPECT_EQ(test, sgx->core_revision_sw, (u32)TEST_CORE_REV);
	KUNIT_EXPECT_EQ(test, sgx->ddk_version, 0x00010711u);
	KUNIT_EXPECT_FALSE(test, sgx->ident_warned);
}

/*
 * A reply for another core than the registers name, on either side of it
 * (the core it runs on, the one it was built for) is kept and warned about
 * once per reply.
 */
static void sgxfive_test_ident_core_mismatch(struct kunit *test)
{
	static const unsigned int field[] = {
		MISCINFO_CORE_REV, MISCINFO_CORE_ID, MISCINFO_CORE_ID_SW, MISCINFO_CORE_REV_SW,
	};
	struct sgxfive *sgx = ident_dev(test);
	u32 mi[MISCINFO_DWORDS];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(field); i++) {
		ident_reply(mi);
		mi[field[i]] ^= 0x100;
		KUNIT_EXPECT_FALSE_MSG(test, sgxfive_ident_core_ok(sgx, mi), "dword %u", field[i]);
		sgx->sw_valid = false;
		KUNIT_EXPECT_EQ_MSG(test, sgxfive_ident_result(sgx, 0, mi), 1u, "dword %u",
				    field[i]);
		KUNIT_EXPECT_TRUE(test, sgx->sw_valid);
	}
	/* The DDK values are not part of the core check. */
	ident_reply(mi);
	mi[MISCINFO_DDK_VERSION] ^= 0xff;
	mi[MISCINFO_DDK_BUILD] ^= 0xff;
	mi[MISCINFO_BUILD_OPTIONS] ^= 0xff;
	KUNIT_EXPECT_TRUE(test, sgxfive_ident_core_ok(sgx, mi));
}

/*
 * No answer leaves SW_VALID clear, even after an earlier answer, and logs
 * once until the EDM answers again.
 */
static void sgxfive_test_ident_no_answer(struct kunit *test)
{
	struct sgxfive *sgx = ident_dev(test);
	u32 mi[MISCINFO_DWORDS];

	ident_reply(mi);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, 0, mi), 0u);
	KUNIT_ASSERT_TRUE(test, sgx->sw_valid);
	memset(mi, 0, sizeof(mi));
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, -ETIMEDOUT, mi), 1u);
	KUNIT_EXPECT_FALSE(test, sgx->sw_valid);
	KUNIT_EXPECT_TRUE(test, sgx->ident_warned);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, -ETIMEDOUT, mi), 0u);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, -EINVAL, mi), 0u);
	KUNIT_EXPECT_FALSE(test, sgx->sw_valid);
	/* An answer clears it, and the next miss is logged again. */
	ident_reply(mi);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, 0, mi), 0u);
	KUNIT_EXPECT_TRUE(test, sgx->sw_valid);
	KUNIT_EXPECT_FALSE(test, sgx->ident_warned);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, -ETIMEDOUT, mi), 1u);
}

/*
 * The firmware stopping (a reset, a power-off, a boot that failed) clears the
 * identity: sgxfive_halt calls sgxfive_ident_clear, which leaves nothing
 * valid until the next answer, and the values themselves are not reported.
 */
static void sgxfive_test_ident_clear(struct kunit *test)
{
	struct sgxfive *sgx = ident_dev(test);
	u32 mi[MISCINFO_DWORDS];

	ident_reply(mi);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, 0, mi), 0u);
	KUNIT_ASSERT_TRUE(test, sgx->sw_valid);
	sgxfive_ident_clear(sgx);
	KUNIT_EXPECT_FALSE(test, sgx->sw_valid);
	KUNIT_EXPECT_EQ(test, sgxfive_ddk_version(sgx), 0u);
	/* Clearing is not a missed answer: the next miss is still the first. */
	KUNIT_EXPECT_FALSE(test, sgx->ident_warned);
	KUNIT_EXPECT_EQ(test, sgxfive_ident_result(sgx, 0, mi), 0u);
	KUNIT_EXPECT_TRUE(test, sgx->sw_valid);
	KUNIT_EXPECT_EQ(test, sgxfive_ddk_version(sgx), 0x00010711u);
}

/* The request needs the firmware running, and leaves what was answered alone. */
static void sgxfive_test_ident_needs_live(struct kunit *test)
{
	struct sgxfive *sgx = ident_dev(test);
	s64 us = 7;

	KUNIT_EXPECT_EQ(test, sgxfive_ident_ask(sgx, &us), -ENODEV);
	KUNIT_EXPECT_EQ(test, us, 7);
	KUNIT_EXPECT_FALSE(test, sgx->ident_warned);
	sgx->sw_valid = true;
	KUNIT_EXPECT_EQ(test, sgxfive_ident_ask(sgx, NULL), -ENODEV);
	KUNIT_EXPECT_TRUE(test, sgx->sw_valid);
}

static struct kunit_case sgxfive_test_cases[] = {
	KUNIT_CASE(sgxfive_test_ident_answer),
	KUNIT_CASE(sgxfive_test_ident_core_mismatch),
	KUNIT_CASE(sgxfive_test_ident_no_answer),
	KUNIT_CASE(sgxfive_test_ident_clear),
	KUNIT_CASE(sgxfive_test_ident_needs_live),
	KUNIT_CASE(sgxfive_test_power_needs_live),
	KUNIT_CASE(sgxfive_test_command_needs_live),
	KUNIT_CASE(sgxfive_test_cleanup_steps),
	{}
};

static struct kunit_suite sgxfive_test_suite = {
	.name = "gma500_sgxfive",
	.test_cases = sgxfive_test_cases,
};

kunit_test_suite(sgxfive_test_suite);
