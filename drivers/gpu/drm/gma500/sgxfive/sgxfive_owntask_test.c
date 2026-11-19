// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgxfive_owntask.c, built only with CONFIG_DRM_GMA500_SGXFIVE_DEBUG: the blob
 * parser and the default-deny verifier. The blobs are the ones tools/fwtask builds from
 * asm/fwtask (accepted) and crafted ones (refused, each with its reason), in the generated
 * sgxfive_owntask_blobs.h; tools/fwtask/test_verifier.py holds the same cases against the tool's
 * verifier, and test_host_verifier.py runs this file's verifier on the host against it.
 */
#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/unaligned.h>

#include "sgx_fw.h"
#include "sgx_own_pds.h"
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
	/* ldr, never str, of the two DPM page status registers */
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x0724 / 4, false));
	KUNIT_EXPECT_TRUE(test, sgx_own_reg_ok(0x072c / 4, false));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x0724 / 4, true));
	KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(0x072c / 4, true));
	/* and nothing around them */
	for (i = 0; i < 4; i++) {
		static const u16 off[] = { 0x0720, 0x0728, 0x0730, 0x0734 };

		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(off[i] / 4, true));
		KUNIT_EXPECT_FALSE(test, sgx_own_reg_ok(off[i] / 4, false));
	}
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
	u8 buf[16 + 8 * 8 + 2];
	unsigned int i;

	if (n > 8)
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

#define OWN_EXIT_CFI	0xf843b300a0000180ULL	/* cfi.global #3, #0, #3 */
#define OWN_EXIT_PHAS	0xfa44070000000000ULL	/* phas #0, #0, pixel, end, parallel */

/*
 * The closed tasks' shared exit: a copy of its five words is accepted, and of its cfi and phas
 * only those two exact words (no bit of either may change)
 */
