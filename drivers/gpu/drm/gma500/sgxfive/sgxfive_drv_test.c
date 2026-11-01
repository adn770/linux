// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the render-node gate (sgxfive_ioctl_allowed): with the
 * render node off, only DEV_QUERY may reach the device; everything that
 * allocates, maps or runs on the GPU is for trusted users only. And HEAP_INFO's
 * copy into a user array of any accepted stride. And GPU_INFO's identity.
 */
#include <kunit/test.h>
#include <linux/err.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <drm/sgxfive_drm.h>

#include "sgxfive.h"
#include "sgxfive_priv.h"

static void sgxfive_drv_test_gate_on(struct kunit *test)
{
	unsigned int nr;

	for (nr = DRM_SGXFIVE_DEV_QUERY; nr <= DRM_SGXFIVE_SUBMIT_JOBS; nr++)
		KUNIT_EXPECT_TRUE_MSG(test, sgxfive_ioctl_allowed(true, nr), "nr %u", nr);
}

static void sgxfive_drv_test_gate_off(struct kunit *test)
{
	unsigned int nr;

	KUNIT_EXPECT_TRUE(test, sgxfive_ioctl_allowed(false, DRM_SGXFIVE_DEV_QUERY));
	for (nr = DRM_SGXFIVE_CREATE_BO; nr <= DRM_SGXFIVE_SUBMIT_JOBS; nr++)
		KUNIT_EXPECT_FALSE_MSG(test, sgxfive_ioctl_allowed(false, nr), "nr %u", nr);
	/* Off, unknown numbers are refused too. */
	KUNIT_EXPECT_FALSE(test, sgxfive_ioctl_allowed(false, DRM_SGXFIVE_SUBMIT_JOBS + 1));
}

/* HEAP_INFO's struct at the start of a user page, the heap array after it. */
#define HEAPS_OFF	64
#define FILL		0xa5

static u64 user_page(struct kunit *test)
{
	unsigned long m = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					MAP_ANONYMOUS | MAP_PRIVATE, 0);
	u64 u = m;
	u8 *fill;

	if (IS_ERR_VALUE(m))
		kunit_skip(test, "no user memory");
	fill = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fill);
	memset(fill, FILL, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, copy_to_user(u64_to_user_ptr(u), fill, PAGE_SIZE), 0);
	return u;
}

/* Run HEAP_INFO on the page with a stride and count; returns the count written. */
static int query_heaps(struct kunit *test, u64 u, u32 stride, u32 count,
		       u32 *written)
{
	struct drm_sgxfive_dev_query_heap_info info = {
		.heaps = { .stride = stride, .count = count, .array = u + HEAPS_OFF },
	};
	struct drm_sgxfive_dev_query_args a = {
		.type = DRM_SGXFIVE_DEV_QUERY_HEAP_INFO,
		.size = sizeof(info),
		.pointer = u,
	};
	int ret;

	KUNIT_ASSERT_EQ(test, copy_to_user(u64_to_user_ptr(u), &info, sizeof(info)), 0);
	ret = sgxfive_query_heaps(&a);
	KUNIT_ASSERT_EQ(test, copy_from_user(&info, u64_to_user_ptr(u), sizeof(info)), 0);
	*written = info.heaps.count;
	return ret;
}

/* The heap array as the page holds it after a query. */
static u8 *heaps_read(struct kunit *test, u64 u)
{
	u8 *buf = kunit_kmalloc(test, PAGE_SIZE - HEAPS_OFF, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_EQ(test, copy_from_user(buf, u64_to_user_ptr(u + HEAPS_OFF),
					     PAGE_SIZE - HEAPS_OFF), 0);
	return buf;
}

static void sgxfive_drv_test_heaps_exact(struct kunit *test)
{
	const u32 size = sizeof(struct drm_sgxfive_heap);
	u64 u = user_page(test);
	u32 n;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, query_heaps(test, u, size, 100, &n), 0);
	KUNIT_EXPECT_EQ(test, n, (u32)DRM_SGXFIVE_HEAP_COUNT);
	buf = heaps_read(test, u);
	KUNIT_EXPECT_MEMEQ(test, buf, sgxfive_rev1_heaps, sizeof(sgxfive_rev1_heaps));
	KUNIT_EXPECT_EQ(test, buf[sizeof(sgxfive_rev1_heaps)], FILL);
}

