// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The debug group's own firmware tasks: the blob parser and the verifier (sgxfive_owntask.h).
 * Default deny: a word is accepted only if it is canonical, a form of the table in
 * sgx_own_regs.h with every bit that no field of the form names pinned, and an ldr or str of an
 * immediate index only on a register of its lists. The clock-gate registers are refused by
 * name, for ldr and str alike, before anything else.
 */
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "sgx_own_regs.h"
#include "sgxfive_owntask.h"

/* the field layouts of the forms (doc/specs/usse_isa.md) */
static unsigned int own_reg_index(u64 w)
{
	return (w & 0x7f) | (w >> 14 & 0x7f) << 7;
}

static int own_br_target(unsigned int i, u64 w)
{
	int off = w & 0x3ffff;

	if (off & 0x20000)
		off -= 0x40000;
	return i + off;
}

static unsigned int own_dst(u64 w)
{
	return w >> 21 & 0x7f;
}

static unsigned int own_base(u64 w)
{
	return w >> 14 & 0x7f;
}

static unsigned int own_off(u64 w)
{
	return w >> 7 & 0x7f;
}

static unsigned int own_mov_pclink_src(u64 w)
{
	return w >> 7 & 0x7f;
}

#define OWN_TRACKED	32		/* the registers a base may name: the stock 32 temporaries */
#define OWN_TOP		0xffff		/* a register of the base analysis that holds no page offset */

bool sgx_own_clockgate(unsigned int index)
{
	unsigned int i;

	index &= 0x3fff;
	for (i = 0; i < ARRAY_SIZE(sgx_own_clockgate_regs); i++)
		if (sgx_own_clockgate_regs[i] == index)
			return true;
	return false;
}

bool sgx_own_reg_ok(unsigned int index, bool store)
{
	const u16 *list = store ? sgx_own_str_regs : sgx_own_ldr_regs;
	unsigned int n = store ? ARRAY_SIZE(sgx_own_str_regs) : ARRAY_SIZE(sgx_own_ldr_regs);
	unsigned int i;

	index &= 0x3fff;
	if (sgx_own_clockgate(index))
		return false;
	for (i = 0; i < n; i++)
		if (list[i] == index)
			return true;
	return false;
}

int sgx_own_parse(const u8 *data, size_t len, struct sgx_own_blob *b)
{
	size_t need;

	if (len < 16 || memcmp(data, "SGXOWN01", 8))
		return -EINVAL;
	b->n_insn = get_unaligned_le16(data + 8);
	b->n_patch = get_unaligned_le16(data + 10);
	b->n_entry = get_unaligned_le16(data + 12);
	if (!b->n_insn || b->n_insn > SGX_OWN_MAX_INSNS || b->n_patch > SGX_OWN_MAX_PATCHES ||
	    b->n_entry > SGX_OWN_MAX_ENTRIES)
		return -EINVAL;
	need = 16 + 8 * (size_t)b->n_insn + 8 * (size_t)b->n_patch + 2 * (size_t)b->n_entry;
	if (len != need)
		return -EINVAL;
	b->insn = data + 16;
	b->patch = b->insn + 8 * (size_t)b->n_insn;
	b->entry = b->patch + 8 * (size_t)b->n_patch;
	return 0;
}

u64 sgx_own_insn(const struct sgx_own_blob *b, unsigned int i)
{
	return get_unaligned_le64(b->insn + 8 * i);
}

u16 sgx_own_entry(const struct sgx_own_blob *b, unsigned int i)
{
	return get_unaligned_le16(b->entry + 2 * i);
}

void sgx_own_patch_get(const struct sgx_own_blob *b, unsigned int i, u16 *index, u8 *kind,
		       u8 *slot, u32 *addend)
{
	const u8 *p = b->patch + 8 * i;

	*index = get_unaligned_le16(p);
	*kind = p[2];
	*slot = p[3];
	*addend = get_unaligned_le32(p + 4);
}

static int own_kind(u64 w)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sgx_own_forms); i++)
		if ((w & sgx_own_forms[i].mask) == sgx_own_forms[i].value)
			return sgx_own_forms[i].kind;
	return -1;
}

/* the predicate field: enc[43:41] of a limm, enc[58:56] of the other forms that have one */
static unsigned int own_pred(int kind, u64 w)
{
	return kind == SGX_OWN_MOVLIMM ? w >> 41 & 7 : w >> 56 & 7;
}

/*
 * What the verifier knows about each word, and the base analysis' state: kind and patch kind of
 * every word, the patch's addend, and for every word the state of the first OWN_TRACKED
 * temporaries on entry to it (the page offset a register holds, or OWN_TOP).
 */