static void sgxfive_owntask_test_exit_words(struct kunit *test)
{
	static const u64 tail[] = { OWN_EXIT_CFI, 0xf820000000000000ULL /* idf drc0, bif */,
				    0xf920000000000000ULL /* wdf drc0 */, OWN_EXIT_PHAS,
				    OWN_NOP_END };
	static const u64 word[] = { OWN_EXIT_CFI, OWN_EXIT_PHAS };
	const char *why = "";
	unsigned int i, b;

	KUNIT_EXPECT_EQ_MSG(test, verify_words(tail, ARRAY_SIZE(tail), &why), 0, "%s", why);
	for (i = 0; i < ARRAY_SIZE(word); i++) {
		for (b = 0; b < 64; b++) {
			u64 w[2] = { word[i] ^ BIT_ULL(b), OWN_NOP_END };

			why = "";
			KUNIT_EXPECT_EQ_MSG(test, verify_words(w, 2, &why), -EPERM,
					    "0x%016llx bit %u", word[i], b);
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
#define F14_BUF		SGXFIVE_OWN_FW14_BUF
#define F14_SIZE	0x68		/* PDS #14's buffer size (fw_format section 3) */
#define F14_ROOM	0x1000		/* its page */
#define F14_ADDR	0xe8011000
#define ARM14		"slots=0x1 entry=0 pds=crumb3 pdsat=fw14"

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
	u8 recs[6 * 16];
	u8 uk[0x20], pds[PDS_SIZE], stock[PDS_SIZE];
	u8 f14[F14_ROOM], stock14[F14_ROOM], f14img[F14_SIZE];	/* own_fw14_new() */
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
	return sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE, f->code, 0x32140000,
			     NULL);
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

/* a blob of the continue triple with a cont<slot> patch on its limm, entry 0 */
static size_t own_triple(u8 *buf, u8 slot)
{
	static const u64 w[3] = { 0xfc23715003cdbeefULL /* mov r30, #0xdeadbeef */,
				  0xf80000c000000f00ULL /* mov pclink, r30 */,
				  0xf800008000000000ULL /* lapc */ };
	unsigned int i;

	memset(buf, 0, 16 + 8 * 3 + 8 + 2);
	memcpy(buf, "SGXOWN01", 8);
	put_unaligned_le16(3, buf + 8);
	put_unaligned_le16(1, buf + 10);
	put_unaligned_le16(1, buf + 12);
	for (i = 0; i < 3; i++)
		put_unaligned_le64(w[i], buf + 16 + 8 * i);
	buf[16 + 24 + 2] = SGX_OWN_PATCH_CONT;		/* index 0, kind, slot, addend 0 */
	buf[16 + 24 + 3] = slot;
	return 16 + 8 * 3 + 8 + 2;
}

static const u8 own_exit_fw_digest[32] = {
	0x6a, 0x00, 0x26, 0x8e, 0x9d, 0x4c, 0xa5, 0x52, 0xc0, 0x2e, 0x52, 0x65, 0x1a, 0x42, 0xa2, 0xb4,
	0xdb, 0x38, 0x87, 0xd7, 0xfe, 0x62, 0x6f, 0x20, 0x98, 0x88, 0x36, 0x99, 0x85, 0x16, 0x2b, 0x0a,
};

/* the shared exit's index is known for one firmware file only */
static void sgxfive_owntask_test_exit_index(struct kunit *test)
{
	u8 d[32];
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sgx_own_exit_index(own_exit_fw_digest), (u32)SGX_OWN_EXIT_UK_INDEX);
	KUNIT_EXPECT_EQ(test, (u32)SGX_OWN_EXIT_UK_INDEX, 0x1974u);
	for (i = 0; i < 32; i++) {
		memcpy(d, own_exit_fw_digest, 32);
		d[i] ^= 0x01;
		KUNIT_EXPECT_EQ_MSG(test, sgx_own_exit_index(d), 0u, "byte %u", i);
	}
	memset(d, 0, sizeof(d));
	KUNIT_EXPECT_EQ(test, sgx_own_exit_index(d), 0u);
}

/*
 * cont4: the verifier takes slot 4 as a continue patch (slot 5 is a bad slot); the overlay puts
 * the exit's index, from the code base, in the limm when the load gave it, and refuses the arm,
 * writing nothing, when it did not
 */
static void sgxfive_owntask_test_apply_cont4(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	struct sgx_own_arm a;
	struct sgx_own_blob b;
	const char *why = "";
	u8 buf[64];
	size_t len;

	len = own_triple(buf, 5);
	KUNIT_EXPECT_EQ(test, verify_data(buf, len, &why), -EPERM);
	KUNIT_EXPECT_STREQ(test, why, SGX_OWN_WHY_PATCH_SLOT);
	len = own_triple(buf, SGX_OWN_EXIT_SLOT);
	KUNIT_ASSERT_EQ(test, verify_data(buf, len, &why), 0);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(buf, len, &b), 0);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=0x1 entry=0", &a), 0);
	KUNIT_EXPECT_EQ(test, a.exit_index, 0u);	/* own_arm never sets it */

	/* not known: refused, nothing written */
	KUNIT_EXPECT_EQ(test, sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE,
					    f->code, 0x32140000, NULL), -ENOENT);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);

	/* known: the limm holds it (the ukernel at the code base here), the rest as cont0 does */
	a.exit_index = SGX_OWN_EXIT_UK_INDEX;
	KUNIT_ASSERT_EQ(test, sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE,
					    f->code, 0x32140000, NULL), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(get_unaligned_le64(f->code)), 0x1974u);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code + 8), sgx_own_insn(&b, 1));
	KUNIT_EXPECT_EQ(test, dw(f->pds, 4), 0x0804000fu);

	/* the ukernel 0x10 instructions above the code base: the index moves with it */
	f = own_fw_new(test, 4);
	f->l.addr[0] = 0xe8000000 + 8 * 0x10;
	KUNIT_ASSERT_EQ(test, sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE,
					    f->code, 0x32140000, NULL), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(get_unaligned_le64(f->code)), 0x1984u);
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
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_V1);	/* not given: the v1 image */
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
	/* the emitpds image: v1 (the default) or crumb, nothing else */
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=crumb", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_CRUMB);
	KUNIT_EXPECT_EQ(test, a.slots, 1u);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 trc=8 pds=v1", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_V1);
	KUNIT_EXPECT_EQ(test, (int)a.trc, 8);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=2", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=CRUMB", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=crumbs", &a), -EINVAL);
	/* own_arm shows the image by these names */
	KUNIT_EXPECT_STREQ(test, sgx_own_img_name(SGX_OWN_IMG_V1), "v1");
	KUNIT_EXPECT_STREQ(test, sgx_own_img_name(SGX_OWN_IMG_CRUMB), "crumb");
	KUNIT_EXPECT_STREQ(test, sgx_own_img_name(SGX_OWN_IMG_COUNT), "?");
	/* round 38: pds=halt|crumb3, and where the image sits, pdsat=own (the default) or fw14 */
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.at, SGX_OWN_AT_OWN);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=halt", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_HALT);
	KUNIT_EXPECT_EQ(test, (int)a.at, SGX_OWN_AT_OWN);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=crumb3 pdsat=fw14", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_CRUMB3);
	KUNIT_EXPECT_EQ(test, (int)a.at, SGX_OWN_AT_FW14);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pdsat=fw14", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_V1);
	KUNIT_EXPECT_EQ(test, (int)a.at, SGX_OWN_AT_FW14);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=crumb pdsat=own", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.at, SGX_OWN_AT_OWN);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=crumb4", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds=halt3", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pdsat=", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pdsat=fw13", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pdsat=FW14", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pdsat=1", &a), -EINVAL);
	KUNIT_EXPECT_STREQ(test, sgx_own_img_name(SGX_OWN_IMG_HALT), "halt");
	KUNIT_EXPECT_STREQ(test, sgx_own_img_name(SGX_OWN_IMG_CRUMB3), "crumb3");
	KUNIT_EXPECT_STREQ(test, sgx_own_at_name(SGX_OWN_AT_OWN), "own");
	KUNIT_EXPECT_STREQ(test, sgx_own_at_name(SGX_OWN_AT_FW14), "fw14");
	KUNIT_EXPECT_STREQ(test, sgx_own_at_name(SGX_OWN_AT_COUNT), "?");
	/* round 39: the emit's s2, pds2=0 (the default) or req, and its word, form=own or fw14 */
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.pds2, SGX_OWN_PDS2_ZERO);
	KUNIT_EXPECT_EQ(test, (int)a.form, SGX_OWN_FORM_OWN);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse(ARM14 " pds2=req form=fw14", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.img, SGX_OWN_IMG_CRUMB3);
	KUNIT_EXPECT_EQ(test, (int)a.at, SGX_OWN_AT_FW14);
	KUNIT_EXPECT_EQ(test, (int)a.pds2, SGX_OWN_PDS2_REQ);
	KUNIT_EXPECT_EQ(test, (int)a.form, SGX_OWN_FORM_FW14);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds2=req", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.pds2, SGX_OWN_PDS2_REQ);
	KUNIT_EXPECT_EQ(test, (int)a.form, SGX_OWN_FORM_OWN);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 form=fw14 pds2=0", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.pds2, SGX_OWN_PDS2_ZERO);
	KUNIT_EXPECT_EQ(test, (int)a.form, SGX_OWN_FORM_FW14);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 form=own", &a), 0);
	KUNIT_EXPECT_EQ(test, (int)a.form, SGX_OWN_FORM_OWN);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds2=", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds2=1", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds2=0x20000", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 pds2=REQ", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 form=", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 form=fw11", &a), -EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_arm_parse("slots=1 entry=0 form=0x843", &a), -EINVAL);
	KUNIT_EXPECT_STREQ(test, sgx_own_pds2_name(SGX_OWN_PDS2_ZERO), "0");
	KUNIT_EXPECT_STREQ(test, sgx_own_pds2_name(SGX_OWN_PDS2_REQ), "req");
	KUNIT_EXPECT_STREQ(test, sgx_own_pds2_name(SGX_OWN_PDS2_COUNT), "?");
	KUNIT_EXPECT_EQ(test, sgx_own_pds2_value(SGX_OWN_PDS2_ZERO), 0u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds2_value(SGX_OWN_PDS2_REQ), 0x00020000u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds2_value(SGX_OWN_PDS2_COUNT), 0u);
	KUNIT_EXPECT_STREQ(test, sgx_own_form_name(SGX_OWN_FORM_OWN), "own");
	KUNIT_EXPECT_STREQ(test, sgx_own_form_name(SGX_OWN_FORM_FW14), "fw14");
	KUNIT_EXPECT_STREQ(test, sgx_own_form_name(SGX_OWN_FORM_COUNT), "?");
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


/* ---- the one emitpds form (round 33, N3) ------------------------------------------------- */
#define OWN_PDS_ADDR	0x30022000u
#define OWN_V0		0x32000000u	/* stand-ins for TA3DCtl dwords 0 and 2 */
#define OWN_V2		0x32000400u

