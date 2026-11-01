/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Kernel pokes (CONFIG_DRM_GMA500_SGXFIVE_DEBUG): a privileged tester changes
 * words of the firmware command the kernel builds for the next job, or of the
 * render context structures the job runs with, through debugfs
 * dri/N/sgx/kpoke, by structure and word index, never by name:
 *
 *   ta:N   word N of a render's TA command (SGX_RENDER_CMD_STRIDE bytes)
 *   rd:N   word N of a render's render details (SGX_RENDER_RD_SIZE bytes)
 *   tq:N   word N of a transfer command (SGX_XFER_CMD_SIZE bytes)
 *   ctx:N  word N of the render context's HW context (SGX_RENDER_CTX_SIZE)
 *   rtds:N word N of its RT data set (SGX_RENDER_RTDS_SIZE)
 *   pb:N   word N of its PB descriptor (SGX_RENDER_PBDESC_SIZE)
 *   kw:N   word N of the words the kernel keeps for it: the access word,
 *          the pending count, the render target's status, the background
 *          primary-PDS update list (SGXFIVE_RCTX_ACCESS_OFF on)
 *   th:N   word N of a transfer context's HW context (SGX_XFER_CTX_SIZE):
 *          its flags, priority, page directory, client CCB and control block
 *   bg:N   word N of a render's background object (SGX_XFER_BGOBJ_DWORDS):
 *          the three PDS state words, six zero words, the rectangle's vertices
 *
 * SPACE:N=V sets the word, SPACE:N^=V flips the bits of V in what the kernel
 * built. A poke applies once, to the next job of its kind (ta and rd to the
 * next render submitted, tq to the next transfer submitted that is not a
 * dummy; ctx, rtds, pb, kw and bg to the next render the kernel kicks, after
 * it has written its own words there (the EDM then writes bg words 0-2 again
 * at the kick, so a poke there is lost, and a bg poke is lost too when the
 * render is replayed after a recovery, which rebuilds the object unpoked); th
 * to the next transfer submitted, a
 * dummy too, in the HW context of the context it is submitted on, where it
 * stays until another poke or the context goes), and is then dropped. debugfs
 * dri/N/sgx/render_words shows the render spaces' words of the newest render
 * context, a line each in the same spelling. Every word may be poked, the
 * addresses the kernel fills included: a poked address can make the job
 * fault, hang, or make the firmware write where it should not (the job fails
 * and is recovered; a firmware that no longer recovers needs a reboot).
 *
 * And the raw register writes of debugfs dri/N/sgx/kreg, only to registers
 * the firmware never touches (sgxfive_kreg_allowed), only while the firmware
 * runs and has no job.
 */
#ifndef _SGXFIVE_KPOKE_H_
#define _SGXFIVE_KPOKE_H_

#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct sgxfive;

enum sgxfive_kpoke_space {
	SGXFIVE_KPOKE_TA,
	SGXFIVE_KPOKE_RD,
	SGXFIVE_KPOKE_TQ,
	SGXFIVE_KPOKE_CTX,
	SGXFIVE_KPOKE_RTDS,
	SGXFIVE_KPOKE_PB,
	SGXFIVE_KPOKE_KW,
	SGXFIVE_KPOKE_TH,
	SGXFIVE_KPOKE_BG,
	SGXFIVE_KPOKE_SPACES,
};

#define SGXFIVE_KPOKE_MAX	16	/* armed at once */
#define SGXFIVE_KPOKE_KW_WORDS	4	/* access, pending, RT status, bgpri */
#define SGXFIVE_KPOKE_LINE	32	/* "tq:305^=0xffffffff", a NUL, room */

struct sgxfive_kpoke {
	u32 space;		/* enum sgxfive_kpoke_space */
	u32 index;		/* word */
	u32 value;
	bool flip;		/* ^=: XOR value in; =: set to value */
};

struct sgxfive_kpokes {
	unsigned int n;
	struct sgxfive_kpoke p[SGXFIVE_KPOKE_MAX];
};

/* The words of a space; 0 for no space. */
u32 sgxfive_kpoke_words(u32 space);

