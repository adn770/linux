/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sgxfive contexts (sgxfive_ctx.c): transfer contexts.
 */
#ifndef _SGXFIVE_CTX_H_
#define _SGXFIVE_CTX_H_

#include <linux/kref.h>

#include <drm/drm_mm.h>

#include "sgx_xfer.h"
#include "sgxfive_priv.h"

/*
 * A context's kernel memory: the client CCB, then one page of control data:
 * the HW transfer context and the CCB's control block.
 */
#define SGXFIVE_CTX_CCB_OFF	0
#define SGXFIVE_CTX_PAGE_OFF	ALIGN(SGX_XFER_CCB_SIZE + SGX_XFER_CMD_SIZE, PAGE_SIZE)
#define SGXFIVE_CTX_HW_OFF	(SGXFIVE_CTX_PAGE_OFF + 0x000)	/* HW context */
#define SGXFIVE_CTX_CTL_OFF	(SGXFIVE_CTX_PAGE_OFF + 0x100)	/* CCB control */
#define SGXFIVE_CTX_MEM_SIZE	(SGXFIVE_CTX_PAGE_OFF + PAGE_SIZE)

struct sgxfive_ctx {
	struct kref ref;
	struct sgxfive *sgx;
	u32 type;			/* enum drm_sgxfive_ctx_type */
	struct sgxfive_vm *vm;		/* holds a reference */
	u32 pd_phys;			/* the VM's page directory */
	struct drm_mm_node node;	/* in the kernel data heap */
	struct sgxfive_buf mem;
};

static inline void *sgxfive_ctx_cpu(struct sgxfive_ctx *ctx, u32 off)
{
	return (u8 *)ctx->mem.va + off;
}

struct sgxfive_ctx *sgxfive_ctx_get(struct sgxfive_file *f, u32 handle);
void sgxfive_ctx_put(struct sgxfive_ctx *ctx);
void sgxfive_ctx_close_all(struct sgxfive_file *f);
int sgxfive_ioctl_create_ctx(struct sgxfive *sgx, void *data,
			     struct drm_file *file);
int sgxfive_ioctl_destroy_ctx(struct sgxfive *sgx, void *data,
			      struct drm_file *file);

#endif