/* the emitpds word is recognised in a blob that has it, and only there */
static void sgxfive_owntask_test_has_emit(struct kunit *test)
{
	struct sgx_own_blob b;
	unsigned int i, n = 0;

	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsa, sizeof(own_ok_emitsa), &b), 0);
	KUNIT_EXPECT_TRUE(test, sgx_own_has_emit(&b));
	for (i = 0; i < b.n_insn; i++)
		n += sgx_own_insn(&b, i) == 0xfb20b00000008184ULL;
	KUNIT_EXPECT_EQ(test, n, 1u);
	KUNIT_EXPECT_GE(test, b.n_entry, 2);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_null, sizeof(own_ok_null), &b), 0);
	KUNIT_EXPECT_FALSE(test, sgx_own_has_emit(&b));
}

/*
 * The image is the generated program with exactly U0, V0 and V2 filled in, the rest of its page
 * zero. U0 starts the blob's instruction on the code page (CBASE 15, the KERNEL_CODE heap; index
 * SGX_OWN_INDEX + entry, inside the page; TRC 8); U1 and U2 are 0; the douta controls name
 * attributes 0 and 2; the instructions are the generated douta, douta, doutu, halt.
 */
static void sgxfive_owntask_test_pds_build(struct kunit *test)
{
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	unsigned int i;
	u32 u0;

	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0xa5, SGX_OWN_PDS_SIZE);
	sgx_own_pds_build(page, SGX_OWN_IMG_V1, 33, OWN_V0, OWN_V2);
	u0 = sgx_own_pds_u0(33);
	KUNIT_EXPECT_EQ(test, u0, 0x0800000fu | (SGX_OWN_INDEX + 33u) << 4);
	KUNIT_EXPECT_EQ(test, u0 & 0xf, 0xfu);
	KUNIT_EXPECT_EQ(test, u0 >> 24, (u32)SGX_OWN_PDS_TRC);
	KUNIT_EXPECT_GE(test, (u0 >> 4) & 0x7ffff, (u32)SGX_OWN_INDEX);
	KUNIT_EXPECT_LT(test, (u0 >> 4) & 0x7ffff, (u32)SGX_OWN_INDEX + SGX_OWN_MAX_INSNS);
	for (i = 0; i < SGX_OWN_PDS_WORDS; i++) {
		u32 want = sgx_own_pds_image[i];

		if (i == SGX_OWN_PDS_U0)
			want = u0;
		else if (i == SGX_OWN_PDS_V0)
			want = OWN_V0;
		else if (i == SGX_OWN_PDS_V2)
			want = OWN_V2;
		KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * i), want, "dword %u", i);
	}
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U1), 0u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U2), 0u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 3), 0x000u);			/* douta: attribute 0 */
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 6), 0x200u);			/* douta: attribute 2 */
	KUNIT_EXPECT_EQ(test, 4 * SGX_OWN_PDS_ROWS, 8);
	KUNIT_EXPECT_EQ(test, (u32)SGX_OWN_PDS_CTL, (u32)SGX_OWN_PDS_ROWS << 23 | 0x100);
	for (i = 4 * SGX_OWN_PDS_WORDS; i < SGX_OWN_PDS_SIZE; i++)
		if (page[i]) {
			KUNIT_FAIL(test, "byte %u of the page is not zero", i);
			break;
		}
	/* the words that are not filled are the generated program's: movs ... halt */
	KUNIT_EXPECT_EQ(test, sgx_own_pds_image[8] >> 24, 0x07u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_image[SGX_OWN_PDS_WORDS - 1], 0xaf000000u);
}

/*
 * The crumb image (round 37, N3b): v1's data rows and fills, dword 7 = 0x005a3305, and the
 * instruction words the header's: tim, douta, douta, tim, doutu, tim, halt; the three tim words
 * read dwords 3, 6 and 7, whose values have bit 24 clear (tim holds them: no count). Zeros after.
 */
static void sgxfive_owntask_test_pds_build_crumb(struct kunit *test)
{
	static const u32 op[] = { 0x07001fe7, 0x07040606, 0x07042ca6, 0x07003547, 0x07000305,
				  0x07003fe7, 0xaf000000 };
	static const unsigned int tim[] = { 3, 6, 7 };		/* ds1[1], ds1[2], ds1[3] */
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, SGX_OWN_PDS_CRUMB_WORDS, 15);
	memset(page, 0xa5, SGX_OWN_PDS_SIZE);
	sgx_own_pds_build(page, SGX_OWN_IMG_CRUMB, 33, OWN_V0, OWN_V2);
	for (i = 0; i < SGX_OWN_PDS_CRUMB_WORDS; i++) {
		u32 want = sgx_own_pds_crumb[i];

		if (i == SGX_OWN_PDS_U0)
			want = sgx_own_pds_u0(33);
		else if (i == SGX_OWN_PDS_V0)
			want = OWN_V0;
		else if (i == SGX_OWN_PDS_V2)
			want = OWN_V2;
		KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * i), want, "dword %u", i);
	}
	/* the data rows are v1's but dword 7 */
	for (i = 0; i < 4 * SGX_OWN_PDS_ROWS - 1; i++)
		KUNIT_EXPECT_EQ_MSG(test, sgx_own_pds_crumb[i], sgx_own_pds_image[i], "dword %u", i);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 3), 0x000u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 6), 0x200u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 7), 0x005a3305u);
	for (i = 0; i < ARRAY_SIZE(tim); i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * tim[i]) & (1u << 24), 0u, "dword %u", tim[i]);
	for (i = 0; i < ARRAY_SIZE(op); i++)
		KUNIT_EXPECT_EQ_MSG(test, sgx_own_pds_crumb[8 + i], op[i], "insn %u", i);
	for (i = 4 * SGX_OWN_PDS_CRUMB_WORDS; i < SGX_OWN_PDS_SIZE; i++)
		if (page[i]) {
			KUNIT_FAIL(test, "byte %u of the page is not zero", i);
			break;
		}
	/* an image the arm cannot name builds an empty page */
	memset(page, 0xa5, SGX_OWN_PDS_SIZE);
	sgx_own_pds_build(page, SGX_OWN_IMG_COUNT, 33, OWN_V0, OWN_V2);
	KUNIT_EXPECT_EQ(test, dw(page, 0), 0u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 8), 0u);
}

static int own_apply_pds(struct own_fw *f, const u8 *blob, size_t len, const char *arm,
			 struct sgx_own_pds *pds)
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
	return sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE, f->code, 0x32140000,
			     pds);
}

/*
 * The arm of a blob with the emitpds: its pds0 to pds2 limms hold the control word, the image's
 * address >> 4 and 0, the image is built for the blob's entry 1, the slot is armed as any. Without
 * the image, or with an address the emit cannot name, it is refused and nothing is written.
 */
