// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Kernel pokes (sgxfive_kpoke.h): parse, arm and apply changes to the words
 * of the firmware command the kernel builds for the next job, and the
 * allow-list of the registers debugfs kreg may write.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <drm/drm_print.h>

#include "sgx_mmu.h"
#include "sgx_render.h"
#include "sgx_seq.h"
#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_kpoke.h"
#include "sgxfive_priv.h"

/* A render image holds the TA command, then the render details. */
static_assert(SGXFIVE_RENDER_IMG_RD >= SGX_RENDER_CMD_STRIDE);
static_assert(SGXFIVE_RENDER_IMG_RD + SGX_RENDER_RD_SIZE <= SGX_XFER_CMD_SIZE);

static const char *const sgxfive_kpoke_names[SGXFIVE_KPOKE_SPACES] = {
	[SGXFIVE_KPOKE_TA] = "ta", [SGXFIVE_KPOKE_RD] = "rd", [SGXFIVE_KPOKE_TQ] = "tq",
	[SGXFIVE_KPOKE_CTX] = "ctx", [SGXFIVE_KPOKE_RTDS] = "rtds", [SGXFIVE_KPOKE_PB] = "pb",
	[SGXFIVE_KPOKE_KW] = "kw", [SGXFIVE_KPOKE_TH] = "th",
	[SGXFIVE_KPOKE_BG] = "bg", [SGXFIVE_KPOKE_PAR] = "par",
};

/*
 * A space of a context's structures (a render context's, poked as its render
 * is kicked, or a transfer context's, as its job is submitted).
 */
static bool sgxfive_kpoke_at_run(u32 space)
{
	return space >= SGXFIVE_KPOKE_CTX && space < SGXFIVE_KPOKE_SPACES;
}

u32 sgxfive_kpoke_words(u32 space)
{
	switch (space) {
	case SGXFIVE_KPOKE_TA:
		return SGX_RENDER_CMD_STRIDE / 4;
	case SGXFIVE_KPOKE_RD:
		return SGX_RENDER_RD_SIZE / 4;
	case SGXFIVE_KPOKE_TQ:
		return SGX_XFER_CMD_SIZE / 4;
	case SGXFIVE_KPOKE_CTX:
		return SGX_RENDER_CTX_SIZE / 4;
	case SGXFIVE_KPOKE_RTDS:
		return SGX_RENDER_RTDS_SIZE / 4;
	case SGXFIVE_KPOKE_PB:
		return SGX_RENDER_PBDESC_SIZE / 4;
	case SGXFIVE_KPOKE_KW:
		return SGXFIVE_KPOKE_KW_WORDS;
	case SGXFIVE_KPOKE_TH:
		return SGX_XFER_CTX_SIZE / 4;
	case SGXFIVE_KPOKE_BG:
		return SGX_XFER_BGOBJ_DWORDS;
	case SGXFIVE_KPOKE_PAR:
		return SGXFIVE_KPOKE_PAR_PAGES * SZ_4K / 4;
	}
	return 0;
}

int sgxfive_kpoke_parse(const char *s, struct sgxfive_kpoke *p)
{
	struct sgxfive_kpoke k = { .space = SGXFIVE_KPOKE_SPACES };
	char buf[SGXFIVE_KPOKE_LINE];
	char *colon, *eq;
	unsigned int i;

	if (strscpy(buf, s, sizeof(buf)) < 0)
		return -EINVAL;
	colon = strchr(buf, ':');
	eq = strchr(buf, '=');
	if (!colon || !eq || eq < colon)
		return -EINVAL;
	*colon = '\0';
	for (i = 0; i < SGXFIVE_KPOKE_SPACES; i++)
		if (!strcmp(buf, sgxfive_kpoke_names[i]))
			k.space = i;
	if (k.space == SGXFIVE_KPOKE_SPACES)
		return -EINVAL;
	if (eq > colon + 1 && eq[-1] == '^') {
		k.flip = true;
		eq[-1] = '\0';
	}
	*eq = '\0';
	if (kstrtou32(colon + 1, 0, &k.index) || kstrtou32(eq + 1, 0, &k.value))
		return -EINVAL;
	if (k.index >= sgxfive_kpoke_words(k.space))
		return -ERANGE;
	*p = k;
	return 0;
}