/* An older, shorter element gets the prefix of each heap, nothing beyond. */
static void sgxfive_drv_test_heaps_short(struct kunit *test)
{
	u64 u = user_page(test);
	u32 n, i;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, query_heaps(test, u, 8, 3, &n), 0);
	KUNIT_EXPECT_EQ(test, n, 3u);
	buf = heaps_read(test, u);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_MEMEQ(test, buf + 8 * i, &sgxfive_rev1_heaps[i].base, 8);
	KUNIT_EXPECT_EQ(test, buf[3 * 8], FILL);
}

/* A newer, longer element gets each heap followed by zeros up to the stride. */
static void sgxfive_drv_test_heaps_long(struct kunit *test)
{
	const u32 size = sizeof(struct drm_sgxfive_heap);
	const u32 stride = DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE;
	u64 u = user_page(test);
	u32 n, i, j;
	u8 *buf;

	KUNIT_ASSERT_EQ(test, query_heaps(test, u, stride, 2, &n), 0);
	KUNIT_EXPECT_EQ(test, n, 2u);
	buf = heaps_read(test, u);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_MEMEQ(test, buf + stride * i, &sgxfive_rev1_heaps[i], size);
		for (j = size; j < stride; j++)
			KUNIT_EXPECT_EQ(test, buf[stride * i + j], 0);
	}
	KUNIT_EXPECT_EQ(test, buf[2 * stride], FILL);
}

/* A stride of 0 or above the cap is refused before anything is written. */
static void sgxfive_drv_test_heaps_stride_cap(struct kunit *test)
{
	static const u32 bad[] = { 0, DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE + 1, U32_MAX };
	u64 u = user_page(test);
	u32 n, i;
	u8 *buf;

	for (i = 0; i < ARRAY_SIZE(bad); i++)
		KUNIT_EXPECT_EQ_MSG(test, query_heaps(test, u, bad[i], 1, &n), -EINVAL,
				    "stride %u", bad[i]);
	buf = heaps_read(test, u);
	KUNIT_EXPECT_EQ(test, buf[0], FILL);
	/* No element asked for, no stride needed. */
	KUNIT_EXPECT_EQ(test, query_heaps(test, u, U32_MAX, 0, &n), 0);
	KUNIT_EXPECT_EQ(test, n, 0u);
}

/*
 * GPU_INFO has the registers' values at once and the firmware's own once it
 * has answered, in a build without the debug option too; none of them, and
 * the flag clear, while it has not or does not.
 */