static void sgxfive_owntask_test_apply_emit(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2 };
	struct sgx_own_blob b;
	unsigned int i, seen = 0;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsa, sizeof(own_ok_emitsa), &b), 0);
	/* refused, nothing written */
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsa, sizeof(own_ok_emitsa),
					    "slots=0x1 entry=0", NULL), -ENOENT);
	pds.addr = OWN_PDS_ADDR + 8;
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsa, sizeof(own_ok_emitsa),
					    "slots=0x1 entry=0", &pds), -EINVAL);
	pds.addr = 0;
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsa, sizeof(own_ok_emitsa),
					    "slots=0x1 entry=0", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);
	KUNIT_EXPECT_EQ(test, dw(page, 0), 0u);
	/* armed */
	pds.addr = OWN_PDS_ADDR;
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsa, sizeof(own_ok_emitsa),
					    "slots=0x1 entry=0", &pds), 0);
	for (i = 0; i < b.n_patch; i++) {
		u16 index;
		u8 kind, slot;
		u32 addend, v;

		sgx_own_patch_get(&b, i, &index, &kind, &slot, &addend);
		if (kind != SGX_OWN_PATCH_PDS)
			continue;
		v = sgx_fw_limm_get(get_unaligned_le64(f->code + 8 * index));
		KUNIT_EXPECT_EQ_MSG(test, v, slot == 0 ? (u32)SGX_OWN_PDS_CTL :
				    slot == 1 ? OWN_PDS_ADDR >> 4 : 0u, "pds%u", slot);
		seen |= 1u << slot;
	}
	KUNIT_EXPECT_EQ(test, seen, 0x7u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U0), sgx_own_pds_u0(sgx_own_entry(&b, 1)));
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_V0), OWN_V0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_V2), OWN_V2);
	KUNIT_EXPECT_EQ(test, dw(f->pds, 4), 0x0804000fu);		/* slot 0, entry 0 */
	/* a blob without the emitpds clears the page, and needs no image */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_mem, sizeof(own_ok_mem), "slots=0x1 entry=0",
					    &pds), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U0), 0u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 8), 0u);
}

/*
 * The arm builds the image own_arm names for emitsb (round 37): pds=crumb the crumb image,
 * no pds= or pds=v1 the v1 image; an image it cannot name is refused, nothing written.
 */
static void sgxfive_owntask_test_apply_emit_image(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2 };
	struct sgx_own_arm a;
	struct sgx_own_blob b;
	u32 u0;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsb, sizeof(own_ok_emitsb), &b), 0);
	KUNIT_EXPECT_TRUE(test, sgx_own_has_emit(&b));
	u0 = sgx_own_pds_u0(sgx_own_entry(&b, 1));
	/* an image index no arm line gives: refused before anything is written */
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse("slots=0x1 entry=0", &a), 0);
	a.img = SGX_OWN_IMG_COUNT;
	KUNIT_EXPECT_EQ(test, sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE,
					    f->code, 0x32140000, &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);
	KUNIT_EXPECT_EQ(test, dw(page, 0), 0u);
	/* crumb */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=crumb", &pds), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U0), u0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_V0), OWN_V0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_V2), OWN_V2);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 7), 0x005a3305u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 8), sgx_own_pds_crumb[8]);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * (SGX_OWN_PDS_CRUMB_WORDS - 1)), 0xaf000000u);
	KUNIT_EXPECT_EQ(test, dw(f->pds, 4), 0x0804000fu);		/* slot 0, entry 0 */
	/* v1, by default and by name: the crumb words are gone */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0", &pds), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U0), u0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 7), 0u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 8), sgx_own_pds_image[8]);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * (SGX_OWN_PDS_WORDS - 1)), 0xaf000000u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_WORDS), 0u);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=crumb", &pds), 0);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=v1", &pds), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 7), 0u);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_WORDS), 0u);
}

/* a blob with the emitpds is not armed without the image it names: refused, nothing written */
static void sgxfive_owntask_test_apply_emit_needs_image(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);

	KUNIT_EXPECT_EQ(test, own_apply(f, own_ok_emitsa, sizeof(own_ok_emitsa), "slots=0x1 entry=0"),
			-ENOENT);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);
}

/* one crafted blob per rule, with its reason (the full list is own_bad) */
static void sgxfive_owntask_test_emit_refusals(struct kunit *test)
{
	static const struct { const u8 *data; size_t len; const char *why; } c[] = {
		{ own_bad_emitpds_sideband, sizeof(own_bad_emitpds_sideband), SGX_OWN_WHY_UNKNOWN },
		{ own_bad_emitpds_sideband_11, sizeof(own_bad_emitpds_sideband_11),
		  SGX_OWN_WHY_UNKNOWN },
		{ own_bad_emitpds_unpatched, sizeof(own_bad_emitpds_unpatched), SGX_OWN_WHY_EMIT_ARGS },
		{ own_bad_emitpds_one_path, sizeof(own_bad_emitpds_one_path), SGX_OWN_WHY_EMIT_ARGS },
		{ own_bad_emitpds_twice, sizeof(own_bad_emitpds_twice), SGX_OWN_WHY_EMIT_ONCE },
		{ own_bad_emitpds_loop, sizeof(own_bad_emitpds_loop), SGX_OWN_WHY_EMIT_ONCE },
		{ own_bad_emitpds_from_entry1, sizeof(own_bad_emitpds_from_entry1), SGX_OWN_WHY_EMIT_ONCE },
		{ own_bad_emitpds_one_entry, sizeof(own_bad_emitpds_one_entry), SGX_OWN_WHY_EMIT_ENTRY },
		{ own_bad_pds_slot3, sizeof(own_bad_pds_slot3), SGX_OWN_WHY_PATCH_SLOT },
		{ own_bad_pds_addend, sizeof(own_bad_pds_addend), SGX_OWN_WHY_PATCH_ADD },
		{ own_bad_pds_base, sizeof(own_bad_pds_base), SGX_OWN_WHY_BASE },
	};
	const char *why;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c); i++) {
		why = "";
		KUNIT_EXPECT_EQ_MSG(test, verify_data(c[i].data, c[i].len, &why), -EPERM, "case %u", i);
		KUNIT_EXPECT_STREQ_MSG(test, why, c[i].why, "case %u", i);
	}
}

/*
 * Round 38 (N3c): halt is v1's data rows and fills, then halt alone; crumb3 is crumb's data rows,
 * a third row of zeros, then crumb's instructions. Each image has its own control word: 2 rows for
 * v1, crumb and halt, 3 (PDS #14's) for crumb3. Zeros after each.
 */
