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

#include <linux/unaligned.h>

#include "sgx_fw.h"
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
	/* str, never ldr, of the two host clear registers (CLEAR2, CLEAR) */
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0114 / 4, true));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0134 / 4, true));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x0114 / 4, false));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x0134 / 4, false));
	/* and of the EDM timer, which the closed tick handler writes */
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0acc / 4, true));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x0acc / 4, false));
}

static bool own_in_deny_row(unsigned int off)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sgx_own_deny); i++)
		if (sgx_own_deny[i][0] <= off && off <= sgx_own_deny[i][1])
			return true;
	return false;
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
	/* the event block: every register a deny row, but the str-only clear registers */
	for (off = 0x0110; off <= 0x0144; off += 4) {
		bool clear = off == 0x0114 || off == 0x0134;

		KUNIT_EXPECT_EQ_MSG(test, own_in_deny_row(off), !clear, "0x%04x", off);
		KUNIT_EXPECT_EQ_MSG(test, sgx_own_reg_ok(off / 4, true), clear, "str 0x%04x", off);
	}
	/* the kicker, the kicks and the PDS invalidates: all denied, but the str-only timer */
	for (off = 0x0ac4; off <= 0x0aec; off += 4) {
		bool timer = off == 0x0acc;

		KUNIT_EXPECT_EQ_MSG(test, own_in_deny_row(off), !timer, "0x%04x", off);
		KUNIT_EXPECT_EQ_MSG(test, sgx_own_reg_ok(off / 4, true), timer, "str 0x%04x", off);
	}
}

/* a blob of the given words, no patch, entry 0 */
static int verify_words(const u64 *w, unsigned int n, const char **why)
{
	u8 buf[16 + 8 * 4 + 2];
	unsigned int i;

	if (n > 4)
		return -E2BIG;
	memset(buf, 0, sizeof(buf));
	memcpy(buf, "SGXOWN01", 8);
	put_unaligned_le16(n, buf + 8);
	put_unaligned_le16(1, buf + 12);
	for (i = 0; i < n; i++)
		put_unaligned_le64(w[i], buf + 16 + 8 * i);
	return verify_data(buf, 16 + 8 * n + 2, why);
}

#define OWN_NOP_END	0xf804014000000000ULL
#define OWN_MOV_PA_R1	0x2801100220000080ULL	/* mov pa0, r1 */