void sgxfive_kpoke_format(const struct sgxfive_kpoke *p, char *buf, size_t size)
{
	snprintf(buf, size, "%s:%u%s=0x%08x",
		 p->space < SGXFIVE_KPOKE_SPACES ? sgxfive_kpoke_names[p->space] : "?",
		 p->index, p->flip ? "^" : "", p->value);
}

int sgxfive_kpoke_arm(struct sgxfive_kpokes *k, const struct sgxfive_kpoke *p,
		      unsigned int n)
{
	unsigned int i;

	if (n > SGXFIVE_KPOKE_MAX - k->n)
		return -ENOSPC;
	for (i = 0; i < n; i++)
		if (p[i].index >= sgxfive_kpoke_words(p[i].space))
			return -ERANGE;
	memcpy(&k->p[k->n], p, n * sizeof(*p));
	k->n += n;
	return 0;
}

unsigned int sgxfive_kpoke_take(struct sgxfive_kpokes *k, u8 *img, bool render,
				struct sgxfive_kpoke *done)
{
	unsigned int i, kept = 0, n = 0;

	for (i = 0; i < k->n; i++) {
		const struct sgxfive_kpoke p = k->p[i];
		u8 *w;
		u32 v;

		if (sgxfive_kpoke_at_run(p.space) || render == (p.space == SGXFIVE_KPOKE_TQ)) {
			k->p[kept++] = p;		/* another kind's */
			continue;
		}
		w = img + (p.space == SGXFIVE_KPOKE_RD ? SGXFIVE_RENDER_IMG_RD : 0) +
		    4 * p.index;
		v = get_unaligned_le32(w);
		put_unaligned_le32(p.flip ? v ^ p.value : p.value, w);
		done[n++] = p;
	}
	k->n = kept;
	return n;
}

unsigned int sgxfive_kpoke_take_run(struct sgxfive_kpokes *k,
				    u32 *const words[SGXFIVE_KPOKE_SPACES],
				    struct sgxfive_kpoke *done)
{
	unsigned int i, kept = 0, n = 0;

	for (i = 0; i < k->n; i++) {
		const struct sgxfive_kpoke p = k->p[i];
		u32 *w;

		if (!sgxfive_kpoke_at_run(p.space) || !words[p.space]) {
			k->p[kept++] = p;
			continue;
		}
		w = words[p.space] + p.index;
		*w = p.flip ? *w ^ p.value : p.value;
		done[n++] = p;
	}
	k->n = kept;
	return n;
}

/*
 * One write's lines, each a poke or "clear" (which drops what was armed
 * before the write's pokes are added). A bad line fails the whole write and
 * arms nothing. Arming needs the firmware running; clearing does not.
 */
int sgxfive_kpoke_write(struct sgxfive *sgx, char *buf)
{
	struct sgxfive_kpoke p[SGXFIVE_KPOKE_MAX];
	unsigned int n = 0;
	bool clear = false;
	char *line;
	int ret;

	while ((line = strsep(&buf, "\n"))) {
		line = strim(line);
		if (!*line)
			continue;
		if (!strcmp(line, "clear")) {
			clear = true;
			continue;
		}
		if (n == SGXFIVE_KPOKE_MAX)
			return -ENOSPC;
		ret = sgxfive_kpoke_parse(line, &p[n]);
		if (ret)
			return ret;
		n++;
	}
	if (n && !READ_ONCE(sgx->live))
		return -ENODEV;
	spin_lock(&sgx->kpoke_lock);
	if (clear)
		sgx->kpokes.n = 0;
	ret = sgxfive_kpoke_arm(&sgx->kpokes, p, n);
	spin_unlock(&sgx->kpoke_lock);
	return ret;
}

static void sgxfive_kpoke_log(struct sgxfive *sgx, const struct sgxfive_kpoke *done,
			      unsigned int n)
{
	char line[SGXFIVE_KPOKE_LINE];
	unsigned int i;

	for (i = 0; i < n; i++) {
		sgxfive_kpoke_format(&done[i], line, sizeof(line));
		drm_info(sgx->dev, "sgxfive: kpoke %s applied\n", line);
	}
}

