// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sgxfive contexts. A transfer context is a firmware HW transfer context with
 * its own client CCB, in kernel memory the EDM reads through its own page
 * directory, bound for its lifetime to one client VM whose directory the
 * firmware gives the context's 3D work.
 */
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>

#include "sgx_xfer.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

/*
 * CREATE_CONTEXT's arguments: no flags, the one priority, zero padding.
 * A transfer context states no render target size: both fields stay zero.
 */
int sgxfive_ctx_check_args(const struct drm_sgxfive_create_context_args *a)
{
	if (a->flags || a->priority != DRM_SGXFIVE_CTX_PRIORITY_NORMAL ||
	    a->_padding_1c)
		return -EINVAL;
	if (a->type == DRM_SGXFIVE_CTX_TYPE_TRANSFER)
		return a->max_width || a->max_height ? -EINVAL : 0;
	return -EINVAL;
}

static void sgxfive_ctx_release(struct kref *ref)
{
	struct sgxfive_ctx *ctx = container_of(ref, struct sgxfive_ctx, ref);
	struct sgxfive *sgx = ctx->sgx;
	sgxfive_kmem_free(sgx, &ctx->node, &ctx->mem);
	sgxfive_vm_put(ctx->vm);
	kfree(ctx);
}

void sgxfive_ctx_put(struct sgxfive_ctx *ctx)
{
	kref_put(&ctx->ref, sgxfive_ctx_release);
}

struct sgxfive_ctx *sgxfive_ctx_get(struct sgxfive_file *f, u32 handle)
{
	struct sgxfive_ctx *ctx;

	xa_lock(&f->ctxs);
	ctx = xa_load(&f->ctxs, handle);
	if (ctx)
		kref_get(&ctx->ref);
	xa_unlock(&f->ctxs);
	return ctx;
}

int sgxfive_ioctl_create_ctx(struct sgxfive *sgx, void *data,
			     struct drm_file *file)
{
	struct drm_sgxfive_create_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_ctx *ctx;
	int ret;

	if (sgxfive_ctx_check_args(a))
		return -EINVAL;
	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->vm = sgxfive_vm_get(f, a->vm_context_handle);
	if (!ctx->vm) {
		kfree(ctx);
		return -EINVAL;
	}
	ret = sgxfive_ensure_live(sgx);
	if (!ret)
		ret = sgxfive_kmem_alloc(sgx, &ctx->node, &ctx->mem,
					 SGXFIVE_CTX_MEM_SIZE);
	if (ret) {
		sgxfive_vm_put(ctx->vm);
		kfree(ctx);
		return ret;
	}
	ctx->sgx = sgx;
	ctx->type = a->type;
	ctx->pd_phys = sgxfive_vm_pd_phys(ctx->vm);
	kref_init(&ctx->ref);
	/* Priority 0, the client CCB and its control block, the client PD. */
	sgx_xfer_ctx_init(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_HW_OFF), ctx->pd_phys,
			  ctx->mem.addr + SGXFIVE_CTX_CCB_OFF,
			  ctx->mem.addr + SGXFIVE_CTX_CTL_OFF);
	ret = xa_alloc(&f->ctxs, &a->handle, ctx,
		       XA_LIMIT(1, DRM_SGXFIVE_FILE_MAX_CONTEXTS), GFP_KERNEL);
	if (ret)
		sgxfive_ctx_put(ctx);
	return ret == -EBUSY ? -ENOSPC : ret;
}

int sgxfive_ioctl_destroy_ctx(struct sgxfive *sgx, void *data,
			      struct drm_file *file)
{
	struct drm_sgxfive_destroy_context_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_ctx *ctx;

	if (a->_padding_4)
		return -EINVAL;
	ctx = xa_erase(&f->ctxs, a->handle);
	if (!ctx)
		return -EINVAL;
	sgxfive_ctx_put(ctx);
	return 0;
}

/* On close: contexts go before the VMs they hold. */
void sgxfive_ctx_close_all(struct sgxfive_file *f)
{
	struct sgxfive_ctx *ctx;
	unsigned long i;

	xa_for_each(&f->ctxs, i, ctx)
		sgxfive_ctx_put(ctx);
	xa_destroy(&f->ctxs);
}