/* SPACE:INDEX=VALUE or ^=VALUE (numbers as kstrtou32 base 0): -EINVAL, -ERANGE. */
int sgxfive_kpoke_parse(const char *s, struct sgxfive_kpoke *p);
void sgxfive_kpoke_format(const struct sgxfive_kpoke *p, char *buf, size_t size);

/* Arm n pokes after those armed, all or none: -ENOSPC, -ERANGE. */
int sgxfive_kpoke_arm(struct sgxfive_kpokes *k, const struct sgxfive_kpoke *p,
		      unsigned int n);

/*
 * Apply, in order, the armed pokes of a job's kind to its firmware image (a
 * render's, sgxfive_render_build, or a transfer's, sgxfive_job_build) and
 * drop them; the others stay armed. Returns how many applied, copied to done
 * (SGXFIVE_KPOKE_MAX entries).
 */
unsigned int sgxfive_kpoke_take(struct sgxfive_kpokes *k, u8 *img, bool render,
				struct sgxfive_kpoke *done);

/*
 * Apply, in order, the armed pokes of the spaces a render context's structures
 * hold (ctx, rtds, pb, kw) to the words at words[space], and drop them; the
 * others stay armed. Returns how many applied, copied to done.
 */
unsigned int sgxfive_kpoke_take_run(struct sgxfive_kpokes *k,
				    u32 *const words[SGXFIVE_KPOKE_SPACES],
				    struct sgxfive_kpoke *done);

/*
 * At a transfer job's submission, on its context's HW context (SGX_XFER_CTX_SIZE
 * bytes at hw): the armed th pokes. The words are the ones the EDM reads, so
 * the writes are ordered before the job goes to the firmware.
 */
void sgxfive_kpoke_xfer(struct sgxfive *sgx, u32 *hw);

/* The debugfs side: arm the lines of buf ("clear" drops what is armed). */
int sgxfive_kpoke_write(struct sgxfive *sgx, char *buf);
/* At submission, on the job's built image: the armed pokes of its kind. */
void sgxfive_kpoke_job(struct sgxfive *sgx, u8 *img, bool render);
/* Before a render's kick, on its context's structures: the armed pokes of theirs. */
void sgxfive_kpoke_run(struct sgxfive *sgx, u32 *const words[SGXFIVE_KPOKE_SPACES]);

/* The registers debugfs kreg may write, sorted. */
#define SGXFIVE_KREG_COUNT	10
extern const u32 sgxfive_kreg_allowed[SGXFIVE_KREG_COUNT];
bool sgxfive_kreg_ok(u32 off);

/* OFFSET=VALUE or OFFSET^=VALUE: -EINVAL, or -EPERM off the allow-list. */
int sgxfive_kreg_parse(const char *s, u32 *off, bool *flip, u32 *value);

/* Write one allowed register (sgxfive_debug.c): -ENODEV, -EPERM, -EBUSY. */
int sgxfive_kreg_write(struct sgxfive *sgx, u32 off, bool flip, u32 value);

/*
 * The registers debugfs kregnow may write at any time, also while a job runs
 * or is held: CR_CLKGATECTL2, the two host event enables
 * (CR_EVENT_HOST_ENABLE2, CR_EVENT_HOST_ENABLE) and the register at 0x0740
 * (SGX_DPM_TASK_FIELDS), none of which the firmware writes after its boot
 * (it reads 0x0740 at a TA kick). Sorted; none is on kreg's list. Same lines
 * as kreg: OFFSET=VALUE or OFFSET^=VALUE (-EPERM off the list).
 */
#define SGXFIVE_KREGNOW_COUNT	4
extern const u32 sgxfive_kregnow_allowed[SGXFIVE_KREGNOW_COUNT];
bool sgxfive_kregnow_ok(u32 off);
int sgxfive_kregnow_parse(const char *s, u32 *off, bool *flip, u32 *value);

/* Write one of them (sgxfive_debug.c): -ENODEV, -EPERM. The write is logged. */
int sgxfive_kregnow_write(struct sgxfive *sgx, u32 off, bool flip, u32 value);

