// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The debug group's own firmware tasks: the blob parser and the verifier (sgxfive_owntask.h).
 * Default deny: a word is accepted only if it is canonical, a form of the table in
 * sgx_own_regs.h with every bit that no field of the form names pinned, and an ldr or str of an
 * immediate index only on a register of its lists. The clock-gate registers are refused by
 * name, for ldr and str alike, before anything else.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "sgx_fw.h"
#include "sgx_own_pds.h"
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

#define SGX_OWN_PA_FIRST	2		/* the pa registers a mov may write */
#define SGX_OWN_PA_LAST		4
#define OWN_TRACKED	32		/* the registers a base may name: the stock 32 temporaries */
#define OWN_TOP		0xffff		/* a register of the base analysis that holds no page offset */
#define OWN_PDS_STATE	0xfff0		/* + slot: the register holds the overlay's pds<slot> value */

/* the registers the emitpds word reads, s0 to s2 */
static const u8 own_emit_regs[SGX_OWN_PDS_SLOTS] = { 2, 3, 4 };

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

bool sgx_own_has_emit(const struct sgx_own_blob *b)
{
	return sgx_own_emit_form(b) != SGX_OWN_FORM_COUNT;
}

unsigned int sgx_own_emit_form(const struct sgx_own_blob *b)
{
	unsigned int i, f;
	u64 w;

	/* the listed words (sgx_own_regs.h), each one exact emitpds-<name> form row of the verifier */
	for (i = 0; i < b->n_insn; i++) {
		w = sgx_own_insn(b, i);
		for (f = 0; f < SGX_OWN_FORM_COUNT; f++)
			if (w == sgx_own_emit_forms[f].word)
				return f;
	}
	return SGX_OWN_FORM_COUNT;
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
	u8 patch[SGX_OWN_MAX_INSNS];			/* 0, SGX_OWN_PATCH_RES, _CONT or _PDS */
	u16 add[SGX_OWN_MAX_INSNS];			/* a res addend, a pds slot */
	u16 work[SGX_OWN_MAX_INSNS];
	bool reached[SGX_OWN_MAX_INSNS];
	bool queued[SGX_OWN_MAX_INSNS];
	bool seen[SGX_OWN_MAX_INSNS];			/* the emitpds rule's reach */
	u16 st[SGX_OWN_MAX_INSNS][OWN_TRACKED];
};

/*
 * the state after word i: its destination registers become unknown, a patched limm a page offset;
 * a mov to a pa register writes no temporary
 */
