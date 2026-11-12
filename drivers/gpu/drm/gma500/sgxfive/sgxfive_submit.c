// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * SUBMIT_JOBS for sgxfive contexts: transfers and renders.
 * A call is validated as a whole before anything is queued, its waits
 * included: a wait takes the fence its syncobj holds then, unless an earlier
 * job of the call signals that syncobj; it then waits for that job, whose
 * fence is attached by the time the waiting job is armed.
 */
#include <linux/dma-fence-chain.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>
#include <drm/gpu_scheduler.h>

#include "sgx_xfer.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

struct sgxfive_submit {
	struct drm_sgxfive_job desc;
	struct drm_sgxfive_transfer_cmd regs;
	struct drm_sgxfive_render_cmd rc;
	bool bg_layer;			/* debugfs render_bg_layer, read once at the check */
	struct drm_sgxfive_sync_op ops[SGXFIVE_MAX_SYNC_OPS];
	struct sgxfive_job *job;
	struct dma_fence_chain *chains[SGXFIVE_MAX_SYNC_OPS];
	struct dma_fence *waits[SGXFIVE_MAX_SYNC_OPS];	/* resolved at the check */
};

/*
 * One versioned struct of len bytes from userspace into size bytes: a shorter
 * one is zero-extended, a longer one's tail must be zero.
 */
static int copy_elem(void *dst, size_t size, const void __user *src, u32 len)
{
	int ret = copy_struct_from_user(dst, size, src, len);

	return ret == -E2BIG ? -EINVAL : ret;
}

/*
 * Copy the count elements of a user array, at most max, into dst, size bytes
 * each. The stride is bounded, so a call reads little whatever it asks.
 */
int sgxfive_copy_array(void *dst, size_t size, const struct drm_sgxfive_obj_array *a,
		       u32 max)
{
	u32 i;
	int ret;

	if (a->count > max || (a->count && (!a->stride ||
	    a->stride > DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE)))
		return -EINVAL;
	for (i = 0; i < a->count; i++) {
		const u8 __user *src = u64_to_user_ptr(a->array) + (u64)i * a->stride;

		ret = copy_elem((u8 *)dst + i * size, size, src, a->stride);
		if (ret)
			return ret;
	}
	return 0;
}

/* A sync op's encoding: known flags and handle type, no point on a binary syncobj. */
int sgxfive_sync_op_check(const struct drm_sgxfive_sync_op *op)
{
	u32 type = op->flags & DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK;

	if ((op->flags & ~DRM_SGXFIVE_SYNC_OP_FLAGS_MASK) ||
	    type > DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ ||
	    (type == DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ && op->value))
		return -EINVAL;
	return 0;
}

/*
 * Whether one of n sync ops signals what a wait waits for: the same binary
 * syncobj, or the same timeline at the wait's point or a later one.
 */
bool sgxfive_sync_signals(const struct drm_sgxfive_sync_op *ops, u32 n,
			  const struct drm_sgxfive_sync_op *wait)
{
	u32 type = wait->flags & DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK;
	u32 i;

	for (i = 0; i < n; i++) {
		if (!(ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL) ||
		    ops[i].handle != wait->handle ||
		    (ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK) != type)
			continue;
		if (type == DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ ||
		    ops[i].value >= wait->value)
			return true;
	}
	return false;
}

/*
 * A render job: its command, checked, its target within the context's maximum;
 * with debugfs render_bg_layer, its layer's source mapped in the context's VM.
 */
static int sgxfive_submit_check_render(struct sgxfive_submit *s,
				       const struct sgxfive_ctx *ctx)
{
	const struct drm_sgxfive_job *d = &s->desc;
	u32 len = d->cmd_stream_len, src;
	int ret;

	if (len < sizeof(s->rc) || len > PAGE_SIZE || d->cmd_stream % 8)
		return -EINVAL;
	ret = copy_elem(&s->rc, sizeof(s->rc), u64_to_user_ptr(d->cmd_stream), len);
	if (!ret)
		ret = sgxfive_render_check(&s->rc, ctx->max_width, ctx->max_height,
					   ctx->aa_ms);
	if (ret)
		return ret;
	s->bg_layer = sgxfive_render_bg_layer(ctx->sgx, &src);
	return sgxfive_render_check_bg(s->bg_layer, src,
				       s->bg_layer && src ? sgxfive_vm_lookup(ctx->vm, src) : 0);
}