void sgxfive_kpoke_job(struct sgxfive *sgx, u8 *img, bool render)
{
	struct sgxfive_kpoke done[SGXFIVE_KPOKE_MAX];
	unsigned int n = 0;

	spin_lock(&sgx->kpoke_lock);
	if (sgx->kpokes.n)
		n = sgxfive_kpoke_take(&sgx->kpokes, img, render, done);
	spin_unlock(&sgx->kpoke_lock);
	sgxfive_kpoke_log(sgx, done, n);
}

void sgxfive_kpoke_run(struct sgxfive *sgx, u32 *const words[SGXFIVE_KPOKE_SPACES])
{
	struct sgxfive_kpoke done[SGXFIVE_KPOKE_MAX];
	unsigned int n = 0;

	spin_lock(&sgx->kpoke_lock);
	if (sgx->kpokes.n)
		n = sgxfive_kpoke_take_run(&sgx->kpokes, words, done);
	spin_unlock(&sgx->kpoke_lock);
	sgxfive_kpoke_log(sgx, done, n);
}

bool sgxfive_kpoke_armed(struct sgxfive *sgx, u32 space)
{
	bool armed = false;
	unsigned int i;

	spin_lock(&sgx->kpoke_lock);
	for (i = 0; i < sgx->kpokes.n; i++)
		armed |= sgx->kpokes.p[i].space == space;
	spin_unlock(&sgx->kpoke_lock);
	return armed;
}

void sgxfive_kpoke_xfer(struct sgxfive *sgx, u32 *hw)
{
	u32 *words[SGXFIVE_KPOKE_SPACES] = { [SGXFIVE_KPOKE_TH] = hw };

	sgxfive_kpoke_run(sgx, words);
	wmb();	/* the context's memory, before the job reaches the firmware */
}

/*
 * The registers kreg may write, sorted. 0x0810-0x0abc the EDM firmware never
 * reaches (none is in sgxfive_regdump_regs, which lists every register it
 * reads or writes): writing them cannot race with the firmware's own use.
 * 0x0418-0x0800 it writes only from a 3D render's details, never in the
 * transfer kick: written before a transfer job, they hold through it, and the
 * next render overwrites them.
 */
const u32 sgxfive_kreg_allowed[SGXFIVE_KREG_COUNT] = {
	0x0418, 0x04dc, 0x04e8, 0x0520, 0x0800,
	0x0810, 0x0814, 0x0818, 0x0a78, 0x0abc,
};

/*
 * The HWPerf control register, then what the firmware's HWPerf routine copies
 * to its buffer: 0x0044-0x0064 by an indexed loop, 0x0e84-0x0e90 and
 * 0x0eb4-0x0ec0 one by one. Reading them is plain MMIO; 0x0e80, which the firmware pulses
 * after it sets the control, is left out.
 */
const u32 sgxfive_hwperf_regs[SGXFIVE_HWPERF_NREGS] = {
	0x0040, 0x0044, 0x0048, 0x004c, 0x0050, 0x0054, 0x0058, 0x005c, 0x0060,
	0x0064, 0x0e84, 0x0e88, 0x0e8c, 0x0e90, 0x0eb4, 0x0eb8, 0x0ebc, 0x0ec0,
};

int sgxfive_hwperf_parse(const char *s, u32 *group, u32 *word)
{
	char buf[SGXFIVE_KPOKE_LINE];
	char *a, *b;

	if (strscpy(buf, s, sizeof(buf)) < 0)
		return -EINVAL;
	a = skip_spaces(buf);
	b = strpbrk(a, " \t");
	if (!b)
		return -EINVAL;
	*b = '\0';
	b = skip_spaces(b + 1);
	if (!*b || strpbrk(b, " \t"))
		return -EINVAL;
	if (kstrtou32(a, 0, group) || kstrtou32(b, 0, word))
		return -EINVAL;
	return *group > 15 ? -ERANGE : 0;
}

bool sgxfive_kreg_ok(u32 off)
{
	unsigned int i;

	for (i = 0; i < SGXFIVE_KREG_COUNT; i++)
		if (sgxfive_kreg_allowed[i] == off)
			return true;
	return false;
}