static void own_effects(const struct own_scan *s, u64 w, unsigned int i, u16 *st)
{
	unsigned int d = own_dst(w), r;

	switch (s->kind[i]) {
	case SGX_OWN_MOVLIMM:
		if (d < OWN_TRACKED)
			st[d] = s->patch[i] == SGX_OWN_PATCH_RES ? s->add[i] :
				s->patch[i] == SGX_OWN_PATCH_PDS ? OWN_PDS_STATE + s->add[i] : OWN_TOP;
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
		if (s->reached[i] && s->kind[i] == SGX_OWN_EMITPDS) {
			unsigned int k;

			for (k = 0; k < SGX_OWN_PDS_SLOTS; k++) {
				if (s->st[i][own_emit_regs[k]] != OWN_PDS_STATE + k) {
					*why = SGX_OWN_WHY_EMIT_ARGS;
					return -EPERM;
				}
			}
			continue;
		}
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

/* the words some path from `start` reaches (marked in s->seen); s->work is free again by now */
static void own_reach(const struct sgx_own_blob *b, struct own_scan *s, unsigned int start)
{
	unsigned int nwork = 0, i, j, n;
	unsigned int next[2];
	u64 w;

	if (start >= b->n_insn || s->seen[start])
		return;
	s->seen[start] = true;
	s->work[nwork++] = start;
	while (nwork) {
		i = s->work[--nwork];
		w = sgx_own_insn(b, i);
		n = 0;
		if (s->kind[i] == SGX_OWN_LAPC || (s->kind[i] == SGX_OWN_NOP && (w >> 50 & 1)))
			continue;
		if (s->kind[i] == SGX_OWN_BR) {
			next[n++] = own_br_target(i, w);
			if (own_pred(SGX_OWN_BR, w))
				next[n++] = i + 1;
		} else {
			next[n++] = i + 1;
		}
		for (j = 0; j < n; j++) {
			if (next[j] < b->n_insn && !s->seen[next[j]]) {
				s->seen[next[j]] = true;
				s->work[nwork++] = next[j];
			}
		}
	}
}

/*
 * Default deny, in passes, the first that finds a reason ends the scan with it: every word a
 * canonical form (ldr and str also on the lists, a br inside the blob), the patches, the
 * continue triple, the last word, the entries, the bases and the emitpds's sources, and the emitpds
 * run once. tools/fwtask's verify_why() has the same passes in the same order and the same
 * reasons.
 */
int sgx_own_verify(const struct sgx_own_blob *b, const char **why)
{
	struct own_scan *s;
	unsigned int i, t, emit = SGX_OWN_MAX_INSNS;
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
		case SGX_OWN_MOVPA:
			/* pa0 and pa1 are what the closed task reads: only pa2 to pa4 */
			if (own_dst(w) < SGX_OWN_PA_FIRST || own_dst(w) > SGX_OWN_PA_LAST) {
				*why = SGX_OWN_WHY_PA;
				goto out;
			}
			break;
		case SGX_OWN_EMITPDS:
			if (emit < b->n_insn) {
				*why = SGX_OWN_WHY_EMIT_ONCE;
				goto out;
			}
			emit = i;
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
		if ((pk == SGX_OWN_PATCH_CONT && slot > SGX_OWN_EXIT_SLOT) ||
		    (pk == SGX_OWN_PATCH_PDS && slot >= SGX_OWN_PDS_SLOTS)) {
			*why = SGX_OWN_WHY_PATCH_SLOT;
			goto out;
		}
		if (pk != SGX_OWN_PATCH_CONT && pk != SGX_OWN_PATCH_RES && pk != SGX_OWN_PATCH_PDS) {
			*why = SGX_OWN_WHY_PATCH_KIND;
			goto out;
		}
		if (s->patch[index]) {
			*why = SGX_OWN_WHY_PATCH_TWICE;
			goto out;
		}
		if (((pk == SGX_OWN_PATCH_CONT || pk == SGX_OWN_PATCH_PDS) && addend) ||
		    (pk == SGX_OWN_PATCH_RES && addend >= SGX_OWN_RES_SIZE)) {
			*why = SGX_OWN_WHY_PATCH_ADD;
			goto out;
		}
		s->patch[index] = pk;
		s->add[index] = pk == SGX_OWN_PATCH_PDS ? slot : addend;
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
	if (emit < b->n_insn && b->n_entry <= SGX_OWN_EMIT_ENTRY) {
		*why = SGX_OWN_WHY_EMIT_ENTRY;
		goto out;
	}
	ret = own_check_bases(b, s, why);
	if (ret || emit >= b->n_insn)
		goto out;
	/* the emitpds once per task: nothing after it, nor the task its image starts, reaches it */
	own_reach(b, s, emit + 1);
	own_reach(b, s, sgx_own_entry(b, SGX_OWN_EMIT_ENTRY));
	if (s->seen[emit]) {
		*why = SGX_OWN_WHY_EMIT_ONCE;
		ret = -EPERM;
	}
out:
	kvfree(s);
	return ret;
}

/* ---- the overlay ---------------------------------------------------------------------------- */
/* sgx545_ukernel.fw as tools/fwcutter cuts it (format v3), the file E154 read the exit from */
static const u8 sgx_own_exit_fw_sha256[32] = {
	0x6a, 0x00, 0x26, 0x8e, 0x9d, 0x4c, 0xa5, 0x52,
	0xc0, 0x2e, 0x52, 0x65, 0x1a, 0x42, 0xa2, 0xb4,
	0xdb, 0x38, 0x87, 0xd7, 0xfe, 0x62, 0x6f, 0x20,
	0x98, 0x88, 0x36, 0x99, 0x85, 0x16, 0x2b, 0x0a,
};

u32 sgx_own_exit_index(const u8 *digest)
{
	return memcmp(digest, sgx_own_exit_fw_sha256, sizeof(sgx_own_exit_fw_sha256)) ? 0 :
	       SGX_OWN_EXIT_UK_INDEX;
}

int sgx_own_arm_parse(const char *line, struct sgx_own_arm *a)
{
	char buf[96], *tok, *p, *e;
	bool have_slots = false, have_entry = false;
	unsigned int v;
	int ret;

	if (strscpy(buf, line, sizeof(buf)) < 0)
		return -EINVAL;
	*a = (struct sgx_own_arm){ .trc = SGX_OWN_TRC_STOCK };
	p = strim(buf);
	while ((tok = strsep(&p, " \t")) != NULL) {
		if (!*tok)
			continue;
		if (!strncmp(tok, "slots=", 6)) {
			ret = kstrtouint(tok + 6, 0, &v);
			if (ret || !v || v >> SGX_OWN_SLOTS)
				return -EINVAL;
			a->slots = v;
			have_slots = true;
		} else if (!strncmp(tok, "entry=", 6)) {
			e = tok + 6;
			while (e && *e) {
				char *n = strsep(&e, ",");

				if (a->nentry >= SGX_OWN_SLOTS || kstrtouint(n, 0, &v) ||
				    v >= SGX_OWN_MAX_ENTRIES)
					return -EINVAL;
				a->entry[a->nentry++] = v;
			}
			have_entry = true;
		} else if (!strncmp(tok, "trc=", 4)) {
			ret = kstrtouint(tok + 4, 0, &v);
			if (ret || !v || v > 255)
				return -EINVAL;
			a->trc = v;
		} else if (!strncmp(tok, "trc_hazard=", 11)) {
			ret = kstrtouint(tok + 11, 0, &v);
			if (ret || v > 1)
				return -EINVAL;
			a->trc_hazard = v;
		} else if (!strncmp(tok, "pds=", 4)) {
			for (v = 0; v < SGX_OWN_IMG_COUNT; v++)
				if (!strcmp(tok + 4, sgx_own_img_name(v)))
					break;
			if (v == SGX_OWN_IMG_COUNT)
				return -EINVAL;
			a->img = v;
		} else if (!strncmp(tok, "pdsat=", 6)) {
			for (v = 0; v < SGX_OWN_AT_COUNT; v++)
				if (!strcmp(tok + 6, sgx_own_at_name(v)))
					break;
			if (v == SGX_OWN_AT_COUNT)
				return -EINVAL;
			a->at = v;
		} else if (!strncmp(tok, "pds2=", 5)) {
			for (v = 0; v < SGX_OWN_PDS2_COUNT; v++)
				if (!strcmp(tok + 5, sgx_own_pds2_name(v)))
					break;
			if (v == SGX_OWN_PDS2_COUNT)
				return -EINVAL;
			a->pds2 = v;
		} else if (!strncmp(tok, "form=", 5)) {
			for (v = 0; v < SGX_OWN_FORM_COUNT; v++)
				if (!strcmp(tok + 5, sgx_own_form_name(v)))
					break;
			if (v == SGX_OWN_FORM_COUNT)
				return -EINVAL;
			a->form = v;
		} else {
			return -EINVAL;
		}
	}
	if (!have_slots || !have_entry || a->nentry != hweight32(a->slots))
		return -EINVAL;
	if (a->trc > SGX_OWN_TRC_FREE_MAX && !a->trc_hazard)
		return -EPERM;		/* 32 and up only with trc_hazard=1 */
	return 0;
}

bool sgx_own_res_write_ok(loff_t pos, size_t len)
{
	/* written as pos <= SIZE && len <= SIZE - pos: pos + len could overflow a loff_t */
	return pos >= 4 && !(pos & 3) && len && !(len & 3) && pos <= SGX_OWN_RES_SIZE &&
	       len <= (size_t)(SGX_OWN_RES_SIZE - pos);
}

/* as an armed slot's word: TRC, the code page's index plus the entry, CBASE 15 (KERNEL_CODE) */
u32 sgx_own_pds_u0(unsigned int entry)
{
	return (u32)SGX_OWN_PDS_TRC << 24 | (SGX_OWN_INDEX + entry) << 4 | 0xf;
}

static const struct {
	const char *name;
	const u32 *words;
	unsigned int n;
	u32 ctl;
	bool fillv;			/* the arm fills V0 and V2 besides U0 (sgx_own_pds.h _FILLV) */
} sgx_own_imgs[SGX_OWN_IMG_COUNT] = {
	[SGX_OWN_IMG_V1] = { "v1", sgx_own_pds_image, SGX_OWN_PDS_WORDS, SGX_OWN_PDS_CTL, true },
	[SGX_OWN_IMG_CRUMB] = { "crumb", sgx_own_pds_crumb, SGX_OWN_PDS_CRUMB_WORDS,
				SGX_OWN_PDS_CRUMB_CTL, SGX_OWN_PDS_CRUMB_FILLV },
	[SGX_OWN_IMG_HALT] = { "halt", sgx_own_pds_halt, SGX_OWN_PDS_HALT_WORDS,
			       SGX_OWN_PDS_HALT_CTL, SGX_OWN_PDS_HALT_FILLV },
	[SGX_OWN_IMG_CRUMB3] = { "crumb3", sgx_own_pds_crumb3, SGX_OWN_PDS_CRUMB3_WORDS,
				 SGX_OWN_PDS_CRUMB3_CTL, SGX_OWN_PDS_CRUMB3_FILLV },
	[SGX_OWN_IMG_LAND] = { "land", sgx_own_pds_land, SGX_OWN_PDS_LAND_WORDS,
			       SGX_OWN_PDS_LAND_CTL, SGX_OWN_PDS_LAND_FILLV },
	[SGX_OWN_IMG_MARK] = { "mark", sgx_own_pds_mark, SGX_OWN_PDS_MARK_WORDS,
			       SGX_OWN_PDS_MARK_CTL, SGX_OWN_PDS_MARK_FILLV },
	[SGX_OWN_IMG_LANDV] = { "landv", sgx_own_pds_landv, SGX_OWN_PDS_LANDV_WORDS,
				SGX_OWN_PDS_LANDV_CTL, SGX_OWN_PDS_LANDV_FILLV },
	[SGX_OWN_IMG_CRUMBZ] = { "crumbz", sgx_own_pds_crumbz, SGX_OWN_PDS_CRUMBZ_WORDS,
				 SGX_OWN_PDS_CRUMBZ_CTL, SGX_OWN_PDS_CRUMBZ_FILLV },
	[SGX_OWN_IMG_LANDQ] = { "landq", sgx_own_pds_landq, SGX_OWN_PDS_LANDQ_WORDS,
				SGX_OWN_PDS_LANDQ_CTL, SGX_OWN_PDS_LANDQ_FILLV },
	[SGX_OWN_IMG_LANDR] = { "landr", sgx_own_pds_landr, SGX_OWN_PDS_LANDR_WORDS,
				SGX_OWN_PDS_LANDR_CTL, SGX_OWN_PDS_LANDR_FILLV },
	[SGX_OWN_IMG_LANDP] = { "landp", sgx_own_pds_landp, SGX_OWN_PDS_LANDP_WORDS,
				SGX_OWN_PDS_LANDP_CTL, SGX_OWN_PDS_LANDP_FILLV },
};

static const char *const sgx_own_ats[SGX_OWN_AT_COUNT] = {
	[SGX_OWN_AT_OWN] = "own",
	[SGX_OWN_AT_FW14] = "fw14",
};

const char *sgx_own_img_name(unsigned int img)
{
	return img < SGX_OWN_IMG_COUNT ? sgx_own_imgs[img].name : "?";
}

static const struct {
	const char *name;
	u32 value;
} sgx_own_pds2s[SGX_OWN_PDS2_COUNT] = {
	[SGX_OWN_PDS2_ZERO] = { "0", 0 },
	[SGX_OWN_PDS2_REQ] = { "req", SGX_OWN_PDS2_REQ_VALUE },
};

const char *sgx_own_form_name(unsigned int form)
{
	return form < SGX_OWN_FORM_COUNT ? sgx_own_emit_forms[form].name : "?";
}

const char *sgx_own_pds2_name(unsigned int v)
{
	return v < SGX_OWN_PDS2_COUNT ? sgx_own_pds2s[v].name : "?";
}

u32 sgx_own_pds2_value(unsigned int v)
{
	return v < SGX_OWN_PDS2_COUNT ? sgx_own_pds2s[v].value : 0;
}

const char *sgx_own_at_name(unsigned int at)
{
	return at < SGX_OWN_AT_COUNT ? sgx_own_ats[at] : "?";
}

u32 sgx_own_pds_ctl(unsigned int img)
{
	return img < SGX_OWN_IMG_COUNT ? sgx_own_imgs[img].ctl : 0;
}

unsigned int sgx_own_pds_words(unsigned int img)
{
	return img < SGX_OWN_IMG_COUNT ? sgx_own_imgs[img].n : 0;
}

/* the image's words, U0 (and V0 and V2 where the image takes them) filled in, at dst; nothing
 * else is touched */
static void sgx_own_pds_put(u8 *dst, unsigned int img, unsigned int entry, u32 v0, u32 v2)
{
	unsigned int i;
	u32 w;

	if (img >= SGX_OWN_IMG_COUNT)
		return;
	for (i = 0; i < sgx_own_imgs[img].n; i++) {
		w = sgx_own_imgs[img].words[i];
		if (i == SGX_OWN_PDS_U0)
			w = sgx_own_pds_u0(entry);
		else if (i == SGX_OWN_PDS_V0 && sgx_own_imgs[img].fillv)
			w = v0;
		else if (i == SGX_OWN_PDS_V2 && sgx_own_imgs[img].fillv)
			w = v2;
		put_unaligned_le32(w, dst + 4 * i);
	}
}

void sgx_own_pds_build(u8 *page, unsigned int img, unsigned int entry, u32 v0, u32 v2)
{
	memset(page, 0, SGX_OWN_PDS_SIZE);
	sgx_own_pds_put(page, img, entry, v0, v2);
}

int sgx_own_fw14_off(const struct sgx_fw *fw, unsigned int buf, unsigned int img, u32 room,
		     u32 *off)
{
	const struct sgx_fw_buffer *fb = sgx_fw_buffer(fw, buf);
	u64 end, o;
	u32 i;

	if (!fb)
		return -ENOENT;
	if (img >= SGX_OWN_IMG_COUNT)
		return -EINVAL;
	end = fb->size;
	/* a PATCHES word: 8 bytes for a USSE limm (encoding 1), else 4 (format section 4) */
	for (i = 0; i < fw->patches.count; i++) {
		const u8 *r = fw->patches.rec + 16 * i;
		u64 e = (u64)get_unaligned_le32(r + 4) + (r[1] == 1 ? 8 : 4);

		if (r[0] == buf && e > end)
			end = e;
	}
	o = (end + SGX_OWN_FW14_ALIGN - 1) & ~(u64)(SGX_OWN_FW14_ALIGN - 1);
	if (o + 4ull * sgx_own_imgs[img].n > room)
		return -ERANGE;
	*off = (u32)o;
	return 0;
}

void sgx_own_fw14_clear(u8 *page, u32 size, u32 room)
{
	if (page && size < room)
		memset(page + size, 0, room - size);
}

int sgx_own_apply(const struct sgx_own_blob *b, const struct sgx_own_arm *a,
		  const struct sgx_fw *fw, const struct sgx_fw_layout *l, unsigned int pds_buf,
		  u8 *pds_image, u32 pds_size, u8 *code, u32 res_addr,
		  struct sgx_own_pds *pds)
{
	const struct sgx_fw_buffer *fb14;
	u32 emit_addr = 0, off14 = 0;
	bool emit;
	struct sgx_fw_patched p;
	u32 tgt[SGX_OWN_SLOTS];
	u32 index, addend, i, k;
	const char *why = "";
	unsigned int n, j;
	int ret;

	/* the verifier is the last step before the copy: a blob stored without it cannot get in */
	ret = sgx_own_verify(b, &why);
	if (ret)
		return ret;
	/* the emitpds names the kernel's image: its page and an address the emit can name */
	emit = sgx_own_has_emit(b);
	if (emit && (!pds || !pds->image))
		return -ENOENT;
	if (emit && (!pds->addr || (pds->addr & 15)))
		return -EINVAL;
	if (a->img >= SGX_OWN_IMG_COUNT || a->at >= SGX_OWN_AT_COUNT ||
	    a->pds2 >= SGX_OWN_PDS2_COUNT || a->form >= SGX_OWN_FORM_COUNT)
		return -EINVAL;
	/* pds2= and form= name the emit: none for a blob without it, form= the word it holds */
	if (!emit && (a->pds2 != SGX_OWN_PDS2_ZERO || a->form != SGX_OWN_FORM_OWN))
		return -EINVAL;
	if (emit && sgx_own_emit_form(b) != a->form)
		return -EINVAL;
	/* pdsat=fw14: a blob that emits, #14's page, and room for the image in its slack */
	if (a->at == SGX_OWN_AT_FW14) {
		if (!emit)
			return -EINVAL;
		if (!pds->fw14 || !pds->fw14_addr)
			return -ENOENT;
		ret = sgx_own_fw14_off(fw, SGXFIVE_OWN_FW14_BUF, a->img, pds->fw14_room, &off14);
		if (ret)
			return ret;
		emit_addr = pds->fw14_addr + off14;
		if (emit_addr & 15)
			return -EINVAL;
	} else if (emit) {
		emit_addr = pds->addr;
	}
	n = sgx_fw_code_targets(fw, pds_buf, tgt, ARRAY_SIZE(tgt));
	if (n > ARRAY_SIZE(tgt))
		return -ENOENT;
	if (!a->slots || a->slots >> n || a->nentry != hweight32(a->slots))
		return -ENOENT;
	if (a->trc > SGX_OWN_TRC_FREE_MAX && !a->trc_hazard)
		return -EPERM;
	for (j = 0; j < a->nentry; j++)
		if (a->entry[j] >= b->n_entry)
			return -EINVAL;
	/* the shared exit is known for one firmware file only */
	for (i = 0; i < b->n_patch; i++) {
		u16 pi;
		u8 pk, slot;
		u32 add;

		sgx_own_patch_get(b, i, &pi, &pk, &slot, &add);
		if (pk == SGX_OWN_PATCH_CONT && slot == SGX_OWN_EXIT_SLOT && !a->exit_index)
			return -ENOENT;
	}
	/* check every armed slot before anything is written */
	for (k = 0; k < n; k++) {
		if (!(a->slots & 1u << k))
			continue;
		/* sgx_fw_patch_word() takes the first record of a target: more would leave a start
		 * dword of the closed task behind */
		if (sgx_fw_code_records(fw, pds_buf, tgt[k]) != 1)
			return -EEXIST;
		ret = sgx_fw_patch_word(fw, l, pds_buf, tgt[k], SGX_OWN_INDEX, 0, &p);
		if (ret)
			return ret;
		if (p.offset + 4 > pds_size)
			return -ERANGE;
	}
	memset(code, 0, SGX_OWN_CODE_SIZE);
	for (i = 0; i < b->n_insn; i++)
		put_unaligned_le64(sgx_own_insn(b, i), code + 8 * i);
	for (i = 0; i < b->n_patch; i++) {
		u16 pi;
		u8 pk, slot;
		u32 add, val;

		sgx_own_patch_get(b, i, &pi, &pk, &slot, &add);	/* verified: cont has no addend */
		if (pk == SGX_OWN_PATCH_CONT && slot == SGX_OWN_EXIT_SLOT) {
			/* the exit's ukernel index plus the ukernel's offset from the code base */
			ret = sgx_fw_patch_word(fw, l, pds_buf, tgt[0], SGX_OWN_INDEX, 0, &p);
			if (ret)
				return ret;
			val = a->exit_index + p.old_index - tgt[0];
		} else if (pk == SGX_OWN_PATCH_PDS) {
			/* verified: slot 0 to 2, no addend; pds is set (a blob with the emitpds) */
			val = slot == 0 ? sgx_own_pds_ctl(a->img) : slot == 1 ? emit_addr >> 4 :
			      sgx_own_pds2_value(a->pds2);
		} else if (pk == SGX_OWN_PATCH_CONT) {
			if (slot >= n)
				return -ENOENT;
			ret = sgx_fw_patch_word(fw, l, pds_buf, tgt[slot], SGX_OWN_INDEX, 0, &p);
			if (ret)
				return ret;
			val = p.old_index;
		} else {
			val = res_addr + add;
		}
		put_unaligned_le64(sgx_fw_limm_set(sgx_own_insn(b, pi), val), code + 8 * pi);
	}
	j = 0;
	for (k = 0; k < n; k++) {
		if (!(a->slots & 1u << k))
			continue;
		ret = sgx_fw_patch_word(fw, l, pds_buf, tgt[k], SGX_OWN_INDEX, 0, &p);
		if (ret)
			return ret;
		index = SGX_OWN_INDEX + sgx_own_entry(b, a->entry[j++]);
		/* the old word's addend (its index field, bits 22:4, taken out) with our TRC, or the
		 * word's own when none was asked for */
		addend = (p.old_val & ~(0x7ffffu << 4) & 0x00ffffffu) |
			 (u32)(a->trc ? a->trc : p.old_val >> 24) << 24;
		ret = sgx_fw_patch_word(fw, l, pds_buf, tgt[k], index, addend, &p);
		if (ret)
			return ret;
		put_unaligned_le32(p.new_val, pds_image + p.offset);
	}
	/* the image for the blob's entry 1 where the arm puts it; no program anywhere else */
	if (pds && pds->image) {
		if (emit && a->at == SGX_OWN_AT_OWN)
			sgx_own_pds_build(pds->image, a->img, sgx_own_entry(b, SGX_OWN_EMIT_ENTRY),
					  pds->v0, pds->v2);
		else
			memset(pds->image, 0, SGX_OWN_PDS_SIZE);
	}
	if (pds && pds->fw14) {
		fb14 = sgx_fw_buffer(fw, SGXFIVE_OWN_FW14_BUF);
		if (fb14)
			sgx_own_fw14_clear(pds->fw14, fb14->size, pds->fw14_room);
		if (emit && a->at == SGX_OWN_AT_FW14)
			sgx_own_pds_put(pds->fw14 + off14, a->img,
					sgx_own_entry(b, SGX_OWN_EMIT_ENTRY), pds->v0, pds->v2);
	}
	if (pds)
		pds->at = emit_addr;
	return 0;
}