static void sgxfive_owntask_test_pds_build_halt_crumb3(struct kunit *test)
{
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_ctl(SGX_OWN_IMG_V1), 0x01000100u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_ctl(SGX_OWN_IMG_CRUMB), 0x01000100u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_ctl(SGX_OWN_IMG_HALT), 0x01000100u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_ctl(SGX_OWN_IMG_CRUMB3), (3u << 23) | 0x100);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_ctl(SGX_OWN_IMG_COUNT), 0u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_words(SGX_OWN_IMG_V1), 12u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_words(SGX_OWN_IMG_CRUMB), 15u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_words(SGX_OWN_IMG_HALT), 9u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_words(SGX_OWN_IMG_CRUMB3), 19u);
	KUNIT_EXPECT_EQ(test, sgx_own_pds_words(SGX_OWN_IMG_COUNT), 0u);
	/* halt */
	memset(page, 0xa5, SGX_OWN_PDS_SIZE);
	sgx_own_pds_build(page, SGX_OWN_IMG_HALT, 33, OWN_V0, OWN_V2);
	for (i = 0; i < 8; i++) {
		u32 want = sgx_own_pds_image[i];

		if (i == SGX_OWN_PDS_U0)
			want = sgx_own_pds_u0(33);
		else if (i == SGX_OWN_PDS_V0)
			want = OWN_V0;
		else if (i == SGX_OWN_PDS_V2)
			want = OWN_V2;
		KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * i), want, "dword %u", i);
	}
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 8), 0xaf000000u);
	for (i = 4 * 9; i < SGX_OWN_PDS_SIZE; i++)
		if (page[i]) {
			KUNIT_FAIL(test, "halt: byte %u of the page is not zero", i);
			break;
		}
	/* crumb3 */
	memset(page, 0xa5, SGX_OWN_PDS_SIZE);
	sgx_own_pds_build(page, SGX_OWN_IMG_CRUMB3, 33, OWN_V0, OWN_V2);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_U0), sgx_own_pds_u0(33));
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_V0), OWN_V0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * SGX_OWN_PDS_V2), OWN_V2);
	for (i = 0; i < 8; i++)
		if (i != SGX_OWN_PDS_U0 && i != SGX_OWN_PDS_V0 && i != SGX_OWN_PDS_V2)
			KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * i), sgx_own_pds_crumb[i],
					    "dword %u", i);
	for (i = 8; i < 12; i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * i), 0u, "dword %u", i);
	for (i = 0; i < 7; i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(page, 4 * (12 + i)), sgx_own_pds_crumb[8 + i],
				    "insn %u", i);
	for (i = 4 * 19; i < SGX_OWN_PDS_SIZE; i++)
		if (page[i]) {
			KUNIT_FAIL(test, "crumb3: byte %u of the page is not zero", i);
			break;
		}
}

/*
 * pdsat=fw14's place: past PDS #14's buffer and every PATCHES word into it, at the next 0x100,
 * the whole image inside the page; patches into other buffers do not move it.
 */
static void sgxfive_owntask_test_fw14_off(struct kunit *test)
{
	struct sgx_fw *fw = kunit_kzalloc(test, sizeof(*fw), GFP_KERNEL);
	u8 *recs = kunit_kzalloc(test, 3 * 16, GFP_KERNEL);
	u32 off = 0;

	KUNIT_ASSERT_NOT_NULL(test, fw);
	KUNIT_ASSERT_NOT_NULL(test, recs);
	fw->nbuffers = 2;
	fw->buffers[0] = (struct sgx_fw_buffer){ .id = PDS_BUF, .heap = 2, .size = 0x400,
						 .align = 16 };
	fw->buffers[1] = (struct sgx_fw_buffer){ .id = F14_BUF, .heap = 2, .size = F14_SIZE,
						 .align = 16 };
	rec(recs, F14_BUF, 0, 100, PDS_ADDEND);
	rec(recs + 16, F14_BUF, 0x14, 101, PDS_ADDEND);
	rec(recs + 32, PDS_BUF, 0x3fc, 102, PDS_ADDEND);	/* another buffer's */
	fw->patches.rec = recs;
	fw->patches.count = 3;
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_CRUMB3, F14_ROOM, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x100u);
	KUNIT_EXPECT_EQ(test, off % 16, 0u);
	KUNIT_EXPECT_GE(test, off, (u32)F14_SIZE);
	/* exactly 0x100 stays; one byte more moves to 0x200 */
	fw->buffers[1].size = 0x100;
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_V1, F14_ROOM, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x100u);
	fw->buffers[1].size = 0x101;
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_V1, F14_ROOM, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x200u);
	/* a patch word past the size moves it too (a USSE limm is 8 bytes) */
	fw->buffers[1].size = F14_SIZE;
	rec(recs + 16, F14_BUF, 0x1f8, 101, PDS_ADDEND);
	recs[16 + 1] = 1;
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_V1, F14_ROOM, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x200u);
	recs[16 + 1] = 0;
	rec(recs + 16, F14_BUF, 0x1fc, 101, PDS_ADDEND);
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_V1, F14_ROOM, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x200u);
	rec(recs + 16, F14_BUF, 0x14, 101, PDS_ADDEND);
	/* the whole image inside the room: crumb3 is 0x4c bytes */
	off = 0xdead;
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_CRUMB3, 0x14c, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x100u);
	off = 0xdead;
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_CRUMB3, 0x14b, &off),
			-ERANGE);
	KUNIT_EXPECT_EQ(test, off, 0xdeadu);
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, F14_BUF, SGX_OWN_IMG_COUNT, F14_ROOM, &off),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, sgx_own_fw14_off(fw, 6, SGX_OWN_IMG_V1, F14_ROOM, &off), -ENOENT);
}

/* the value the arm wrote in the blob's pds0 limm (the emitpds control word) */
static u32 own_pds0(const struct own_fw *f, const struct sgx_own_blob *b)
{
	unsigned int i;

	for (i = 0; i < b->n_patch; i++) {
		u16 index;
		u8 kind, slot;
		u32 addend;

		sgx_own_patch_get(b, i, &index, &kind, &slot, &addend);
		if (kind == SGX_OWN_PATCH_PDS && slot == 0)
			return sgx_fw_limm_get(get_unaligned_le64(f->code + 8 * index));
	}
	return 0;
}