/* a mov to pa2, pa3 or pa4 is accepted; to any other pa refused for its reason */
static void sgxfive_owntask_test_mov_pa(struct kunit *test)
{
	unsigned int n;

	for (n = 0; n < 128; n++) {
		u64 w[2] = { OWN_MOV_PA_R1 | (u64)n << 21, OWN_NOP_END };
		const char *why = "";
		int ret = verify_words(w, 2, &why);

		if (n >= 2 && n <= 4) {
			KUNIT_EXPECT_EQ_MSG(test, ret, 0, "pa%u: %s", n, why);
		} else {
			KUNIT_EXPECT_EQ_MSG(test, ret, -EPERM, "pa%u", n);
			KUNIT_EXPECT_STREQ_MSG(test, why, SGX_OWN_WHY_PA, "pa%u", n);
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


/* ---- the overlay ---------------------------------------------------------------------------- */
/*
 * A synthetic firmware, built in place: a ukernel at the code base (uk_off 0) and a PDS buffer
 * (id 4, like PDS #13) whose four CODE_WORD records name the closed tasks 6, 1434, 2766 and 3594,
 * in a scrambled order, so the slot of a task is its rank by index and not its place.
 */
#define PDS_BUF		SGXFIVE_OWN_PDS_BUF
#define PDS_SIZE	0x100
#define PDS_ADDEND	0x0800000f

static void rec(u8 *p, u8 buffer, u32 offset, u32 target, u32 addend)
{
	memset(p, 0, 16);
	p[0] = buffer;			/* buffer, WORD32 (0), CODE_WORD (5), param 0 */
	p[2] = 5;
	put_unaligned_le32(offset, p + 4);
	put_unaligned_le32(target, p + 8);
	put_unaligned_le32(addend, p + 12);
}

struct own_fw {
	struct sgx_fw fw;
	struct sgx_fw_layout l;
	u8 recs[4 * 16];
	u8 uk[0x20], pds[PDS_SIZE], stock[PDS_SIZE];
	u8 *bufs[SGX_FW_MAX_BUFFERS];
	u8 code[SGX_OWN_CODE_SIZE];
};

static struct own_fw *own_fw_new(struct kunit *test, unsigned int nrecs)
{
	static const u32 tgt[4] = { 3594, 6, 2766, 1434 };
	struct own_fw *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->fw.nbuffers = 2;
	f->fw.buffers[0] = (struct sgx_fw_buffer){ .id = 0, .heap = 2, .size = sizeof(f->uk), .align = 8 };
	f->fw.buffers[1] = (struct sgx_fw_buffer){ .id = PDS_BUF, .heap = 2, .size = PDS_SIZE, .align = 16 };
	for (i = 0; i < nrecs; i++)
		rec(f->recs + 16 * i, PDS_BUF, 4 * i, tgt[i], PDS_ADDEND);
	f->fw.patches.rec = f->recs;
	f->fw.patches.count = nrecs;
	f->l.heap_base[SGX_FW_HEAP_KERNEL_CODE] = 0xe8000000;
	f->l.heap_set = 1u << SGX_FW_HEAP_KERNEL_CODE;
	f->l.addr[0] = 0xe8000000;			/* uk_off 0 */
	f->l.addr[PDS_BUF] = 0xe8010000;
	f->l.addr_set = 1u << 0 | 1u << PDS_BUF;
	f->bufs[0] = f->uk;
	f->bufs[PDS_BUF] = f->pds;
	KUNIT_ASSERT_EQ(test, sgx_fw_build(&f->fw, &f->l, f->bufs), 0);
	memcpy(f->stock, f->pds, PDS_SIZE);
	return f;
}

static u32 dw(const u8 *p, unsigned int off)
{
	return get_unaligned_le32(p + off);
}

static int own_apply(struct own_fw *f, const u8 *blob, size_t len, const char *arm)
{
	struct sgx_own_arm a;
	struct sgx_own_blob b;
	int ret;

	ret = sgx_own_arm_parse(arm, &a);
	if (ret)
		return ret;
	ret = sgx_own_parse(blob, len, &b);
	if (ret)
		return ret;
	return sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE, f->code, 0x32140000);
}

/* exactly the armed start dwords differ, each ((entry << 4) | the addend) with the TRC */
static void sgxfive_owntask_test_apply_only_armed_dwords(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	unsigned int off;

	/* slots 0 and 2: the tasks 6 (at offset 4) and 2766 (at offset 8) */
	KUNIT_ASSERT_EQ(test, own_apply(f, own_ok_null, sizeof(own_ok_null),
					"slots=0x5 entry=0,0 trc=9"), 0);
	for (off = 0; off < PDS_SIZE; off += 4) {
		if (off == 4 || off == 8)
			KUNIT_EXPECT_EQ_MSG(test, dw(f->pds, off), 0x0904000fu, "offset %u", off);
		else
			KUNIT_EXPECT_EQ_MSG(test, dw(f->pds, off), dw(f->stock, off), "offset %u", off);
	}
	/* the unarmed ones still hold the closed tasks' words */
	KUNIT_EXPECT_EQ(test, dw(f->pds, 0), (3594u << 4) | PDS_ADDEND);
	KUNIT_EXPECT_EQ(test, dw(f->pds, 12), (1434u << 4) | PDS_ADDEND);
}

static void sgxfive_owntask_test_apply_default_trc_and_entry(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	struct own_fw *g = own_fw_new(test, 4);
	unsigned int i;

	/* no trc= keeps what the stock word holds, whatever it is (here 9, not the 8 of the file) */
	for (i = 0; i < 4; i++)
		put_unaligned_le32(0x0900000f, g->recs + 16 * i + 12);
	KUNIT_ASSERT_EQ(test, sgx_fw_build(&g->fw, &g->l, g->bufs), 0);
	KUNIT_ASSERT_EQ(test, own_apply(g, own_ok_null, sizeof(own_ok_null), "slots=0x8 entry=0"), 0);
	KUNIT_EXPECT_EQ(test, dw(g->pds, 0), 0x0904000fu);

	/* the null blob has one entry (0): slot 3 is the task 3594 at offset 0, trc 8 */
	KUNIT_ASSERT_EQ(test, own_apply(f, own_ok_null, sizeof(own_ok_null), "slots=0x8 entry=0"), 0);
	KUNIT_EXPECT_EQ(test, dw(f->pds, 0), 0x0804000fu);
	KUNIT_EXPECT_EQ(test, dw(f->pds, 4), dw(f->stock, 4));
}

/* the blob's patches: the results address, and the continue index of the slot's closed task */
static void sgxfive_owntask_test_apply_patches(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	struct sgx_own_blob b;

	KUNIT_ASSERT_EQ(test, own_apply(f, own_ok_null, sizeof(own_ok_null), "slots=0x1 entry=0"), 0);
	/* null.s: mov r30, #cont0 at index 0 (the continue of slot 0, the task 6) */
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(get_unaligned_le64(f->code)), 6u);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_null, sizeof(own_ok_null), &b), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code + 8), sgx_own_insn(&b, 1));
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code + 16), sgx_own_insn(&b, 2));
	/* the rest of the code page is zero */
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code + 24), 0ull);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_mem, sizeof(own_ok_mem), &b), 0);
	KUNIT_ASSERT_EQ(test, own_apply(f, own_ok_mem, sizeof(own_ok_mem), "slots=0x4 entry=0"), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(get_unaligned_le64(f->code)), 0x32140000u);
	KUNIT_EXPECT_EQ(test, dw(f->pds, 8), 0x0804000fu);
}

