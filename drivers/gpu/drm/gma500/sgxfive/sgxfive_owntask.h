/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The debug group's own firmware tasks (CONFIG_DRM_GMA500_SGXFIVE_DEBUG): a blob of USSE code
 * of a tester's, checked here and run as an EDM task from a page of its own, in place of what
 * a slot of PDS #13 started (sgxfive_debug.c: own_code, own_res and own_arm).
 *
 * A blob is the 16-byte header "SGXOWN01", u16 n_insn, u16 n_patch, u16 n_entry, u16 0, then
 * n_insn 64-bit instruction words, n_patch patch records (u16 index, u8 kind, u8 slot, u32
 * addend) and n_entry u16 entry points (instruction indices in the blob). It is written by
 * tools/fwtask in resgx545; the verifier below is default-deny and repeats tools/fwtask's, from
 * the same generated table (sgx_own_regs.h).
 */
#ifndef _SGXFIVE_OWNTASK_H_
#define _SGXFIVE_OWNTASK_H_

#include <linux/types.h>

#define SGX_OWN_MAX_INSNS	1024
#define SGX_OWN_MAX_PATCHES	256
#define SGX_OWN_MAX_ENTRIES	4
#define SGX_OWN_MAX_BYTES	(16 + 8 * SGX_OWN_MAX_INSNS + 8 * SGX_OWN_MAX_PATCHES + \
				 2 * SGX_OWN_MAX_ENTRIES)

/* the blob's code is run from the KERNEL_CODE heap base + SGX_OWN_CODE_OFF (index OWN_INDEX) */
#define SGX_OWN_CODE_OFF	0x20000
#define SGX_OWN_INDEX		(SGX_OWN_CODE_OFF / 8)
#define SGX_OWN_CODE_SIZE	(8 * SGX_OWN_MAX_INSNS)
#define SGX_OWN_RES_OFF		0x140000		/* KERNEL_DATA */
#define SGX_OWN_RES_SIZE	0x1000
#define SGX_OWN_MAGIC_DWORD	0x4e574f53		/* "SOWN", dword 0 of the results page */

enum { SGX_OWN_PATCH_RES = 1, SGX_OWN_PATCH_CONT = 2, SGX_OWN_PATCH_PDS = 3 };

/*
 * The one emitpds form a blob may hold (sgx_own_regs.h's emitpds-own row: exactly
 * emitpds.tasks.taske #0, r2, r3, r4, #0x0): its sources r2, r3 and r4 hold, on every path, the
 * values of the blob's pds0, pds1 and pds2 patches (kind SGX_OWN_PATCH_PDS, slot 0 to 2, no
 * addend), which the overlay writes. It runs once per task: there is at most one, no path from
 * it or from the blob's entry 1 reaches it again, and a blob that has it has an entry 1.
 */
#define SGX_OWN_PDS_SLOTS	3
#define SGX_OWN_EMIT_ENTRY	1

/*
 * The program the emitpds names is the kernel's (sgx_own_pds.h, from resgx545's
 * asm/fwtask/pds/emit.s): pds0 is its control word (2 data rows, addend 0x100), pds1 its address
 * shifted right by four, pds2 0. It sits in a page of its own beside the code page, read-only for
 * the EDM and written by the overlay only. Its two douta write the values sa0 and sa2 hold
 * (TA3DCtl dwords 0 and 2, which PDS #11 loads into the secondary attributes) to attributes 0
 * and 2, so wherever they land no value the firmware reads changes; its doutu starts the blob's
 * entry 1 on the code page with no flag.
 */
#define SGX_OWN_PDS_OFF		0x22000		/* KERNEL_CODE */
#define SGX_OWN_PDS_SIZE	0x1000
#define SGX_OWN_PDS_TRC		8		/* the started task's temporaries, in fours */

struct sgx_own_blob;

struct sgx_own_pds {
	u8 *image;			/* the CPU view of the image's page (SGX_OWN_PDS_SIZE) */
	u32 addr;			/* its device address, 16-byte aligned */
	u32 v0, v2;			/* TA3DCtl dwords 0 and 2: the values sa0 and sa2 hold */
};

/* the doutu word of the task at the blob's instruction `entry` */
u32 sgx_own_pds_u0(unsigned int entry);
/* the image's page: the generated words with U0 for `entry`, V0 and V2; zeros after them */
void sgx_own_pds_build(u8 *page, unsigned int entry, u32 v0, u32 v2);

