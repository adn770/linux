// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The SGX545 boot register sequence, as the DDK's SGXInitialise runs it on the
 * first power-on (resgx545's doc/specs/boot_sequence.md, phase C): clock
 * gating, init script part 1, the hardware-recovery variant of SGXReset, init
 * script part 2, then the boot kick on EVENT_KICK2. The caller polls
 * HostCtl.ui32InitStatus.
 *
 * Unlike the DDK this does not page-map BIF faults seen during the reset: it
 * stops with the BIF still held, so the fault can be inspected.
 *
 * Once the EDM runs, the host gives it work through its kernel CCB
 * (sgx_ccb_submit(), resgx545's doc/specs/transfer.md, the kernel command
 * queue).
 */
#include "sgx_seq.h"

static void wr(const struct sgx_seq *s, u32 reg, u32 val)
{
	s->wr(s->ctx, reg, val);
}

static void delay(const struct sgx_seq *s)
{
	s->udelay(s->ctx, 1);	/* SGXWaitClocks(100) at 400 MHz, rounded up */
}

static void script(const struct sgx_seq *s, const struct sgx_fw_write *w,
		   unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		wr(s, w[i].reg, w[i].val);
}

static void inval_dc(const struct sgx_seq *s)
{
	wr(s, SGX_BIF_CTRL, SGX_BIF_CTRL_INVALDC);
	wr(s, SGX_BIF_CTRL, 0);
	delay(s);
}

/*
 * The same invalidate with the other bits of CR_BIF_CTRL kept, for a running
 * GPU: the firmware reaches the register too (debugfs bif_flush, before the
 * reset that ends a held recovery).
 */
void sgx_seq_bif_flush(const struct sgx_seq *s)
{
	const u32 ctrl = s->rd(s->ctx, SGX_BIF_CTRL) & ~SGX_BIF_CTRL_INVALDC;

	wr(s, SGX_BIF_CTRL, ctrl | SGX_BIF_CTRL_INVALDC);
	wr(s, SGX_BIF_CTRL, ctrl);
	delay(s);
}

int sgx_seq_boot(const struct sgx_seq *s)
{
	unsigned int i;

	wr(s, SGX_CLKGATECTL, s->clkgatectl);
	wr(s, SGX_CLKGATECTL2, s->clkgatectl2);
	script(s, s->part1, s->npart1);

	/* SGXReset(bHWR): all blocks into reset, the BIF contexts cleared. */
	wr(s, SGX_SOFT_RESET, SGX_SOFT_RESET_ALL);
	delay(s);
	wr(s, SGX_BIF_CTRL, 0);
	wr(s, SGX_BIF_BANK_SET, 0);
	wr(s, SGX_BIF_BANK0, 0);
	wr(s, SGX_BIF_DIR_LIST_BASE0, 0);
	for (i = 0; i < 15; i++)
		wr(s, SGX_BIF_DIR_LIST_BASE1 + 4 * i, 0);

	/* Release the BIF alone on the reset PD and check it is quiet. */
	wr(s, SGX_BIF_DIR_LIST_BASE0, s->reset_pd);
	delay(s);
	wr(s, SGX_SOFT_RESET, SGX_SOFT_RESET_ALL & ~SGX_SOFT_RESET_BIF);
	delay(s);
	inval_dc(s);
	if (s->rd(s->ctx, SGX_BIF_INT_STAT) & SGX_BIF_FAULT_REQ)
		return -EIO;

	/* The EDM translates through directory list 15. */
	wr(s, SGX_BIF_BANK0, SGX_BIF_BANK0_EDM15);
	wr(s, SGX_BIF_DIR_LIST_BASE15, s->edm_pd);
	inval_dc(s);
	wr(s, SGX_SOFT_RESET, 0);
	delay(s);

	script(s, s->part2, s->npart2);
	s->pre_kick(s->ctx);
	/* SGX_FEATURE_MULTI_EVENT_KICK: the boot kick does not bump the kicker. */
	wr(s, SGX_EVENT_KICK2, 1);
	return 0;
}

/*
 * SGXScheduleCCBCommand: write the command at the CCB write offset, advance
 * the write offset and the event-kicker word (both mod 256), then kick the
 * EDM on EVENT_KICK. ctl[0] is the host's write offset, ctl[1] the EDM's read
 * offset. service is the command's EDM handler, an entry of the firmware
 * file's kick table; 0 means this firmware build has none.
 */
int sgx_ccb_submit(const struct sgx_seq *s, u32 *ccb, u32 ctl[2], u32 *kicker,
		   u32 service, u32 cache_control, const u32 *data,
		   unsigned int ndata)
{
	u32 woff = ctl[0] % SGX_CCB_ENTRIES;
	u32 *cmd = &ccb[woff * SGX_CCB_CMD_DWORDS];
	unsigned int i;

	if (ndata > SGX_CCB_DATA_DWORDS)
		return -EINVAL;
	if (!service)
		return -ENOENT;
	if ((woff + 1) % SGX_CCB_ENTRIES == ctl[1])
		return -EBUSY;		/* full */

	cmd[0] = service;
	cmd[1] = cache_control;
	for (i = 0; i < SGX_CCB_DATA_DWORDS; i++)
		cmd[2 + i] = i < ndata ? data[i] : 0;
	ctl[0] = (woff + 1) % SGX_CCB_ENTRIES;
	*kicker = (*kicker + 1) % SGX_CCB_ENTRIES;
	s->wmb(s->ctx);		/* the EDM reads all of this after the kick */
	wr(s, SGX_EVENT_KICK, 1);
	return 0;
}