static void sgxfive_owntask_test_apply_refusals(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	struct own_fw *g = own_fw_new(test, 2);

	/* a slot past the tasks the firmware has, and a blob without that entry */
	KUNIT_EXPECT_EQ(test, own_apply(g, own_ok_null, sizeof(own_ok_null), "slots=0x4 entry=0"),
			-ENOENT);
	KUNIT_EXPECT_EQ(test, own_apply(f, own_ok_null, sizeof(own_ok_null), "slots=0x1 entry=1"),
			-EINVAL);
	/* nothing was written by the refused ones */
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, memcmp(g->pds, g->stock, PDS_SIZE), 0);
	/* an index below the ukernel's offset is refused by the evaluation */
	f->l.addr[0] = 0xe8000000 + 8 * 0x5000;		/* uk_off 0x5000, above our index */
	KUNIT_EXPECT_EQ(test, own_apply(f, own_ok_null, sizeof(own_ok_null), "slots=0x1 entry=0"),
			-ERANGE);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
}

/* the verifier is the last step before the copy, and one record per armed target */
static void sgxfive_owntask_test_apply_reverifies_and_wants_one_record(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	struct own_fw *g = own_fw_new(test, 4);

	/* a blob the verifier refuses (a clock-gate str) is refused here too, and nothing is written */
	KUNIT_EXPECT_EQ(test, own_apply(f, own_bad_clock_str_ctl, sizeof(own_bad_clock_str_ctl),
					"slots=0x1 entry=0"), -EPERM);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);
	/* two records for the task 6: the second start dword would still start it */
	put_unaligned_le32(6, g->recs + 16 * 3 + 8);		/* record 3 was the task 1434 */
	KUNIT_EXPECT_EQ(test, own_apply(g, own_ok_null, sizeof(own_ok_null), "slots=0x1 entry=0"),
			-EEXIST);
	KUNIT_EXPECT_EQ(test, memcmp(g->pds, g->stock, PDS_SIZE), 0);
}

