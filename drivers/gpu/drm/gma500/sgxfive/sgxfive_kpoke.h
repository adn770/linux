/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Kernel pokes (CONFIG_DRM_GMA500_SGXFIVE_DEBUG): a privileged tester changes
 * words of the firmware command the kernel builds for the next job, through
 * debugfs dri/N/sgx/kpoke, by structure and word index, never by name:
 *
 *   ta:N   word N of a render's TA command (SGX_RENDER_CMD_STRIDE bytes)
 *   rd:N   word N of a render's render details (SGX_RENDER_RD_SIZE bytes)
 *   tq:N   word N of a transfer command (SGX_XFER_CMD_SIZE bytes)
 *
 * SPACE:N=V sets the word, SPACE:N^=V flips the bits of V in what the kernel
 * built. A poke applies once, to the next job of its kind submitted (ta and
 * rd to the next render, tq to the next transfer that is not a dummy), and is
 * then dropped. Every word may be poked, the addresses the kernel fills
 * included: a poked address can make the job fault, hang, or make the
 * firmware write where it should not (the job fails and is recovered; a
 * firmware that no longer recovers needs a reboot).
 *
 * And the raw register writes of debugfs dri/N/sgx/kreg, only to registers
 * the firmware never touches (sgxfive_kreg_allowed), only while the firmware
 * runs and has no job.
 */
#ifndef _SGXFIVE_KPOKE_H_
#define _SGXFIVE_KPOKE_H_

#include <linux/types.h>

struct sgxfive;

enum sgxfive_kpoke_space {
	SGXFIVE_KPOKE_TA,
	SGXFIVE_KPOKE_RD,
	SGXFIVE_KPOKE_TQ,
	SGXFIVE_KPOKE_SPACES,
};

#define SGXFIVE_KPOKE_MAX	16	/* armed at once */
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

/* The debugfs side: arm the lines of buf ("clear" drops what is armed). */
int sgxfive_kpoke_write(struct sgxfive *sgx, char *buf);
/* At submission, on the job's built image: the armed pokes of its kind. */
void sgxfive_kpoke_job(struct sgxfive *sgx, u8 *img, bool render);

/* The registers debugfs kreg may write, sorted. */
#define SGXFIVE_KREG_COUNT	10
extern const u32 sgxfive_kreg_allowed[SGXFIVE_KREG_COUNT];
bool sgxfive_kreg_ok(u32 off);

/* OFFSET=VALUE or OFFSET^=VALUE: -EINVAL, or -EPERM off the allow-list. */
int sgxfive_kreg_parse(const char *s, u32 *off, bool *flip, u32 *value);

/* Write one allowed register (sgxfive_debug.c): -ENODEV, -EPERM, -EBUSY. */
int sgxfive_kreg_write(struct sgxfive *sgx, u32 off, bool flip, u32 value);

#endif
