// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The SGX545 debug group's checks on the running firmware
 * (CONFIG_DRM_GMA500_SGXFIVE_DEBUG): each debugfs file sends the EDM work
 * through its kernel CCB and checks what comes back, printing one line per
 * value. They exist to test the firmware interface on the hardware; the
 * driver itself needs none of them.
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>

#include <drm/drm_print.h>

#include "sgxfive_priv.h"
#include "sgx_seq.h"

/* SGXMKIF_CMD_GETMISCINFO's request and its reply in the misc-info buffer. */
#define MISCINFO_READY			0x1
#define MISCINFO_GET_STRUCT_SIZES	0x2
#define MISCINFO_FEATURES		7
#define MISCINFO_STRUCT_SIZES		13
#define MISCINFO_DWORDS		(1 + MISCINFO_FEATURES + MISCINFO_STRUCT_SIZES)

static void sgxfive_check(struct drm_printer *p, const char *what, u32 got,
			  u32 want, int *bad)
{
	drm_printf(p, "%-16s %#010x %s\n", what, got,
		   got == want ? "ok" : "MISMATCH");
	if (got != want) {
		drm_printf(p, "%-16s expected %#010x\n", "", want);
		(*bad)++;
	}
}

static const char *const sgxfive_feature_names[MISCINFO_FEATURES] = {
	"core rev", "core id", "ddk version", "ddk build", "core id sw",
	"core rev sw", "build options",
};

/*
 * GETMISCINFO with GET_STRUCT_SIZES: the EDM fills in the core and DDK
 * identity and the sizes of the structures it was built with. Check every
 * value against the registers and the firmware file.
 */
static int sgxfive_miscinfo(struct sgxfive *sgx, struct drm_printer *p)
{
	u32 *mi = sgxfive_words(sgx, BUF_MISC);
	const u32 data[] = { 0, sgx->layout.addr[BUF_MISC] };
	u32 want[MISCINFO_FEATURES];
	ktime_t start;
	int i, ret, bad = 0;

	if (!sgx->live)
		return -ENODEV;
	for (i = 1; i < MISCINFO_DWORDS; i++)
		WRITE_ONCE(mi[i], 0);
	WRITE_ONCE(mi[0], MISCINFO_GET_STRUCT_SIZES);
	ret = sgxfive_command(sgx, SGX_CMD_GETMISCINFO, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	start = ktime_get();
	ret = sgxfive_wait_mem(&mi[0], MISCINFO_READY, MISCINFO_READY);
	if (ret) {
		drm_printf(p, "no MISCINFO_READY after 500 ms\n");
		sgxfive_dump(sgx, p);
		return ret;
	}
	drm_printf(p, "GETMISCINFO answered after %lld us\n",
		   ktime_us_delta(ktime_get(), start));

	want[0] = sgxfive_rd(sgx, SGX_CORE_REVISION);
	want[1] = sgxfive_rd(sgx, SGX_CORE_ID);
	/* major, minor and branch: DDK 1.7 is branch 17 */
	want[2] = sgx->fw.ddk_major << 16 | sgx->fw.ddk_minor << 8 | 17;
	want[3] = sgx->fw.ddk_build;
	want[4] = 545;
	want[5] = want[0];		/* the revision the EDM was built for */
	want[6] = sgx->fw.build_options;
	for (i = 0; i < MISCINFO_FEATURES; i++)
		sgxfive_check(p, sgxfive_feature_names[i], mi[1 + i], want[i],
			      &bad);
	for (i = 0; i < MISCINFO_STRUCT_SIZES; i++) {
		char what[16];

		snprintf(what, sizeof(what), "struct size %d", i);
		sgxfive_check(p, what, mi[1 + MISCINFO_FEATURES + i],
			      sgx->fw.meta[SGX_FW_META_STRUCT_SIZES + i], &bad);
	}
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "all values match");
	return bad ? -EIO : 0;
}

static int sgxfive_miscinfo_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_miscinfo(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_miscinfo);

/* Writing to idle asks the EDM to idle the core through the POWER command. */
static ssize_t sgxfive_idle_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_power(sgx, POWERCMD_IDLE, POWMAN_IDLE_COMPLETE);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_idle_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_idle_write,
};

void sgxfive_debug_init(struct sgxfive *sgx, struct dentry *dir)
{
	debugfs_create_file("miscinfo", 0400, dir, sgx, &sgxfive_miscinfo_fops);
	debugfs_create_file("idle", 0200, dir, sgx, &sgxfive_idle_fops);
}