static void sgxfive_owntask_test_arm_parse(struct kunit *test)
{
	struct sgx_own_arm a;

	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0", &a), 0);
	KUNIT_EXPECT_EQ(test, a.slots, 1u);
	KUNIT_EXPECT_EQ(test, a.nentry, 1u);
	KUNIT_EXPECT_EQ(test, (int)a.trc, SGX_OWN_TRC_STOCK);	/* not given: the stock word's */
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=0xf entry=0,1,2,3 trc=31", &a), 0);
	KUNIT_EXPECT_EQ(test, a.nentry, 4u);
	KUNIT_EXPECT_EQ(test, (int)a.entry[3], 3);
	KUNIT_EXPECT_EQ(test, (int)a.trc, 31);
	/* 32 and up only with the hazard word */
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 trc=32", &a), -EPERM);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 trc=64 trc_hazard=0", &a), -EPERM);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 trc=64 trc_hazard=1", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.trc, 64);
	/* an unknown trc, a slot mask outside the four, a count that does not match, junk */
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 trc=0", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 trc=256 trc_hazard=1", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=0 entry=0", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=0x10 entry=0", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=3 entry=0", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=4", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("entry=0", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 bogus=1", &a), -EINVAL);
}

/* own_res: dword aligned writes that leave dword 0, the kernel's header, alone */
static void sgxfive_owntask_test_res_write(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(0, 4));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(0, 16));
	KUNIT_EXPECT_TRUE(test, sgx_own_res_write_ok(4, 4));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(2, 4));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(4, 3));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(4, 0));
	KUNIT_EXPECT_TRUE(test, sgx_own_res_write_ok(SGX_OWN_RES_SIZE - 4, 4));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(SGX_OWN_RES_SIZE - 4, 8));
	/* pos + len must not overflow a loff_t into a pass */
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(0x7ffffffffffffffcLL, 4));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(SGX_OWN_RES_SIZE + 4, 4));
	KUNIT_EXPECT_FALSE(test, sgx_own_res_write_ok(4, (size_t)-4));
}

static struct kunit_case sgxfive_owntask_test_cases[] = {
	KUNIT_CASE(sgxfive_owntask_test_accepts_our_blobs),
	KUNIT_CASE(sgxfive_owntask_test_refuses_crafted_blobs),
	KUNIT_CASE(sgxfive_owntask_test_forms_are_canonical),
	KUNIT_CASE(sgxfive_owntask_test_clock_gate),
	KUNIT_CASE(sgxfive_owntask_test_register_lists),
	KUNIT_CASE(sgxfive_owntask_test_deny_rows),
	KUNIT_CASE(sgxfive_owntask_test_mov_pa),
	KUNIT_CASE(sgxfive_owntask_test_parse),
	KUNIT_CASE(sgxfive_owntask_test_apply_only_armed_dwords),
	KUNIT_CASE(sgxfive_owntask_test_apply_default_trc_and_entry),
	KUNIT_CASE(sgxfive_owntask_test_apply_patches),
	KUNIT_CASE(sgxfive_owntask_test_apply_refusals),
	KUNIT_CASE(sgxfive_owntask_test_apply_reverifies_and_wants_one_record),
	KUNIT_CASE(sgxfive_owntask_test_arm_parse),
	KUNIT_CASE(sgxfive_owntask_test_res_write),
	{}
};

static struct kunit_suite sgxfive_owntask_test_suite = {
	.name = "gma500_sgxfive_owntask",
	.test_cases = sgxfive_owntask_test_cases,
};
kunit_test_suite(sgxfive_owntask_test_suite);
