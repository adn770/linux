// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the pure parts of sgxfive contexts and jobs: the arguments
 * of a new context.
 */
#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

/*
 * CREATE_CONTEXT's arguments: a known type, no flags, the one priority, zero
 * padding; a transfer context leaves both target sizes at 0.
 */
static void sgxfive_job_test_ctx_args(struct kunit *test)
{
	const struct drm_sgxfive_create_context_args xfer = {
		.type = DRM_SGXFIVE_CTX_TYPE_TRANSFER, .vm_context_handle = 1,
	};
	struct drm_sgxfive_create_context_args a;

	KUNIT_EXPECT_EQ(test, sizeof(a), (size_t)32);
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&xfer), 0);
	a = xfer;
	a.max_width = 64;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.max_height = 64;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a._padding_1c = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.type = 2;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.flags = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	a = xfer;
	a.priority = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
	/* Render contexts are not supported yet. */
	a = xfer;
	a.type = 1;
	KUNIT_EXPECT_EQ(test, sgxfive_ctx_check_args(&a), -EINVAL);
}

static struct kunit_case sgxfive_job_test_cases[] = {
	KUNIT_CASE(sgxfive_job_test_ctx_args),
	{}
};

static struct kunit_suite sgxfive_job_test_suite = {
	.name = "gma500_sgxfive_job",
	.test_cases = sgxfive_job_test_cases,
};

kunit_test_suite(sgxfive_job_test_suite);