/* OFFSET=VALUE or OFFSET^=VALUE, the offset not yet checked against a list. */
static int sgxfive_kreg_split(const char *s, u32 *off, bool *flip, u32 *value)
{
	char buf[SGXFIVE_KPOKE_LINE];
	char *eq;
	bool x = false;

	if (strscpy(buf, s, sizeof(buf)) < 0)
		return -EINVAL;
	eq = strchr(buf, '=');
	if (!eq)
		return -EINVAL;
	if (eq > buf && eq[-1] == '^') {
		x = true;
		eq[-1] = '\0';
	}
	*eq = '\0';
	if (kstrtou32(buf, 0, off) || kstrtou32(eq + 1, 0, value))
		return -EINVAL;
	*flip = x;
	return 0;
}

int sgxfive_kreg_parse(const char *s, u32 *off, bool *flip, u32 *value)
{
	int ret = sgxfive_kreg_split(s, off, flip, value);

	return ret ? ret : sgxfive_kreg_ok(*off) ? 0 : -EPERM;
}

/*
 * CR_CLKGATECTL2, CR_EVENT_HOST_ENABLE2, CR_EVENT_HOST_CLEAR2,
 * CR_EVENT_HOST_ENABLE, CR_EVENT_HOST_CLEAR, the register at 0x0740 that
 * the TA kick reads (never writes) for its DPM store and load tasks, and
 * CR_BIF_CTRL and CR_BIF_INT_STAT.
 */
const u32 sgxfive_kregnow_allowed[SGXFIVE_KREGNOW_COUNT] = {
	SGX_CLKGATECTL2, SGX_EVENT_HOST_ENABLE2, SGX_EVENT_HOST_CLEAR2, SGX_EVENT_HOST_ENABLE,
	SGX_EVENT_HOST_CLEAR, SGX_DPM_TASK_FIELDS, SGX_BIF_CTRL, SGX_BIF_INT_STAT,
};

/* The BIF rows: the firmware reaches CR_BIF_CTRL itself */
bool sgxfive_kregnow_bif(u32 off)
{
	return off == SGX_BIF_CTRL || off == SGX_BIF_INT_STAT;
}

bool sgxfive_kregnow_bif_allowed(bool idle, bool held)
{
	return idle || held;
}

/* The clear registers: write-only (the register dump never reads them) */
bool sgxfive_kregnow_wo(u32 off)
{
	return off == SGX_EVENT_HOST_CLEAR2 || off == SGX_EVENT_HOST_CLEAR;
}

bool sgxfive_kregnow_ok(u32 off)
{
	unsigned int i;

	for (i = 0; i < SGXFIVE_KREGNOW_COUNT; i++)
		if (sgxfive_kregnow_allowed[i] == off)
			return true;
	return false;
}

int sgxfive_kregnow_parse(const char *s, u32 *off, bool *flip, u32 *value)
{
	int ret = sgxfive_kreg_split(s, off, flip, value);

	if (ret)
		return ret;
	if (!sgxfive_kregnow_ok(*off))
		return -EPERM;
	return *flip && sgxfive_kregnow_wo(*off) ? -EINVAL : 0;
}

int sgxfive_kicker_parse(const char *s, u32 *value)
{
	u32 v;

	if (s[0] != '=' || kstrtou32(s + 1, 0, &v) || v > 255)
		return -EINVAL;
	*value = v;
	return 0;
}

void sgxfive_hostirq_init(struct sgxfive_hostirq *h)
{
	spin_lock_init(&h->lock);
	sgxfive_hostirq_reset(h);
}

void sgxfive_hostirq_arm(struct sgxfive_hostirq *h, u32 enable, u32 enable2)
{
	h->saved[0] = enable;
	h->saved[1] = enable2;
	h->armed = true;
}

bool sgxfive_hostirq_note(struct sgxfive_hostirq *h, u32 stat_1, u32 stat_2)
{
	atomic_inc(&h->irqs);
	if (!h->armed || (stat_1 & SGX_EVENT_SW))
		return false;
	atomic_inc(&h->extra);
	h->last[0] = stat_1;
	h->last[1] = stat_2;
	h->armed = false;
	return true;
}

void sgxfive_hostirq_reset(struct sgxfive_hostirq *h)
{
	unsigned long flags;

	spin_lock_irqsave(&h->lock, flags);
	h->armed = false;
	spin_unlock_irqrestore(&h->lock, flags);
	atomic_set(&h->irqs, 0);
	atomic_set(&h->extra, 0);
}