/* own_fw_new() with PDS #14's buffer (a 0x68-byte template and two code words) in its page */
static struct own_fw *own_fw14_new(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	unsigned int i;

	for (i = 0; i < F14_SIZE; i++)
		f->f14img[i] = 0x11 + 3 * i;
	f->fw.buffers[2] = (struct sgx_fw_buffer){ .id = F14_BUF, .heap = 2, .size = F14_SIZE,
						   .align = 16, .image = f->f14img,
						   .image_size = F14_SIZE };
	f->fw.nbuffers = 3;
	rec(f->recs + 4 * 16, F14_BUF, 0x00, 100, PDS_ADDEND);
	rec(f->recs + 5 * 16, F14_BUF, 0x14, 101, PDS_ADDEND);
	f->fw.patches.count = 6;
	f->l.addr[F14_BUF] = F14_ADDR;
	f->l.addr_set |= 1u << F14_BUF;
	f->bufs[F14_BUF] = f->f14;
	KUNIT_ASSERT_EQ(test, sgx_fw_build(&f->fw, &f->l, f->bufs), 0);
	KUNIT_ASSERT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	memcpy(f->stock14, f->f14, F14_ROOM);
	return f;
}

static bool zero(const u8 *p, unsigned int from, unsigned int to)
{
	for (; from < to; from++)
		if (p[from])
			return false;
	return true;
}

/*
 * pdsat=fw14: the image the arm names at #14's base + 0x100, its words with U0, V0, V2; #14's
 * own bytes unchanged; the rest of the slack zero (a stale word of ours too); the own page zero;
 * pds0 the image's control word and pds1 the new address >> 4, 16-aligned. An arm back in the
 * own page leaves the slack zero.
 */
static void sgxfive_owntask_test_apply_fw14(struct kunit *test)
{
	struct own_fw *f = own_fw14_new(test);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2,
				   .fw14 = f->f14, .fw14_addr = F14_ADDR, .fw14_room = F14_ROOM };
	struct sgx_own_blob b;
	unsigned int i, seen;
	u32 u0, at = F14_ADDR + 0x100;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsb, sizeof(own_ok_emitsb), &b), 0);
	u0 = sgx_own_pds_u0(sgx_own_entry(&b, 1));
	memset(f->f14 + 0x400, 0xa5, 0x40);		/* a stale word in the slack */
	memset(page, 0xa5, SGX_OWN_PDS_SIZE);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=crumb3 pdsat=fw14", &pds), 0);
	KUNIT_EXPECT_EQ(test, pds.at, at);
	KUNIT_EXPECT_EQ(test, pds.at & 15, 0u);
	seen = 0;
	for (i = 0; i < b.n_patch; i++) {
		u16 index;
		u8 kind, slot;
		u32 addend, v;

		sgx_own_patch_get(&b, i, &index, &kind, &slot, &addend);
		if (kind != SGX_OWN_PATCH_PDS)
			continue;
		v = sgx_fw_limm_get(get_unaligned_le64(f->code + 8 * index));
		KUNIT_EXPECT_EQ_MSG(test, v, slot == 0 ? 0x01800100u : slot == 1 ? at >> 4 : 0u,
				    "pds%u", slot);
		seen |= 1u << slot;
	}
	KUNIT_EXPECT_EQ(test, seen, 0x7u);
	KUNIT_EXPECT_EQ(test, memcmp(f->f14, f->stock14, F14_SIZE), 0);	/* #14's own bytes */
	KUNIT_EXPECT_TRUE(test, zero(f->f14, F14_SIZE, 0x100));
	KUNIT_EXPECT_EQ(test, dw(f->f14, 0x100 + 4 * SGX_OWN_PDS_U0), u0);
	KUNIT_EXPECT_EQ(test, dw(f->f14, 0x100 + 4 * SGX_OWN_PDS_V0), OWN_V0);
	KUNIT_EXPECT_EQ(test, dw(f->f14, 0x100 + 4 * SGX_OWN_PDS_V2), OWN_V2);
	KUNIT_EXPECT_EQ(test, dw(f->f14, 0x100 + 4 * 7), 0x005a3305u);
	for (i = 8; i < 12; i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(f->f14, 0x100 + 4 * i), 0u, "dword %u", i);
	for (i = 12; i < 19; i++)
		KUNIT_EXPECT_EQ_MSG(test, dw(f->f14, 0x100 + 4 * i), sgx_own_pds_crumb3[i],
				    "dword %u", i);
	KUNIT_EXPECT_TRUE(test, zero(f->f14, 0x100 + 4 * 19, F14_ROOM));
	KUNIT_EXPECT_TRUE(test, zero(page, 0, SGX_OWN_PDS_SIZE));
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE) != 0, true);	/* slot 0 armed */
	KUNIT_EXPECT_EQ(test, dw(f->pds, 4), 0x0804000fu);
	/* crumb (2 rows) in the same place: v1's control word */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=crumb pdsat=fw14", &pds), 0);
	KUNIT_EXPECT_EQ(test, pds.at, at);
	KUNIT_EXPECT_EQ(test, dw(f->f14, 0x100 + 4 * 8), sgx_own_pds_crumb[8]);
	KUNIT_EXPECT_TRUE(test, zero(f->f14, 0x100 + 4 * 15, F14_ROOM));
	KUNIT_EXPECT_EQ(test, own_pds0(f, &b), 0x01000100u);
	/* back in the own page: #14's slack zero, #14's bytes as the build left them */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=crumb", &pds), 0);
	KUNIT_EXPECT_EQ(test, pds.at, OWN_PDS_ADDR);
	KUNIT_EXPECT_EQ(test, memcmp(f->f14, f->stock14, F14_ROOM), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 7), 0x005a3305u);
	/* a blob without the emit names nothing */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_mem, sizeof(own_ok_mem), "slots=0x1 entry=0",
					    &pds), 0);
	KUNIT_EXPECT_EQ(test, pds.at, 0u);
}

/*
 * pdsat=fw14 refused, nothing written: a blob without the emitpds, no #14 page, no room in it,
 * #14's buffer not in the firmware.
 */
static void sgxfive_owntask_test_apply_fw14_refusals(struct kunit *test)
{
	struct own_fw *f = own_fw14_new(test);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2,
				   .fw14 = f->f14, .fw14_addr = F14_ADDR, .fw14_room = F14_ROOM };

	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0x5a, SGX_OWN_PDS_SIZE);
	memset(f->f14 + 0x100, 0xa5, 4);
	memcpy(f->stock14, f->f14, F14_ROOM);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_mem, sizeof(own_ok_mem),
					    "slots=0x1 entry=0 pdsat=fw14", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsn, sizeof(own_ok_emitsn),
					    "slots=0x1 entry=0 pdsat=fw14", &pds), -EINVAL);
	pds.fw14 = NULL;
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    ARM14, &pds), -ENOENT);
	pds.fw14 = f->f14;
	pds.fw14_addr = 0;
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    ARM14, &pds), -ENOENT);
	pds.fw14_addr = F14_ADDR;
	pds.fw14_room = 0x140;
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    ARM14, &pds), -ERANGE);
	pds.fw14_room = F14_ROOM;
	f->fw.nbuffers = 2;				/* no #14 in the firmware */
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    ARM14, &pds), -ENOENT);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);
	KUNIT_EXPECT_EQ(test, memcmp(f->f14, f->stock14, F14_ROOM), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 0), 0x5a5a5a5au);
}

