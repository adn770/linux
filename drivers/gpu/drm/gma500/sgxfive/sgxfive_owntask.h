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

enum { SGX_OWN_PATCH_RES = 1, SGX_OWN_PATCH_CONT = 2 };

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
u16 sgx_own_entry(const struct sgx_own_blob *b, unsigned int i);
void sgx_own_patch_get(const struct sgx_own_blob *b, unsigned int i, u16 *index, u8 *kind,
		       u8 *slot, u32 *addend);

/* what a register index may be used for (sgx_own_regs.h) */
bool sgx_own_reg_ok(unsigned int index, bool store);
bool sgx_own_clockgate(unsigned int index);

#endif