struct own_scan {
	u8 kind[SGX_OWN_MAX_INSNS];
	u8 patch[SGX_OWN_MAX_INSNS];			/* 0, SGX_OWN_PATCH_RES or _CONT */
	u16 add[SGX_OWN_MAX_INSNS];
	u16 work[SGX_OWN_MAX_INSNS];
	bool reached[SGX_OWN_MAX_INSNS];
	bool queued[SGX_OWN_MAX_INSNS];
	u16 st[SGX_OWN_MAX_INSNS][OWN_TRACKED];
};

/* the state after word i: its destination registers become unknown, a patched limm a page offset */
static void own_effects(const struct own_scan *s, u64 w, unsigned int i, u16 *st)
{
	unsigned int d = own_dst(w), r;

	switch (s->kind[i]) {
	case SGX_OWN_MOVLIMM:
		if (d < OWN_TRACKED)
			st[d] = s->patch[i] == SGX_OWN_PATCH_RES ? s->add[i] : OWN_TOP;
		break;
	case SGX_OWN_MOVREG:
		if (!(w >> 51 & 1) && d < OWN_TRACKED)	/* bit 51: a bank that is not the temporaries */
			st[d] = OWN_TOP;
		break;
	case SGX_OWN_MOVFROMPC:
	case SGX_OWN_LDR:
	case SGX_OWN_LOAD_ABS:
		if (d < OWN_TRACKED)
			st[d] = OWN_TOP;
		break;
	case SGX_OWN_IMA32:				/* a 64-bit result: two registers */
		for (r = d; r <= d + 1; r++)
			if (r < OWN_TRACKED)
				st[r] = OWN_TOP;
		break;
	}
}

/* let state `st` flow into word j; queue j if its state changed */
static void own_flow(struct own_scan *s, unsigned int *nwork, unsigned int j, const u16 *st)
{
	unsigned int r;
	bool changed = false;

	if (!s->reached[j]) {
		s->reached[j] = true;
		memcpy(s->st[j], st, sizeof(s->st[j]));
		changed = true;
	} else {
		for (r = 0; r < OWN_TRACKED; r++) {
			if (s->st[j][r] != st[r] && s->st[j][r] != OWN_TOP) {
				s->st[j][r] = OWN_TOP;
				changed = true;
			}
		}
	}
	if (changed && !s->queued[j]) {
		s->queued[j] = true;
		s->work[(*nwork)++] = j;
	}
}

/*
 * A load or store base is a register that every path from an entry has set to the results page
 * (a patched limm) and not written since, and base plus offset stays in the page. The EDM's page
 * directory maps the closed code and data as well, so a base from anywhere else could write
 * into them.
 */
static int own_check_bases(const struct sgx_own_blob *b, struct own_scan *s, const char **why)
{
	unsigned int nwork = 0, i, e, base;
	u16 out[OWN_TRACKED];
	u64 w;

	for (e = 0; e < b->n_entry; e++) {
		memset(out, 0xff, sizeof(out));			/* OWN_TOP everywhere */
		own_flow(s, &nwork, sgx_own_entry(b, e), out);
	}
	while (nwork) {
		i = s->work[--nwork];
		s->queued[i] = false;
		w = sgx_own_insn(b, i);
		memcpy(out, s->st[i], sizeof(out));
		own_effects(s, w, i, out);
		if (s->kind[i] == SGX_OWN_LAPC || (s->kind[i] == SGX_OWN_NOP && (w >> 50 & 1)))
			continue;
		if (s->kind[i] == SGX_OWN_BR) {
			own_flow(s, &nwork, own_br_target(i, w), out);
			if (own_pred(SGX_OWN_BR, w))
				own_flow(s, &nwork, i + 1, out);
		} else {
			own_flow(s, &nwork, i + 1, out);
		}
	}
	for (i = 0; i < b->n_insn; i++) {
		if (!s->reached[i] || (s->kind[i] != SGX_OWN_LOAD_ABS && s->kind[i] != SGX_OWN_STORE_ABS))
			continue;
		w = sgx_own_insn(b, i);
		base = own_base(w);
		if (base >= OWN_TRACKED || s->st[i][base] == OWN_TOP ||
		    s->st[i][base] + 4 * (own_off(w) + 1) > SGX_OWN_RES_SIZE) {
			*why = SGX_OWN_WHY_BASE;
			return -EPERM;
		}
	}
	return 0;
}

/*
 * Default deny, in passes, the first that finds a reason ends the scan with it: every word a
 * canonical form (ldr and str also on the lists, a br inside the blob), the patches, the
 * continue triple, the last word, the entries, the bases. tools/fwtask's verify_why() has the same
 * passes in the same order and the same reasons.
 */