/*
 * The disarm: a recovery reset rebuilds the firmware's buffers, which rewrites #14's 0x68 bytes
 * only (an image past them stays), then clears the slack: no word of ours is left in the page.
 */
static void sgxfive_owntask_test_fw14_disarm(struct kunit *test)
{
	struct own_fw *f = own_fw14_new(test);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2,
				   .fw14 = f->f14, .fw14_addr = F14_ADDR, .fw14_room = F14_ROOM };

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=crumb3 pdsat=fw14", &pds), 0);
	KUNIT_ASSERT_EQ(test, sgx_fw_build(&f->fw, &f->l, f->bufs), 0);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_NE(test, dw(f->f14, 0x100 + 4 * 12), 0u);	/* the build leaves it */
	sgx_own_fw14_clear(f->f14, F14_SIZE, F14_ROOM);
	KUNIT_EXPECT_EQ(test, memcmp(f->f14, f->stock14, F14_ROOM), 0);
	KUNIT_EXPECT_TRUE(test, zero(f->f14, F14_SIZE, F14_ROOM));
	/* no page, or no slack: nothing to do */
	sgx_own_fw14_clear(NULL, F14_SIZE, F14_ROOM);
	sgx_own_fw14_clear(f->f14, F14_ROOM, F14_ROOM);
	KUNIT_EXPECT_EQ(test, memcmp(f->f14, f->stock14, F14_ROOM), 0);
}

/* pds=halt in the own page: v1's control word, the halt image */
static void sgxfive_owntask_test_apply_halt(struct kunit *test)
{
	struct own_fw *f = own_fw_new(test, 4);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0,
				   .v2 = OWN_V2 };
	struct sgx_own_blob b;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsb, sizeof(own_ok_emitsb), &b), 0);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds=halt", &pds), 0);
	KUNIT_EXPECT_EQ(test, pds.at, OWN_PDS_ADDR);
	KUNIT_EXPECT_EQ(test, dw(page, 4 * 8), 0xaf000000u);
	KUNIT_EXPECT_TRUE(test, zero(page, 4 * 9, SGX_OWN_PDS_SIZE));
	KUNIT_EXPECT_EQ(test, own_pds0(f, &b), 0x01000100u);
}

/* ---- round 39 (N3d row Z): the emit's s2 and its second word ---------------------------------- */
#define OWN_FW14_WORD	0xfb20b10800c08184ULL	/* the emitpds-fw14 row: sideband 0x0843 */

/* the value the arm wrote in the blob's pds<slot> limm */
static u32 own_pdsn(const struct own_fw *f, const struct sgx_own_blob *b, u8 want)
{
	unsigned int i;

	for (i = 0; i < b->n_patch; i++) {
		u16 index;
		u8 kind, slot;
		u32 addend;

		sgx_own_patch_get(b, i, &index, &kind, &slot, &addend);
		if (kind == SGX_OWN_PATCH_PDS && slot == want)
			return sgx_fw_limm_get(get_unaligned_le64(f->code + 8 * index));
	}
	return 0xdeadbeef;
}

/*
 * The second word is an emitpds as the first (the verifier's one-emit rules hold across both): a
 * blob with it has the emit, of form fw14; emitsk's word is the first, of form own; a blob
 * without an emit has none. Two emits, one of each word, are two.
 */
static void sgxfive_owntask_test_emit_form(struct kunit *test)
{
	struct sgx_own_blob b;
	unsigned int i, n = 0;
	const char *why = "";

	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsk14, sizeof(own_ok_emitsk14), &b), 0);
	KUNIT_EXPECT_EQ(test, sgx_own_verify(&b, &why), 0);
	KUNIT_EXPECT_TRUE(test, sgx_own_has_emit(&b));
	KUNIT_EXPECT_EQ(test, sgx_own_emit_form(&b), (unsigned int)SGX_OWN_FORM_FW14);
	for (i = 0; i < b.n_insn; i++)
		n += sgx_own_insn(&b, i) == OWN_FW14_WORD;
	KUNIT_EXPECT_EQ(test, n, 1u);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsk, sizeof(own_ok_emitsk), &b), 0);
	KUNIT_EXPECT_TRUE(test, sgx_own_has_emit(&b));
	KUNIT_EXPECT_EQ(test, sgx_own_emit_form(&b), (unsigned int)SGX_OWN_FORM_OWN);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsn, sizeof(own_ok_emitsn), &b), 0);
	KUNIT_EXPECT_FALSE(test, sgx_own_has_emit(&b));
	KUNIT_EXPECT_EQ(test, sgx_own_emit_form(&b), (unsigned int)SGX_OWN_FORM_COUNT);
}

/*
 * pds2=req: the arm writes 0x00020000 in the blob's pds2 limm (s2), pds0 and pds1 as before;
 * form=fw14 arms a blob whose emit is the second word. Both with crumb3 in #14's page: row Z.
 */
static void sgxfive_owntask_test_apply_pds2_form(struct kunit *test)
{
	struct own_fw *f = own_fw14_new(test);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2,
				   .fw14 = f->f14, .fw14_addr = F14_ADDR, .fw14_room = F14_ROOM };
	struct sgx_own_blob b;
	unsigned int i, n;
	u32 at = F14_ADDR + 0x100;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsk14, sizeof(own_ok_emitsk14), &b), 0);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsk14, sizeof(own_ok_emitsk14),
					    ARM14 " pds2=req form=fw14", &pds), 0);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 0), 0x01800100u);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 1), at >> 4);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 2), 0x00020000u);
	KUNIT_EXPECT_EQ(test, pds.at, at);
	KUNIT_EXPECT_EQ(test, dw(f->f14, 0x100 + 4 * SGX_OWN_PDS_U0),
			sgx_own_pds_u0(sgx_own_entry(&b, 1)));
	KUNIT_EXPECT_EQ(test, dw(f->pds, 4), 0x0804000fu);		/* slot 0, entry 0 */
	/* the emit word itself is the blob's: the arm patches limms only */
	for (i = 0, n = 0; i < b.n_insn; i++)
		n += get_unaligned_le64(f->code + 8 * i) == OWN_FW14_WORD;
	KUNIT_EXPECT_EQ(test, n, 1u);
	/* the second word at pds2=0: s2 0 */
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsk14, sizeof(own_ok_emitsk14),
					    ARM14 " form=fw14", &pds), 0);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 2), 0u);
	/* the first word at pds2=req, its own place and image */
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsk, sizeof(own_ok_emitsk), &b), 0);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsk, sizeof(own_ok_emitsk),
					    "slots=0x1 entry=0 pds2=req", &pds), 0);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 0), (u32)SGX_OWN_PDS_CTL);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 1), OWN_PDS_ADDR >> 4);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 2), 0x00020000u);
	KUNIT_ASSERT_EQ(test, own_apply_pds(f, own_ok_emitsk, sizeof(own_ok_emitsk),
					    "slots=0x1 entry=0", &pds), 0);
	KUNIT_EXPECT_EQ(test, own_pdsn(f, &b, 2), 0u);
}

