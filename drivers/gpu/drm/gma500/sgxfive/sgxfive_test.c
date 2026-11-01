// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgxfive.c's paths that need no device: what they refuse
 * before the firmware runs.
 */
#include <kunit/test.h>
#include <linux/module.h>

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

static struct kunit_case sgxfive_test_cases[] = {
	KUNIT_CASE(sgxfive_test_power_needs_live),
	KUNIT_CASE(sgxfive_test_command_needs_live),
	{}
};

static struct kunit_suite sgxfive_test_suite = {
	.name = "gma500_sgxfive",
	.test_cases = sgxfive_test_cases,
};

kunit_test_suite(sgxfive_test_suite);