int sgx_own_verify(const struct sgx_own_blob *b, const char **why)
{
	struct own_scan *s;
	unsigned int i, t;
	int kind, ret = -EPERM;
	u64 w;

	s = kvzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	for (i = 0; i < b->n_insn; i++) {
		w = sgx_own_insn(b, i);
		kind = own_kind(w);
		if (kind < 0) {
			*why = SGX_OWN_WHY_UNKNOWN;
			goto out;
		}
		s->kind[i] = kind;
		switch (kind) {
		case SGX_OWN_DENY:
			*why = SGX_OWN_WHY_DENY;
			goto out;
		case SGX_OWN_LDR:
		case SGX_OWN_STR:
			if (sgx_own_clockgate(own_reg_index(w))) {
				*why = SGX_OWN_WHY_CLOCK;
				goto out;
			}
			if (!sgx_own_reg_ok(own_reg_index(w), kind == SGX_OWN_STR)) {
				*why = kind == SGX_OWN_STR ? SGX_OWN_WHY_STR : SGX_OWN_WHY_LDR;
				goto out;
			}
			break;
		case SGX_OWN_BR:
			t = own_br_target(i, w);
			if (t >= b->n_insn) {
				*why = SGX_OWN_WHY_BR_OUT;
				goto out;
			}
			break;
		}
	}
	for (i = 0; i < b->n_patch; i++) {
		u16 index;
		u8 pk, slot;
		u32 addend;

		sgx_own_patch_get(b, i, &index, &pk, &slot, &addend);
		if (index >= b->n_insn || s->kind[index] != SGX_OWN_MOVLIMM ||
		    own_pred(SGX_OWN_MOVLIMM, sgx_own_insn(b, index))) {
			*why = SGX_OWN_WHY_PATCH_LIMM;
			goto out;
		}
		if (pk == SGX_OWN_PATCH_CONT && slot > 3) {
			*why = SGX_OWN_WHY_PATCH_SLOT;
			goto out;
		}
		if (pk != SGX_OWN_PATCH_CONT && pk != SGX_OWN_PATCH_RES) {
			*why = SGX_OWN_WHY_PATCH_KIND;
			goto out;
		}
		if (s->patch[index]) {
			*why = SGX_OWN_WHY_PATCH_TWICE;
			goto out;
		}
		if ((pk == SGX_OWN_PATCH_CONT && addend) ||
		    (pk == SGX_OWN_PATCH_RES && addend >= SGX_OWN_RES_SIZE)) {
			*why = SGX_OWN_WHY_PATCH_ADD;
			goto out;
		}
		s->patch[index] = pk;
		s->add[index] = addend;
	}
	/* the continue triple, and nothing else touches pclink or branches into it */
	for (i = 0; i < b->n_insn; i++) {
		w = sgx_own_insn(b, i);
		switch (s->kind[i]) {
		case SGX_OWN_LAPC:
			if (i < 2 || s->kind[i - 1] != SGX_OWN_MOVPCLINK ||
			    s->kind[i - 2] != SGX_OWN_MOVLIMM ||
			    s->patch[i - 2] != SGX_OWN_PATCH_CONT ||
			    own_mov_pclink_src(sgx_own_insn(b, i - 1)) !=
			    own_dst(sgx_own_insn(b, i - 2))) {
				*why = SGX_OWN_WHY_LAPC;
				goto out;
			}
			break;
		case SGX_OWN_MOVPCLINK:
			if (i + 1 >= b->n_insn || s->kind[i + 1] != SGX_OWN_LAPC) {
				*why = SGX_OWN_WHY_PCLINK;
				goto out;
			}
			break;
		case SGX_OWN_BR:
			t = own_br_target(i, w);
			if (s->kind[t] == SGX_OWN_MOVPCLINK || s->kind[t] == SGX_OWN_LAPC) {
				*why = SGX_OWN_WHY_BR_TRIPLE;
				goto out;
			}
			break;
		}
	}
	/* the task ends: the last word cannot fall off the blob */
	i = b->n_insn - 1;
	w = sgx_own_insn(b, i);
	if (!(s->kind[i] == SGX_OWN_LAPC ||
	      (s->kind[i] == SGX_OWN_BR && !own_pred(SGX_OWN_BR, w)) ||
	      (s->kind[i] == SGX_OWN_NOP && (w >> 50 & 1)))) {
		*why = SGX_OWN_WHY_LAST;
		goto out;
	}
	for (i = 0; i < b->n_entry; i++) {
		t = sgx_own_entry(b, i);
		if (t >= b->n_insn || s->kind[t] == SGX_OWN_MOVPCLINK || s->kind[t] == SGX_OWN_LAPC) {
			*why = SGX_OWN_WHY_ENTRY;
			goto out;
		}
	}
	ret = own_check_bases(b, s, why);
out:
	kvfree(s);
	return ret;
}