bool sgxfive_oneshot_take(bool *armed)
{
	return xchg(armed, false);
}

u32 sgxfive_ctx_page_flags(bool rw)
{
	return rw ? 0 : SGX_MMU_RO;
}

int sgxfive_tdfill_parse(const char *s, struct sgxfive_tdfill *f)
{
	struct sgxfive_tdfill t = { };
	char buf[SGXFIVE_KPOKE_LINE];
	char *page, *value, *ramp;

	if (strscpy(buf, s, sizeof(buf)) < 0)
		return -EINVAL;
	page = skip_spaces(buf);
	value = strpbrk(page, " \t");
	if (!value)
		return -EINVAL;
	*value++ = '\0';
	value = skip_spaces(value);
	ramp = strpbrk(value, " \t");
	if (ramp) {
		*ramp++ = '\0';
		ramp = skip_spaces(ramp);
		if (strcmp(ramp, "ramp"))
			return -EINVAL;
		t.ramp = true;
	}
	if (kstrtou32(page, 0, &t.page) || kstrtou32(value, 0, &t.value))
		return -EINVAL;
	if (t.page >= SGX_RENDER_TD_MAX_SIZE >> PAGE_SHIFT)
		return -ERANGE;
	*f = t;
	return 0;
}

void sgxfive_tdfill_apply(const struct sgxfive_tdfill *f, u32 *page)
{
	unsigned int i;

	for (i = 0; i < PAGE_SIZE / 4; i++)
		page[i] = f->ramp ? f->value + i : f->value;
}

int sgxfive_tdfill_write(struct sgxfive *sgx, char *buf)
{
	struct sgxfive_tdfill f = { .armed = true };
	char *line = strim(buf);
	int ret = 0;

	if (strcmp(line, "clear")) {
		ret = sgxfive_tdfill_parse(line, &f);
		if (ret)
			return ret;
		f.armed = true;
		if (!READ_ONCE(sgx->live))
			return -ENODEV;
	} else {
		f.armed = false;
	}
	spin_lock(&sgx->kpoke_lock);
	sgx->tdfill = f;
	spin_unlock(&sgx->kpoke_lock);
	return 0;
}

bool sgxfive_tdfill_place(const struct sgxfive_tdfill *f, u8 *td, unsigned int td_pages)
{
	if (f->page >= td_pages)
		return false;
	sgxfive_tdfill_apply(f, (u32 *)(td + ((size_t)f->page << PAGE_SHIFT)));
	return true;
}

bool sgxfive_tdfill_take(struct sgxfive *sgx, struct sgxfive_tdfill *f)
{
	bool armed;

	spin_lock(&sgx->kpoke_lock);
	armed = sgx->tdfill.armed;
	*f = sgx->tdfill;
	sgx->tdfill.armed = false;
	spin_unlock(&sgx->kpoke_lock);
	return armed;
}

void sgxfive_keep_init(struct sgxfive_keep *k)
{
	spin_lock_init(&k->lock);
	INIT_LIST_HEAD(&k->list);
	k->n = 0;
}

void sgxfive_keep_park(struct sgxfive_keep *k, struct sgxfive_kept *kept)
{
	spin_lock(&k->lock);
	list_add_tail(&kept->link, &k->list);
	k->n++;
	spin_unlock(&k->lock);
}

unsigned int sgxfive_keep_drain(struct sgxfive_keep *k,
				void (*fn)(struct sgxfive_kept *kept, void *arg), void *arg)
{
	struct sgxfive_kept *kept, *next;
	LIST_HEAD(taken);
	unsigned int n;

	spin_lock(&k->lock);
	list_splice_init(&k->list, &taken);
	n = k->n;
	k->n = 0;
	spin_unlock(&k->lock);
	list_for_each_entry_safe(kept, next, &taken, link)
		fn(kept, arg);
	return n;
}

size_t sgxfive_keep_read(struct sgxfive_keep *k, void *dst, size_t len, loff_t off)
{
	struct sgxfive_kept *kept;
	size_t n = 0;

	spin_lock(&k->lock);
	kept = list_last_entry_or_null(&k->list, struct sgxfive_kept, link);
	if (kept)
		n = sgxfive_pages_copy(kept->td, kept->td_pages, dst, len, off);
	spin_unlock(&k->lock);
	return n;
}