/*
 * A continue patch's slot 0 to 3 is the closed task a slot of PDS #13 started; slot 4 is the
 * closed tasks' shared exit, at a ukernel index read from one firmware file only (E154): it is
 * resolved for that file alone (sgx_own_exit_index()), and an arm that needs it on any other file
 * is refused.
 */
#define SGX_OWN_EXIT_SLOT	4
#define SGX_OWN_EXIT_UK_INDEX	0x1974

struct sgx_own_blob {
	u16 n_insn, n_patch, n_entry;
	const u8 *insn;
	const u8 *patch;
	const u8 *entry;
};

int sgx_own_parse(const u8 *data, size_t len, struct sgx_own_blob *b);
/* 0, or -EPERM with the first reason in *why */
int sgx_own_verify(const struct sgx_own_blob *b, const char **why);

u64 sgx_own_insn(const struct sgx_own_blob *b, unsigned int i);
/* whether the blob holds the emitpds word (a verified blob: at most one) */
bool sgx_own_has_emit(const struct sgx_own_blob *b);
u16 sgx_own_entry(const struct sgx_own_blob *b, unsigned int i);
void sgx_own_patch_get(const struct sgx_own_blob *b, unsigned int i, u16 *index, u8 *kind,
		       u8 *slot, u32 *addend);

/* what a register index may be used for (sgx_own_regs.h) */
bool sgx_own_reg_ok(unsigned int index, bool store);
bool sgx_own_clockgate(unsigned int index);

/*
 * The overlay: what own_arm asks for, and sgx_own_apply() which puts the code in its page and
 * retargets the armed slots of PDS #13 (sgxfive_own_apply() in sgxfive_debug.c calls it from the
 * firmware load, after the buffers are built, and nowhere else: a recovery reset builds the
 * stock buffers again and the stock EDM runs).
 */
#define SGX_OWN_TRC_STOCK	0	/* not given: keep the stock word's TRC byte */
#define SGX_OWN_TRC_FREE_MAX	31	/* up to here a tester arms freely */
#define SGX_OWN_SLOTS		4	/* PDS #13 starts one of four tasks */
#define SGXFIVE_OWN_PDS_BUF	4	/* PDS #13's buffer id (fw_format section 3) */

struct sgx_own_arm {
	u32 slots;			/* bit k: the k-th closed task, in increasing index order */
	u8 entry[SGX_OWN_SLOTS];	/* the blob's entry point for each armed slot, ascending */
	unsigned int nentry;
	u8 trc;				/* TRC field: units of 4 temporaries; 0: the stock word's */
	bool trc_hazard;
	u32 exit_index;			/* the shared exit's ukernel index, set by the load (not by
					 * own_arm): 0, unknown, refuses a cont4 patch */
};

/* the debug group's own-task state, in struct sgxfive */
struct sgxfive_own {
	u8 blob[SGX_OWN_MAX_BYTES];	/* a verified blob */
	size_t len;			/* 0: none */
	bool armed;
	struct sgx_own_arm arm;
};

struct sgx_fw;
struct sgx_fw_layout;

/* SGX_OWN_EXIT_UK_INDEX for the firmware file of that sha256 digest, else 0 */
u32 sgx_own_exit_index(const u8 *digest);

/* "slots=<mask> entry=<n>,<n>.. [trc=<t>] [trc_hazard=1]" */
int sgx_own_arm_parse(const char *line, struct sgx_own_arm *a);
/*
 * pds: the image's page, needed by a blob with the emitpds (-ENOENT without it, -EINVAL for an
 * address that is 0 or not 16-byte aligned, nothing written either way); the page is built for
 * such a blob and cleared for any other. NULL for a blob without the emitpds.
 */
int sgx_own_apply(const struct sgx_own_blob *b, const struct sgx_own_arm *a,
		  const struct sgx_fw *fw, const struct sgx_fw_layout *l, unsigned int pds_buf,
		  u8 *pds_image, u32 pds_size, u8 *code, u32 res_addr,
		  const struct sgx_own_pds *pds);
/* own_res: a write must be dword aligned and leave dword 0 (the header) alone */
bool sgx_own_res_write_ok(loff_t pos, size_t len);

#endif