static void sgxfive_drv_test_gpu_info_identity(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	struct drm_sgxfive_dev_query_gpu_info g;
	u32 mi[MISCINFO_DWORDS] = { MISCINFO_READY };

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	sgx->core_id = 0x01150000;
	sgx->core_revision = 0x10001;
	sgx->host_bridge_rev = 2;
	memset(&g, 0xa5, sizeof(g));
	sgxfive_gpu_info(sgx, &g);
	KUNIT_EXPECT_EQ(test, g.core_id, 0x01150000u);
	KUNIT_EXPECT_EQ(test, g.core_revision, 0x10001u);
	KUNIT_EXPECT_EQ(test, g.host_bridge_rev, 2u);
	KUNIT_EXPECT_EQ(test, g.quirks, (u64)DRM_SGXFIVE_QUIRK_SAMPLE_CACHE);
	KUNIT_EXPECT_EQ(test, g.flags, 0u);
	KUNIT_EXPECT_EQ(test, g.core_id_sw, 0u);
	KUNIT_EXPECT_EQ(test, g.core_revision_sw, 0u);

	mi[MISCINFO_CORE_REV] = 0x10001;
	mi[MISCINFO_CORE_REV_SW] = 0x10001;
	mi[MISCINFO_CORE_ID] = 0x01150000;
	mi[MISCINFO_CORE_ID_SW] = 545;
	sgxfive_ident_result(sgx, 0, mi);
	sgxfive_gpu_info(sgx, &g);
	KUNIT_EXPECT_EQ(test, g.flags, (u32)DRM_SGXFIVE_GPU_INFO_SW_VALID);
	KUNIT_EXPECT_EQ(test, g.core_id_sw, 545u);
	KUNIT_EXPECT_EQ(test, g.core_revision_sw, 0x10001u);

	sgxfive_ident_result(sgx, -ETIMEDOUT, mi);
	sgxfive_gpu_info(sgx, &g);
	KUNIT_EXPECT_EQ(test, g.flags, 0u);
	KUNIT_EXPECT_EQ(test, g.core_id_sw, 0u);

	/* A reset clears it when the firmware stops, until the next answer. */
	sgxfive_ident_result(sgx, 0, mi);
	sgxfive_ident_clear(sgx);
	sgxfive_gpu_info(sgx, &g);
	KUNIT_EXPECT_EQ(test, g.flags, 0u);
	KUNIT_EXPECT_EQ(test, g.core_id_sw, 0u);
	KUNIT_EXPECT_EQ(test, g.core_revision_sw, 0u);
}

/*
 * FW_INFO's ddk_version is the firmware's own, as GPU_INFO's values are: 0
 * until it has answered and again after it stops or misses, never the last
 * answer's.
 */
static void sgxfive_drv_test_fw_info_ddk_version(struct kunit *test)
{
	struct sgxfive *sgx = kunit_kzalloc(test, sizeof(*sgx), GFP_KERNEL);
	u32 mi[MISCINFO_DWORDS] = { MISCINFO_READY };

	KUNIT_ASSERT_NOT_NULL(test, sgx);
	sgx->core_id = 0x01150000;
	sgx->core_revision = 0x10001;
	mi[MISCINFO_CORE_REV] = 0x10001;
	mi[MISCINFO_CORE_REV_SW] = 0x10001;
	mi[MISCINFO_CORE_ID] = 0x01150000;
	mi[MISCINFO_CORE_ID_SW] = 545;
	mi[MISCINFO_DDK_VERSION] = 0x10711;
	KUNIT_EXPECT_EQ(test, sgxfive_ddk_version(sgx), 0u);
	sgxfive_ident_result(sgx, 0, mi);
	KUNIT_EXPECT_EQ(test, sgxfive_ddk_version(sgx), 0x10711u);
	sgxfive_ident_result(sgx, -ETIMEDOUT, mi);
	KUNIT_EXPECT_EQ(test, sgxfive_ddk_version(sgx), 0u);
	sgxfive_ident_result(sgx, 0, mi);
	sgxfive_ident_clear(sgx);
	KUNIT_EXPECT_EQ(test, sgxfive_ddk_version(sgx), 0u);
}

static struct kunit_case sgxfive_drv_test_cases[] = {
	KUNIT_CASE(sgxfive_drv_test_gpu_info_identity),
	KUNIT_CASE(sgxfive_drv_test_fw_info_ddk_version),
	KUNIT_CASE(sgxfive_drv_test_gate_on),
	KUNIT_CASE(sgxfive_drv_test_gate_off),
	KUNIT_CASE(sgxfive_drv_test_heaps_exact),
	KUNIT_CASE(sgxfive_drv_test_heaps_short),
	KUNIT_CASE(sgxfive_drv_test_heaps_long),
	KUNIT_CASE(sgxfive_drv_test_heaps_stride_cap),
	{}
};

static struct kunit_suite sgxfive_drv_test_suite = {
	.name = "gma500_sgxfive_drv",
	.test_cases = sgxfive_drv_test_cases,
};

kunit_test_suite(sgxfive_drv_test_suite);