/*
 * Refused, nothing written: the second word without form=fw14, form=fw14 for the first word,
 * pds2=req or form=fw14 for a blob without the emit; an arm with indices no line gives.
 */
static void sgxfive_owntask_test_apply_pds2_form_refusals(struct kunit *test)
{
	struct own_fw *f = own_fw14_new(test);
	u8 *page = kunit_kzalloc(test, SGX_OWN_PDS_SIZE, GFP_KERNEL);
	struct sgx_own_pds pds = { .image = page, .addr = OWN_PDS_ADDR, .v0 = OWN_V0, .v2 = OWN_V2,
				   .fw14 = f->f14, .fw14_addr = F14_ADDR, .fw14_room = F14_ROOM };
	struct sgx_own_arm a;
	struct sgx_own_blob b;

	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0x5a, SGX_OWN_PDS_SIZE);
	memset(f->f14 + 0x100, 0xa5, 4);
	memcpy(f->stock14, f->f14, F14_ROOM);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsk14, sizeof(own_ok_emitsk14),
					    ARM14, &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsk14, sizeof(own_ok_emitsk14),
					    ARM14 " pds2=req", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsk14, sizeof(own_ok_emitsk14),
					    ARM14 " form=own", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsk, sizeof(own_ok_emitsk),
					    ARM14 " form=fw14", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsb, sizeof(own_ok_emitsb),
					    "slots=0x1 entry=0 pds2=req form=fw14", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_mem, sizeof(own_ok_mem),
					    "slots=0x1 entry=0 pds2=req", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_mem, sizeof(own_ok_mem),
					    "slots=0x1 entry=0 form=fw14", &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, own_apply_pds(f, own_ok_emitsn, sizeof(own_ok_emitsn),
					    "slots=0x1 entry=0 pds2=req", &pds), -EINVAL);
	KUNIT_ASSERT_EQ(test, sgx_own_parse(own_ok_emitsk14, sizeof(own_ok_emitsk14), &b), 0);
	KUNIT_ASSERT_EQ(test, sgx_own_arm_parse(ARM14 " pds2=req form=fw14", &a), 0);
	a.pds2 = SGX_OWN_PDS2_COUNT;
	KUNIT_EXPECT_EQ(test, sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE,
					    f->code, 0x32140000, &pds), -EINVAL);
	a.pds2 = SGX_OWN_PDS2_REQ;
	a.form = SGX_OWN_FORM_COUNT;
	KUNIT_EXPECT_EQ(test, sgx_own_apply(&b, &a, &f->fw, &f->l, PDS_BUF, f->pds, PDS_SIZE,
					    f->code, 0x32140000, &pds), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(f->pds, f->stock, PDS_SIZE), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->code), 0ull);
	KUNIT_EXPECT_EQ(test, memcmp(f->f14, f->stock14, F14_ROOM), 0);
	KUNIT_EXPECT_EQ(test, dw(page, 0), 0x5a5a5a5au);
}

static struct kunit_case sgxfive_owntask_test_cases[] = {
	KUNIT_CASE(sgxfive_owntask_test_accepts_our_blobs),
	KUNIT_CASE(sgxfive_owntask_test_refuses_crafted_blobs),
	KUNIT_CASE(sgxfive_owntask_test_forms_are_canonical),
	KUNIT_CASE(sgxfive_owntask_test_clock_gate),
	KUNIT_CASE(sgxfive_owntask_test_register_lists),
	KUNIT_CASE(sgxfive_owntask_test_deny_rows),
	KUNIT_CASE(sgxfive_owntask_test_mov_pa),
	KUNIT_CASE(sgxfive_owntask_test_exit_words),
	KUNIT_CASE(sgxfive_owntask_test_parse),
	KUNIT_CASE(sgxfive_owntask_test_apply_only_armed_dwords),
	KUNIT_CASE(sgxfive_owntask_test_apply_default_trc_and_entry),
	KUNIT_CASE(sgxfive_owntask_test_apply_patches),
	KUNIT_CASE(sgxfive_owntask_test_apply_refusals),
	KUNIT_CASE(sgxfive_owntask_test_exit_index),
	KUNIT_CASE(sgxfive_owntask_test_apply_cont4),
	KUNIT_CASE(sgxfive_owntask_test_apply_reverifies_and_wants_one_record),
	KUNIT_CASE(sgxfive_owntask_test_arm_parse),
	KUNIT_CASE(sgxfive_owntask_test_res_write),
	KUNIT_CASE(sgxfive_owntask_test_has_emit),
	KUNIT_CASE(sgxfive_owntask_test_apply_emit_needs_image),
	KUNIT_CASE(sgxfive_owntask_test_pds_build),
	KUNIT_CASE(sgxfive_owntask_test_pds_build_crumb),
	KUNIT_CASE(sgxfive_owntask_test_apply_emit_image),
	KUNIT_CASE(sgxfive_owntask_test_apply_emit),
	KUNIT_CASE(sgxfive_owntask_test_emit_refusals),
	KUNIT_CASE(sgxfive_owntask_test_pds_build_halt_crumb3),
	KUNIT_CASE(sgxfive_owntask_test_fw14_off),
	KUNIT_CASE(sgxfive_owntask_test_apply_fw14),
	KUNIT_CASE(sgxfive_owntask_test_apply_fw14_refusals),
	KUNIT_CASE(sgxfive_owntask_test_fw14_disarm),
	KUNIT_CASE(sgxfive_owntask_test_apply_halt),
	KUNIT_CASE(sgxfive_owntask_test_emit_form),
	KUNIT_CASE(sgxfive_owntask_test_apply_pds2_form),
	KUNIT_CASE(sgxfive_owntask_test_apply_pds2_form_refusals),
	{}
};

static struct kunit_suite sgxfive_owntask_test_suite = {
	.name = "gma500_sgxfive_owntask",
	.test_cases = sgxfive_owntask_test_cases,
};
kunit_test_suite(sgxfive_owntask_test_suite);
