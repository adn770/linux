// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgxfive_owntask.c, built only with CONFIG_DRM_GMA500_SGXFIVE_DEBUG: the blob
 * parser and the default-deny verifier. The blobs are the ones tools/fwtask builds from
 * asm/fwtask (accepted) and crafted ones (refused, each with its reason), in the generated
 * sgxfive_owntask_blobs.h; tools/fwtask/test_verifier.py holds the same cases against the tool's
 * verifier, and test_host_verifier.py runs this file's verifier on the host against it.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_own_regs.h"
#include "sgxfive_owntask.h"
#include "sgxfive_owntask_blobs.h"

static int verify_data(const u8 *data, size_t len, const char **why)
{
	struct sgx_own_blob b;
	int ret = sgx_own_parse(data, len, &b);

	if (ret)
		return ret;
	return sgx_own_verify(&b, why);
}

static void sgxfive_owntask_test_accepts_our_blobs(struct kunit *test)
{
	const char *why = "";
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(own_ok); i++)
		KUNIT_EXPECT_EQ_MSG(test, verify_data(own_ok[i].data, own_ok[i].len, &why), 0,
				    "%s: %s", own_ok[i].name, why);
}

static void sgxfive_owntask_test_refuses_crafted_blobs(struct kunit *test)
{
	const char *why = "";
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(own_bad); i++) {
		why = "";
		KUNIT_EXPECT_EQ_MSG(test, verify_data(own_bad[i].data, own_bad[i].len, &why),
				    own_bad[i].err, "%s", own_bad[i].name);
		/* a blob refused for the wrong reason (mis-assembled, a bad terminator) is not a pass */
		KUNIT_EXPECT_STREQ_MSG(test, why, own_bad[i].why, "%s", own_bad[i].name);
	}
}

/* R3: no unnamed bit of the canonical words is ignored: every row pins at least its identity */
static void sgxfive_owntask_test_forms_are_canonical(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sgx_own_forms); i++) {
		KUNIT_EXPECT_EQ_MSG(test, sgx_own_forms[i].value & ~sgx_own_forms[i].mask, 0ULL,
				    "row %u", i);
		/* the first row that matches a row's own value is the row (none shadows it) */
		if (sgx_own_forms[i].kind != SGX_OWN_DENY) {
			unsigned int j;

			for (j = 0; j < i; j++)
				KUNIT_EXPECT_NE_MSG(test,
						    sgx_own_forms[i].value & sgx_own_forms[j].mask,
						    sgx_own_forms[j].value, "row %u shadowed by %u", i, j);
		}
	}
}

/* every clock-gate register, by name, for str and ldr alike */
static void sgxfive_owntask_test_clock_gate(struct kunit *test)
{
	unsigned int i;

	KUNIT_EXPECT_EQ(test, (int)ARRAY_SIZE(sgx_own_clockgate_regs), 4);
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, (unsigned int)sgx_own_clockgate_regs[i], i);
		KUNIT_EXPECT_TRUE(test, sgx_own_clockgate(i));
		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(i, true));
		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(i, false));
		/* a 14-bit index is masked like the hardware does */
		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(i + 0x4000, true));
		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(i + 0x8000, false));
	}
}

static void sgxfive_owntask_test_register_lists(struct kunit *test)
{
	unsigned int i;

	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0814 / 4, true));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0a78 / 4, true));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0814 / 4, false));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0a78 / 4, false));
	/* read only: CORE_ID, CORE_REVISION, CR_TIMER */
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(7, false));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(8, false));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0144 / 4, false));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(7, true));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(8, true));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x0144 / 4, true));
	/* not in this phase: tier 2, register 3 and the ones the plan kept out */
	for (i = 0; i < 7; i++) {
		static const u16 off[] = { 0x0520, 0x0810, 0x0818, 0x0abc, 0x0a7c, 0x0a80, 0x0800 };

		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(off[i] / 4, true));
		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(off[i] / 4, false));
	}
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(1000, false));	/* default deny */
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x3fff, true));
}

/* the deny rows: no offset of any range may be stored, and only CR_TIMER of them read */
static void sgxfive_owntask_test_deny_rows(struct kunit *test)
{
	unsigned int i, off;

	for (i = 0; i < ARRAY_SIZE(sgx_own_deny); i++) {
		for (off = sgx_own_deny[i][0]; off <= sgx_own_deny[i][1]; off += 4) {
			KUNIT_EXPECT_FALSE_MSG(test, sgx_own_reg_ok(off / 4, true), "str 0x%04x", off);
			if (off != 0x0144)
				KUNIT_EXPECT_FALSE_MSG(test, sgx_own_reg_ok(off / 4, false),
						       "ldr 0x%04x", off);
		}
	}
}

static void sgxfive_owntask_test_parse(struct kunit *test)
{
	struct sgx_own_blob b;
	u8 buf[16 + 8 + 2];
	static const u8 hdr[] = "SGXOWN01";

	memset(buf, 0, sizeof(buf));
	memcpy(buf, hdr, 8);
	buf[8] = 1;				/* one instruction, no patch, no entry */
	KUNIT_EXPECT_EQ(test, sgx_own_parse(buf, 16 + 8, &b), 0);
	KUNIT_EXPECT_EQ(test, (int)b.n_insn, 1);
	KUNIT_EXPECT_EQ(test, sgx_own_parse(buf, 16 + 8 + 1, &b), -EINVAL);	/* length */
	KUNIT_EXPECT_EQ(test, sgx_own_parse(buf, 15, &b), -EINVAL);
	buf[8] = 0;				/* no instruction */
	KUNIT_EXPECT_EQ(test, sgx_own_parse(buf, 16, &b), -EINVAL);
	buf[8] = 1;
	buf[12] = 5;				/* too many entries */
	KUNIT_EXPECT_EQ(test, sgx_own_parse(buf, 16 + 8 + 10, &b), -EINVAL);
}

static struct kunit_case sgxfive_owntask_test_cases[] = {
	KUNIT_CASE(sgxfive_owntask_test_accepts_our_blobs),
	KUNIT_CASE(sgxfive_owntask_test_refuses_crafted_blobs),
	KUNIT_CASE(sgxfive_owntask_test_forms_are_canonical),
	KUNIT_CASE(sgxfive_owntask_test_clock_gate),
	KUNIT_CASE(sgxfive_owntask_test_register_lists),
	KUNIT_CASE(sgxfive_owntask_test_deny_rows),
	KUNIT_CASE(sgxfive_owntask_test_parse),
	{}
};

static struct kunit_suite sgxfive_owntask_test_suite = {
	.name = "gma500_sgxfive_owntask",
	.test_cases = sgxfive_owntask_test_cases,
};
kunit_test_suite(sgxfive_owntask_test_suite);