static int sgxfive_submit_check(struct sgxfive_submit *s, struct sgxfive_ctx *ctx)
{
	const struct drm_sgxfive_job *d = &s->desc;
	bool dummy = d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY;
	u32 len = d->cmd_stream_len, i;
	int ret;
	u32 mask = d->type == DRM_SGXFIVE_JOB_TYPE_RENDER ?
		   DRM_SGXFIVE_SUBMIT_JOB_RENDER_FLAGS_MASK :
		   DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_FLAGS_MASK;

	if ((d->type != DRM_SGXFIVE_JOB_TYPE_TRANSFER &&
	     d->type != DRM_SGXFIVE_JOB_TYPE_RENDER) || (d->flags & ~mask) ||
	    d->updates.count || d->updates.stride || d->updates.array)
		return -EINVAL;
	/* Job and context types match (they share their values). */
	if (d->type != ctx->type)
		return -EINVAL;
	if (READ_ONCE(ctx->lost) || READ_ONCE(ctx->client->banned))
		return -ECANCELED;
	if (d->type == DRM_SGXFIVE_JOB_TYPE_RENDER) {
		ret = sgxfive_submit_check_render(s, ctx);
		if (ret)
			return ret;
	} else if (dummy) {
		if (len || d->cmd_stream)
			return -EINVAL;
	} else {
		if (len < sizeof(s->regs) || len > PAGE_SIZE || d->cmd_stream % 8)
			return -EINVAL;
		ret = copy_elem(&s->regs, sizeof(s->regs),
				u64_to_user_ptr(d->cmd_stream), len);
		if (ret)
			return ret;
		ret = sgxfive_job_check(&s->regs);
		if (ret)
			return ret;
	}
	ret = sgxfive_copy_array(s->ops, sizeof(s->ops[0]), &d->sync_ops,
				 SGXFIVE_MAX_SYNC_OPS);
	if (ret)
		return ret;
	for (i = 0; i < d->sync_ops.count; i++) {
		u32 type = s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_MASK;

		ret = sgxfive_sync_op_check(&s->ops[i]);
		if (ret)
			return ret;
		if (type == DRM_SGXFIVE_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ &&
		    (s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL)) {
			s->chains[i] = dma_fence_chain_alloc();
			if (!s->chains[i])
				return -ENOMEM;
		}
	}
	return 0;
}

/*
 * The armed pokes (debug option) of a transfer job whose command is built:
 * the tq pokes on the command, unless the job is a dummy, which has no
 * registers to poke, then the th pokes on its context's HW context, a dummy
 * too. Both land when the job is queued, not when it runs: see sgxfive_kpoke.h.
 */
void sgxfive_submit_xfer_pokes(struct sgxfive_ctx *ctx, u8 *cmd, bool dummy)
{
	if (!dummy)
		sgxfive_kpoke_job(ctx->sgx, cmd, false);
	sgxfive_kpoke_xfer(ctx->sgx, sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_HW_OFF));
}

/* Queue one validated job: dependencies, arming, its command, its signals. */
static int sgxfive_submit_queue(struct sgxfive_submit *s, struct sgxfive_ctx *ctx,
				struct drm_file *file)
{
	const struct drm_sgxfive_job *d = &s->desc;
	struct sgxfive_job *job = s->job;
	struct dma_fence *done;
	u32 i;
	int ret;

	if (ctx->closed)
		return -EINVAL;		/* destroyed since the check */
	ret = drm_sched_job_init(&job->base, &ctx->entity, 1, file,
				 file->client_id);
	if (ret)
		return ret;
	for (i = 0; i < d->sync_ops.count; i++) {
		if (s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL)
			continue;
		if (s->waits[i]) {
			ret = drm_sched_job_add_dependency(&job->base, s->waits[i]);
			s->waits[i] = NULL;	/* consumed, even on error */
		} else {
			/* An earlier job of the call has signalled it by now. */
			ret = drm_sched_job_add_syncobj_dependency(&job->base, file,
								   s->ops[i].handle,
								   s->ops[i].value);
		}
		if (ret) {
			drm_sched_job_cleanup(&job->base);
			return ret;
		}
	}

	drm_sched_job_arm(&job->base);
	job->ctx = ctx;
	job->seqno = ++ctx->seqno;
	if (ctx->type == DRM_SGXFIVE_CTX_TYPE_RENDER) {
		job->rc = s->rc;
		job->bg_layer = s->bg_layer;
		sgxfive_render_job_scene(ctx, job,
					 d->flags & DRM_SGXFIVE_SUBMIT_JOB_RENDER_GEOMETRY);
		sgxfive_render_job_build(ctx, job);
		sgxfive_kpoke_job(ctx->sgx, job->hw->cmd, true);
	} else {
		job->size = sgxfive_job_build(job->hw->cmd,
					      (d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY) ?
					      NULL : &s->regs,
					      d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY,
					      ctx->mem.addr + SGXFIVE_CTX_FENCE_OFF,
					      job->seqno);
		sgxfive_submit_xfer_pokes(ctx, job->hw->cmd,
					  d->flags & DRM_SGXFIVE_SUBMIT_JOB_TRANSFER_DUMMY);
	}
	done = &job->base.s_fence->finished;
	for (i = 0; i < d->sync_ops.count; i++) {
		struct drm_syncobj *obj;

		if (!(s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL))
			continue;
		obj = drm_syncobj_find(file, s->ops[i].handle);
		if (!obj)
			continue;	/* checked again below: nothing to signal */
		if (s->chains[i]) {
			drm_syncobj_add_point(obj, s->chains[i], done,
					      s->ops[i].value);
			s->chains[i] = NULL;
		} else {
			drm_syncobj_replace_fence(obj, done);
		}
		drm_syncobj_put(obj);
	}
	kref_get(&ctx->ref);		/* the job's reference */
	drm_sched_entity_push_job(&job->base);
	s->job = NULL;
	return 0;
}

