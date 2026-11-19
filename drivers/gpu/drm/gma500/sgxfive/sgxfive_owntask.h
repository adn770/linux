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
struct sgx_fw;

/*
 * Where the image sits, own_arm's pdsat= (round 38, N3c): own, the default, the page above; fw14,
 * the slack of the firmware's PDS #14 page, past #14's buffer and every PATCHES word in it at the
 * next SGX_OWN_FW14_ALIGN (sgx_own_fw14_off()). That page is #14's own: the same PTE and flags
 * (read-write, 0) as a program the firmware emits on every kick. The slack is zeroed at every arm
 * that gives the page and by the recovery reset (sgx_own_fw14_clear()), since the firmware's
 * build rewrites #14's buffer only, so no word of ours outlives the run.
 */
#define SGXFIVE_OWN_FW14_BUF	5		/* PDS #14's buffer id (fw_format section 3) */
#define SGX_OWN_FW14_ALIGN	0x100

enum sgx_own_at {
	SGX_OWN_AT_OWN,
	SGX_OWN_AT_FW14,
	SGX_OWN_AT_COUNT,
};

struct sgx_own_pds {
	u8 *image;			/* the CPU view of the image's page (SGX_OWN_PDS_SIZE) */
	u32 addr;			/* its device address, 16-byte aligned */
	u32 v0, v2;			/* TA3DCtl dwords 0 and 2: the values sa0 and sa2 hold */
	u8 *fw14;			/* the CPU view of PDS #14's buffer (NULL: none) */
	u32 fw14_addr;			/* its device address */
	u32 fw14_room;			/* its mapped bytes, whole pages */
	u32 at;				/* set by sgx_own_apply(): the emit's address, or 0 */
};

/*
 * The images own_arm may name (pds=, sgx_own_pds.h): v1, the default, the program above; crumb
 * (round 37) the same with three movs tim breadcrumbs, T1 = 0 first, T2 = 0x200 after both douta,
 * T3 = 0x005a3305 after the doutu. Each has bit 24 clear: tim (EVENT_TIMER) holds it and counts
 * nothing, so the EDM tick stops at T1 until the next reset, and the host reads how far it ran.
 * halt (round 38) is v1's data rows and a lone halt; crumb3 is crumb with a third data row of
 * zeros, 3 rows as PDS #14 has, so its control word is (3 << 23) | 0x100.
 */
enum sgx_own_img {
	SGX_OWN_IMG_V1,
	SGX_OWN_IMG_CRUMB,
	SGX_OWN_IMG_HALT,
	SGX_OWN_IMG_CRUMB3,
	SGX_OWN_IMG_COUNT,
};

/* own_arm's name of an image ("v1", "crumb", "halt", "crumb3"; "?" for none) */
const char *sgx_own_img_name(unsigned int img);
/* own_arm's name of a place ("own", "fw14"; "?" for none) */
const char *sgx_own_at_name(unsigned int at);
/* the image's emitpds control word, (rows << 23) | 0x100 (0 for none) */
u32 sgx_own_pds_ctl(unsigned int img);
/* the image's size in dwords (0 for none) */
unsigned int sgx_own_pds_words(unsigned int img);
/* the doutu word of the task at the blob's instruction `entry` */
u32 sgx_own_pds_u0(unsigned int entry);
/*
 * the image's page: the generated words of image `img` with U0 for `entry`, V0 and V2; zeros
 * after them (all zeros for an image that is not one of enum sgx_own_img)
 */
void sgx_own_pds_build(u8 *page, unsigned int img, unsigned int entry, u32 v0, u32 v2);
/*
 * pdsat=fw14: the byte offset in buffer `buf` (PDS #14's) where image `img` goes, past the
 * buffer's size and every PATCHES record's word in it, rounded up to SGX_OWN_FW14_ALIGN. -ENOENT
 * without the buffer, -EINVAL for an image that is none, -ERANGE when it does not fit in `room`.
 */
int sgx_own_fw14_off(const struct sgx_fw *fw, unsigned int buf, unsigned int img, u32 room,
		     u32 *off);
/* zero bytes [size, room) of PDS #14's page: whatever the slack holds, #14's own bytes kept */
void sgx_own_fw14_clear(u8 *page, u32 size, u32 room);

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
	u8 img;				/* the emitpds image (enum sgx_own_img): pds=, v1 if not given */
	u8 at;				/* where it sits (enum sgx_own_at): pdsat=, own if none */
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

/*
 * "slots=<mask> entry=<n>,<n>.. [trc=<t>] [trc_hazard=1] [pds=v1|crumb|halt|crumb3]
 * [pdsat=own|fw14]"
 */
int sgx_own_arm_parse(const char *line, struct sgx_own_arm *a);
/*
 * pds: the image's page, needed by a blob with the emitpds (-ENOENT without it, -EINVAL for an
 * address that is 0 or not 16-byte aligned, nothing written either way); the page is built for
 * such a blob, with the image the arm names (-EINVAL for one that is not an enum sgx_own_img,
 * nothing written), and cleared for any other. NULL for a blob without the emitpds.
 * pdsat=fw14 needs a blob with the emitpds (-EINVAL), PDS #14's buffer in pds->fw14 (-ENOENT) and
 * room for the image past it (-ERANGE), nothing written otherwise; the image is then written
 * there and the own page cleared. #14's slack is cleared whenever pds->fw14 is given. pds->at is
 * the address the emit names (0: none).
 */
int sgx_own_apply(const struct sgx_own_blob *b, const struct sgx_own_arm *a,
		  const struct sgx_fw *fw, const struct sgx_fw_layout *l, unsigned int pds_buf,
		  u8 *pds_image, u32 pds_size, u8 *code, u32 res_addr,
		  struct sgx_own_pds *pds);
/* own_res: a write must be dword aligned and leave dword 0 (the header) alone */
bool sgx_own_res_write_ok(loff_t pos, size_t len);

#endif