/*
 * The host's SGX interrupts, counted (debugfs kregnow): every one, and the
 * ones no EDM SW_EVENT came with while the host event enables were written
 * (an event the firmware did not enable raised them). That event's status
 * is kept and the enables are put back at once, so that an event the host
 * does not clear cannot storm. saved is what the enables held before the
 * first write of them since the load or reset.
 *
 * lock makes the write of an enable (the arm before it, the register read and
 * the write) and the interrupt's note and restore each one step, so that the
 * restore cannot land between the arm and the write. It guards armed, last
 * and saved; arm and note run with it held (irqsave), reset takes it.
 */
struct sgxfive_hostirq {
	spinlock_t lock;
	atomic_t irqs;		/* host interrupts of the live firmware */
	atomic_t extra;		/* of them, the extra events' */
	u32 last[2];		/* the status registers (EVENT_STATUS, 2) at the last */
	u32 saved[2];		/* CR_EVENT_HOST_ENABLE and ..2 before the writes */
	bool armed;		/* the enables were written */
};

void sgxfive_hostirq_init(struct sgxfive_hostirq *h);
/* With h->lock held: remember the enables as they were before the first write. */
void sgxfive_hostirq_arm(struct sgxfive_hostirq *h, u32 enable, u32 enable2);
/*
 * With h->lock held: count one interrupt; true when the caller must put saved[]
 * back (and has).
 */
bool sgxfive_hostirq_note(struct sgxfive_hostirq *h, u32 stat_1, u32 stat_2);
/* Start the counts over and disarm (takes the lock). */
void sgxfive_hostirq_reset(struct sgxfive_hostirq *h);

/*
 * debugfs tadata_fill: before the next render's kick, one page of its
 * context's TA data is filled with a value ("PAGE VALUE") or a ramp ("PAGE
 * VALUE ramp": the word's index added), to see what a unit reads or writes
 * there. PAGE counts pages of the TA data from its start; it must be within
 * the largest TA data a context can have, and within this context's own when
 * the fill is applied (else it is dropped, and logged). Taken once.
 */
struct sgxfive_tdfill {
	bool armed;
	bool ramp;
	u32 page;
	u32 value;
};

/* -EINVAL, -ERANGE for a page past the largest TA data. */
int sgxfive_tdfill_parse(const char *s, struct sgxfive_tdfill *f);
/* Fill the PAGE_SIZE bytes at page with the fill's words. */
void sgxfive_tdfill_apply(const struct sgxfive_tdfill *f, u32 *page);
/* One line, or "clear": arm (the firmware must run: -ENODEV) or drop the fill. */
int sgxfive_tdfill_write(struct sgxfive *sgx, char *buf);
/* Take the armed fill, once. */
bool sgxfive_tdfill_take(struct sgxfive *sgx, struct sgxfive_tdfill *f);
/*
 * Fill its page of a context's TA data (td_pages pages at td); false, with
 * nothing written, for a page past the context's own.
 */
bool sgxfive_tdfill_place(const struct sgxfive_tdfill *f, u8 *td, unsigned int td_pages);

/*
 * Switches a tester arms from debugfs for the next thing that reads them
 * (alias_ctx_rw, render_share_pb): taken once, whoever reads them first.
 */
bool sgxfive_oneshot_take(bool *armed);

/* The VM flags of a render context's page, writable when rw (debugfs alias_ctx_rw). */
u32 sgxfive_ctx_page_flags(bool rw);

/*
 * debugfs hwperf: the counter group (HostCtl dword 15, which the firmware
 * puts in the HWPerf control register's bits 12:9) and the data word of the
 * firmware's SETHWPERFSTATUS command (bit 0: apply; the rest becomes its
 * HWPerf status; any status but 0 selects the group, bits 1-3 also record
 * events). Reading shows the control register and the registers the
 * firmware's HWPerf routine reads.
 */
#define SGXFIVE_HWPERF_NREGS	18
extern const u32 sgxfive_hwperf_regs[SGXFIVE_HWPERF_NREGS];

/* "GROUP WORD" (numbers as kstrtou32 base 0): -EINVAL, -ERANGE past group 15. */
int sgxfive_hwperf_parse(const char *s, u32 *group, u32 *word);

/* Set the group and send the command (sgxfive_debug.c): -ENODEV, -EBUSY, -ENXIO, -ETIMEDOUT. */
int sgxfive_hwperf_set(struct sgxfive *sgx, u32 group, u32 word);

#endif