/*
 * Every wait of job i resolved before anything is queued: the fence its
 * syncobj holds now, kept, unless an earlier job of the call signals it.
 */
static int sgxfive_submit_waits(struct sgxfive_submit *subs, u32 i,
				struct drm_file *file)
{
	struct sgxfive_submit *s = &subs[i];
	u32 k, j;
	int ret;

	for (k = 0; k < s->desc.sync_ops.count; k++) {
		const struct drm_sgxfive_sync_op *op = &s->ops[k];
		struct dma_fence *fence;
		bool earlier = false;

		if (op->flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL)
			continue;
		for (j = 0; j < i && !earlier; j++)
			earlier = sgxfive_sync_signals(subs[j].ops,
						       subs[j].desc.sync_ops.count, op);
		if (earlier)
			continue;
		/* -ENOENT without the syncobj, -EINVAL without a fence. */
		ret = drm_syncobj_find_fence(file, op->handle, op->value, 0, &fence);
		if (ret)
			return ret;
		s->waits[k] = fence;
	}
	return 0;
}

/* Every signal syncobj must exist before anything is queued. */
static int sgxfive_submit_signals_exist(struct sgxfive_submit *s,
					struct drm_file *file)
{
	u32 i;

	for (i = 0; i < s->desc.sync_ops.count; i++) {
		struct drm_syncobj *obj;

		if (!(s->ops[i].flags & DRM_SGXFIVE_SYNC_OP_FLAG_SIGNAL))
			continue;
		obj = drm_syncobj_find(file, s->ops[i].handle);
		if (!obj)
			return -ENOENT;
		drm_syncobj_put(obj);
	}
	return 0;
}

int sgxfive_ioctl_submit(struct sgxfive *sgx, void *data, struct drm_file *file)
{
	struct drm_sgxfive_submit_jobs_args *a = data;
	struct sgxfive_file *f = file->driver_priv;
	struct sgxfive_ctx *ctxs[SGXFIVE_MAX_JOBS] = { NULL };
	struct drm_sgxfive_job *descs;
	struct sgxfive_submit *subs;
	u32 n = a->jobs.count, i, j;
	int ret;

	if (!n || n > SGXFIVE_MAX_JOBS)
		return -EINVAL;
	descs = kcalloc(n, sizeof(*descs), GFP_KERNEL);
	subs = kcalloc(n, sizeof(*subs), GFP_KERNEL);
	ret = descs && subs ? sgxfive_copy_array(descs, sizeof(*descs), &a->jobs,
						 SGXFIVE_MAX_JOBS) : -ENOMEM;
	if (ret) {
		kfree(descs);
		kfree(subs);
		return ret;
	}
	for (i = 0; i < n; i++)
		subs[i].desc = descs[i];
	kfree(descs);

	for (i = 0; i < n && !ret; i++) {
		ctxs[i] = sgxfive_ctx_get(f, subs[i].desc.context_handle);
		ret = ctxs[i] ? sgxfive_submit_check(&subs[i], ctxs[i]) : -EINVAL;
		if (!ret)
			ret = sgxfive_submit_signals_exist(&subs[i], file);
		if (!ret)
			ret = sgxfive_submit_waits(subs, i, file);
		if (!ret) {
			subs[i].job = kzalloc_obj(*subs[i].job);
			if (subs[i].job)
				subs[i].job->hw = sgxfive_fence_alloc();
			ret = subs[i].job && subs[i].job->hw ? 0 : -ENOMEM;
		}
	}
	if (ret) {
		a->jobs.count = i ? i - 1 : 0;	/* the failing job */
		goto out;
	}

	for (i = 0; i < n; i++) {
		mutex_lock(&ctxs[i]->submit_lock);
		ret = sgxfive_submit_queue(&subs[i], ctxs[i], file);
		mutex_unlock(&ctxs[i]->submit_lock);
		if (ret) {
			a->jobs.count = i;
			break;
		}
	}
out:
	for (i = 0; i < n; i++) {
		if (subs[i].job) {		/* NULL once queued */
			kfree(subs[i].job->hw);
			kfree(subs[i].job);
		}
		for (j = 0; j < SGXFIVE_MAX_SYNC_OPS; j++) {
			dma_fence_chain_free(subs[i].chains[j]);
			dma_fence_put(subs[i].waits[j]);
		}
		if (ctxs[i])
			sgxfive_ctx_put(ctxs[i]);
	}
	kfree(subs);
	return ret;
}
