// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sgxfive render contexts. A render context is a firmware HW render context
 * with its TA client CCB, one RT data set and one parameter buffer. The
 * firmware structures live in the context's kernel memory, which the EDM
 * reads through its own page directory and, while it loads the parameter
 * buffer, through the context's (so the VM aliases it read-only). The memory
 * the TA, DPM and 3D pipe write lives in the VM itself, in the context's
 * slots of the reserved PARAMETERS and TA_DATA heaps: the PB pages, their
 * page table, the scratch primitive block, the background object and the RT
 * data's buffers, sized for the largest target the context was created for.
 *
 * Each job is one kick of a scene, built at submission into its hardware
 * fence (sgxfive_render_build): the TA command and the render details. One
 * render job per context is in the firmware at a time (sgxfive_sched.c), so
 * running a job may rewrite the render details and the background object in
 * place.
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>

#include <drm/drm_print.h>

#include "sgx_mmu.h"
#include "sgx_render.h"
#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"

/* The parameter slot is mapped from its PB; the CPU sees from the table on. */
#define PARAM_PAGES	((SGX_RENDER_PARAM_SIZE - SGX_RENDER_PARAM_PB) >> PAGE_SHIFT)
#define CPU_FIRST	((SGX_RENDER_PARAM_PT - SGX_RENDER_PARAM_PB) >> PAGE_SHIFT)
#define CPU_PAGES	(PARAM_PAGES - CPU_FIRST)

struct sgxfive_render {
	struct sgxfive_ctx *ctx;
	unsigned int slot;
	struct sgx_render_addrs a;
	struct page *param[PARAM_PAGES];
	struct page **td;		/* the RT data's buffers: a.td.size */
	unsigned int td_pages;
	u8 *cpu;			/* page table, scratch, background */
	u8 *tdva;			/* the RT data's buffers */
	bool mapped;			/* in the VM */
	u32 pb_pages;			/* its PB, as set at creation */
	bool pb_shared;			/* it runs on a sibling's PB (debug option) */
	struct sgxfive_ctx *donor;	/* that PB's context, referenced until this is freed */
	struct sgxfive_deferred release;
#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
	struct sgxfive_kept kept;	/* parked here when closed with debugfs render_keep */
#endif
};

static u32 *rctx(struct sgxfive_ctx *ctx, u32 off)
{
	return sgxfive_ctx_cpu(ctx, off);
}

static u32 *render_bg(struct sgxfive_render *r)
{
	return (u32 *)(r->cpu + SGX_RENDER_PARAM_BG - SGX_RENDER_PARAM_PT);
}

#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
/*
 * The context's structures by kpoke space (sgxfive_kpoke.h): the HW context,
 * the RT data set, the PB descriptor, the kernel's own words, the render
 * details and the background object.
 */
static void render_spaces(struct sgxfive_ctx *ctx, u32 *w[SGXFIVE_KPOKE_SPACES])
{
	struct sgxfive_render *r = READ_ONCE(ctx->render);

	memset(w, 0, SGXFIVE_KPOKE_SPACES * sizeof(*w));
	w[SGXFIVE_KPOKE_CTX] = rctx(ctx, SGXFIVE_CTX_HW_OFF);
	w[SGXFIVE_KPOKE_RTDS] = rctx(ctx, SGXFIVE_RCTX_RTDS_OFF);
	w[SGXFIVE_KPOKE_PB] = rctx(ctx, SGXFIVE_RCTX_PBDESC_OFF);
	w[SGXFIVE_KPOKE_KW] = rctx(ctx, SGXFIVE_RCTX_ACCESS_OFF);
	w[SGXFIVE_KPOKE_RD] = rctx(ctx, SGXFIVE_RCTX_RD_OFF);
	w[SGXFIVE_KPOKE_BG] = r ? render_bg(r) : NULL;
}
#endif

static bool sgxfive_render_share_pb(struct sgxfive_ctx *ctx, struct sgxfive_render *r);

static void free_pages_wc(struct page **pages, unsigned int n)
{
	unsigned int i;

	if (!pages || !n)
		return;
	if (pages[0])
		set_pages_array_wb(pages, n);
	for (i = 0; i < n; i++)
		if (pages[i])
			__free_page(pages[i]);
}

/*
 * Zeroed lowmem pages (below 4 GiB), set write-combined like the firmware's,
 * charged to the memory cgroup of the client that creates the context.
 */
static int alloc_pages_wc(struct page **pages, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		pages[i] = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!pages[i]) {
			while (i--) {
				__free_page(pages[i]);
				pages[i] = NULL;
			}
			return -ENOMEM;
		}
	}
	set_pages_array_wc(pages, n);
	return 0;
}

/*
 * The context's firmware structures as at creation (and after a reset): a
 * new HW render context, an empty TA CCB, a free RT data, the PB with its
 * initial free list and the host-built page table, zeroed tiling data.
 */
static void sgxfive_render_init_mem(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;
	u32 *ctl = rctx(ctx, SGXFIVE_CTX_CTL_OFF);

	sgx_render_ctx(rctx(ctx, SGXFIVE_CTX_HW_OFF), &r->a);
	ctl[0] = 0;
	ctl[1] = 0;
	/* Sized for the smallest target until a job says otherwise. */
	sgx_render_rtds(rctx(ctx, SGXFIVE_RCTX_RTDS_OFF), &r->a, 0);
	sgx_render_pb(rctx(ctx, SGXFIVE_RCTX_PBDESC_OFF),
		      rctx(ctx, SGXFIVE_RCTX_PBBLOCK_OFF), (u32 *)r->cpu, &r->a,
		      r->pb_pages);
	*rctx(ctx, SGXFIVE_RCTX_ACCESS_OFF) = 0;
	*rctx(ctx, SGXFIVE_RCTX_PENDING_OFF) = 0;
	*rctx(ctx, SGXFIVE_RCTX_RTSTATUS_OFF) = 0;
	*rctx(ctx, SGXFIVE_CTX_TA_OFF) = *rctx(ctx, SGXFIVE_CTX_FENCE_OFF);
	memset(r->tdva, 0, r->a.td.size);
	ctx->woff = 0;
}

static void sgxfive_render_unmap(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;

	if (!r->mapped)
		return;
	sgxfive_vm_unmap_pages(ctx->vm, r->a.param_base + SGX_RENDER_PARAM_PB,
			       PARAM_PAGES);
	sgxfive_vm_unmap_pages(ctx->vm, r->a.tadata, r->td_pages);
	sgxfive_vm_unmap_pages(ctx->vm, ctx->mem.addr, ctx->mem.npages);
	r->mapped = false;
}

static int sgxfive_render_map(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;
	int ret;

	r->mapped = true;
	ret = sgxfive_vm_map_pages(ctx->vm, r->a.param_base + SGX_RENDER_PARAM_PB,
				   r->param, PARAM_PAGES, 0);
	if (!ret)
		ret = sgxfive_vm_map_pages(ctx->vm, r->a.tadata, r->td, r->td_pages, 0);
	/*
	 * The EDM reads the PB descriptor here through the context's directory.
	 * Read-only, but for the context a tester asked a writable page for
	 * (debugfs alias_ctx_rw): the EDM's own writes there then do not fault.
	 */
	if (!ret)
		ret = sgxfive_vm_map_pages(ctx->vm, ctx->mem.addr, ctx->mem.pages,
					   ctx->mem.npages,
					   sgxfive_ctx_page_flags(sgxfive_take_ctx_rw(ctx->sgx)));
	if (ret)
		sgxfive_render_unmap(r);
	return ret;
}

/*
 * Everything but the VM's slot, which the caller still holds. The context
 * whose PB this one ran on (debug option) is let go last: its memory and PB
 * stay until nothing of this context can name them.
 */
static void sgxfive_render_free(struct sgxfive_render *r)
{
	struct sgxfive_ctx *donor = r->donor;

	if (r->cpu)
		vunmap(r->cpu);
	if (r->tdva)
		vunmap(r->tdva);
	free_pages_wc(r->param, PARAM_PAGES);
	free_pages_wc(r->td, r->td_pages);
	kfree(r->td);
	kfree(r);
	if (donor)
		sgxfive_ctx_put(donor);
}

/*
 * A new render context, its kernel memory allocated, its VM and its largest
 * target (ctx->max_width x max_height, checked) set: the slot and the
 * firmware alias in the VM, the slot's pages, the TA data for that target,
 * the structures.
 */
int sgxfive_render_init(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = kzalloc_obj(*r, GFP_KERNEL_ACCOUNT);
	struct sgx_render_tadata td;
	int ret;

	if (!r)
		return -ENOMEM;
	ret = sgx_render_tadata_layout(&td, ctx->max_width, ctx->max_height);
	if (ret) {
		kfree(r);
		return ret;
	}
	r->ctx = ctx;
	r->td_pages = td.size >> PAGE_SHIFT;
	r->td = kcalloc(r->td_pages, sizeof(*r->td), GFP_KERNEL_ACCOUNT);
	if (!r->td) {
		kfree(r);
		return -ENOMEM;
	}
	ret = sgxfive_vm_render_attach(ctx->vm, &r->slot);
	if (ret) {
		kfree(r->td);
		kfree(r);
		return ret;
	}
	sgxfive_render_addrs(&r->a, ctx->mem.addr, sgxfive_vm_pd_phys(ctx->vm), r->slot,
			     &td);
	r->pb_shared = sgxfive_render_share_pb(ctx, r);
	r->pb_pages = sgxfive_render_pb_pages(ctx->sgx);
	r->a.ctx_flags = sgxfive_render_ctx_flags(ctx->sgx);
	ret = alloc_pages_wc(r->param, PARAM_PAGES);
	if (!ret)
		ret = alloc_pages_wc(r->td, r->td_pages);
	if (!ret) {
		r->cpu = vmap(r->param + CPU_FIRST, CPU_PAGES, VM_MAP,
			      pgprot_writecombine(PAGE_KERNEL));
		r->tdva = vmap(r->td, r->td_pages, VM_MAP,
			       pgprot_writecombine(PAGE_KERNEL));
		ret = r->cpu && r->tdva ? 0 : -ENOMEM;
	}
	if (!ret)
		ret = sgxfive_render_map(r);
	if (ret) {
		sgxfive_vm_render_detach(ctx->vm, r->slot);
		sgxfive_render_free(r);
		return ret;
	}
	sgxfive_render_init_mem(r);
	wmb(); /* the write-combined structures, before the first kick */
	ctx->render = r;
	return 0;
}

/* After the BIF has dropped what it cached of them: pages, memory, the rest. */
static void sgxfive_render_release(struct sgxfive_deferred *d)
{
	struct sgxfive_render *r = container_of(d, struct sgxfive_render, release);
	struct sgxfive_ctx *ctx = r->ctx;

	sgxfive_kmem_free(ctx->sgx, &ctx->node, &ctx->mem);
	sgxfive_vm_put(ctx->vm);
	sgxfive_client_put(ctx->client);
	mutex_destroy(&ctx->submit_lock);
	kfree(ctx);
	sgxfive_render_free(r);
}

/*
 * A render context's CLEANUP got no answer: ask the blame-free reset. While a
 * recovery is held (debug option) that reset is dropped, the hold stays and
 * the reset that ends it resets for both (sgxfive_recovery_hold). The reset
 * says which, under the device lock, and logs it. Returns whether a reset was
 * asked for and not dropped.
 */
bool sgxfive_cleanup_reset(struct sgxfive *sgx)
{
	bool dropped;

	__sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_CLEANUP, &dropped);
	return !dropped;
}

/* Its mappings and slot given back, its memory freed once no stale translation can reach it. */
static void sgxfive_render_drop(struct sgxfive_render *r)
{
	struct sgxfive_ctx *ctx = r->ctx;

	sgxfive_render_unmap(r);
	sgxfive_vm_render_detach(ctx->vm, r->slot);
	r->release.free = sgxfive_render_release;
	sgxfive_defer(ctx->sgx, &r->release);
}

#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
/*
 * With debugfs render_keep armed, the context closed next is not dropped: the
 * firmware has had its CLEANUP, but its pages stay mapped in its VM, its slot
 * stays taken and its memory stays, parked until debugfs reset or the device's
 * destruction (sgxfive_render_keep_release). The switch is taken before the
 * CLEANUP (sgxfive_render_fini), because a CLEANUP the firmware does not
 * answer runs a recovery, which drops a switch still set.
 */
static void sgxfive_render_park(struct sgxfive_ctx *ctx, struct sgxfive_render *r)
{
	sgxfive_keep_park(&ctx->sgx->kept, &r->kept);
	drm_info(ctx->sgx->dev,
		 "sgxfive: render context %#x closed and kept: slot %u, TA data %#x\n",
		 r->a.ctx, r->slot, r->a.tadata);
}

static void sgxfive_render_keep_one(struct sgxfive_kept *kept, void *arg)
{
	sgxfive_render_drop(container_of(kept, struct sgxfive_render, kept));
}

/* Let the kept contexts go the usual way (debugfs reset, the device's destruction). */
void sgxfive_render_keep_release(struct sgxfive *sgx)
{
	unsigned int n = sgxfive_keep_drain(&sgx->kept, sgxfive_render_keep_one, NULL);

	if (n)
		drm_info(sgx->dev, "sgxfive: %u kept render context(s) released\n", n);
}
#else
static void sgxfive_render_park(struct sgxfive_ctx *ctx, struct sgxfive_render *r)
{
}
#endif

/*
 * The last reference to a render context is gone and it is off the device's
 * list: the firmware drops its state (CLEANUP; a blame-free reset if it does
 * not answer, sgxfive_cleanup_reset), the VM its mappings, and the memory goes
 * once no stale translation can reach it. While a recovery is held (debug
 * option) there is no such reset: the memory waits on the deferred list,
 * which the stopped firmware does not advance, for the reset that ends the
 * hold. The slot is free at once: a stale translation still lands on the
 * pinned pages. Frees ctx.
 */
void sgxfive_render_fini(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = ctx->render;
	struct sgxfive *sgx = ctx->sgx;
	bool keep = sgxfive_take_render_keep(sgx);	/* before a CLEANUP reset can drop it */

	if (sgxfive_render_cleanup(sgx, r->a.rtds, r->a.ctx, r->pb_shared ? 0 : r->a.pbdesc,
				   *rctx(ctx, SGXFIVE_CTX_HW_OFF)))
		sgxfive_cleanup_reset(sgx);
	if (keep)
		sgxfive_render_park(ctx, r);
	else
		sgxfive_render_drop(r);
}

#if IS_ENABLED(CONFIG_DRM_GMA500_SGXFIVE_DEBUG)
/*
 * Copy len bytes from off in the n pages to dst, for the debugfs dumps;
 * returns how many, fewer at the end of the pages and none past it. The
 * pages are lowmem: their direct map is the view the GPU's writes reach
 * (write-combined for a render context's).
 */
size_t sgxfive_pages_copy(struct page **pages, unsigned int n, void *dst, size_t len,
			  loff_t off)
{
	size_t done = 0;

	while (done < len && off >= 0 && off < (loff_t)n << PAGE_SHIFT) {
		size_t in = off & ~PAGE_MASK;
		size_t c = min_t(size_t, len - done, PAGE_SIZE - in);

		memcpy(dst + done, page_address(pages[off >> PAGE_SHIFT]) + in, c);
		done += c;
		off += c;
	}
	return done;
}

/* The newest render context still alive, with a reference (debugfs dumps). */
struct sgxfive_ctx *sgxfive_render_newest(struct sgxfive *sgx)
{
	struct sgxfive_ctx *ctx, *found = NULL;

	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry_reverse(ctx, &sgx->ctx_list, link) {
		if (ctx->type == DRM_SGXFIVE_CTX_TYPE_RENDER && kref_get_unless_zero(&ctx->ref)) {
			found = ctx;
			break;
		}
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return found;
}

/*
 * Copy from a render context's parameter slot, from its PB on (the PB pages,
 * their page table, the scratch primitive block, the background object), or
 * from its TA data buffers, as sgxfive_pages_copy(); none before its pages are.
 */
size_t sgxfive_render_dump(struct sgxfive_ctx *ctx, bool tadata, void *dst, size_t len,
			   loff_t off)
{
	struct sgxfive_render *r = READ_ONCE(ctx->render);

	if (!r)
		return 0;
	if (tadata)
		return sgxfive_pages_copy(r->td, r->td_pages, dst, len, off);
	return sgxfive_pages_copy(r->param, PARAM_PAGES, dst, len, off);
}

/*
 * The words of the context's structures the kernel and the firmware share,
 * as they are now: every word of the render spaces (render_spaces), one line
 * each, spelt as a kpoke that would set it.
 */
void sgxfive_render_words(struct sgxfive_ctx *ctx, struct drm_printer *p)
{
	static const u32 spaces[] = {
		SGXFIVE_KPOKE_CTX, SGXFIVE_KPOKE_RTDS, SGXFIVE_KPOKE_PB, SGXFIVE_KPOKE_KW,
		SGXFIVE_KPOKE_RD,
	};
	char line[SGXFIVE_KPOKE_LINE];
	u32 *w[SGXFIVE_KPOKE_SPACES];
	unsigned int i, j;

	render_spaces(ctx, w);
	for (i = 0; i < ARRAY_SIZE(spaces); i++) {
		for (j = 0; j < sgxfive_kpoke_words(spaces[i]); j++) {
			const struct sgxfive_kpoke k = {
				.space = spaces[i], .index = j,
				.value = READ_ONCE(w[spaces[i]][j]),
			};

			sgxfive_kpoke_format(&k, line, sizeof(line));
			drm_printf(p, "%s\n", line);
		}
	}
}

/* Before the kick: the armed fill of a page of the TA data (debugfs tadata_fill). */
static void sgxfive_render_tdfill(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = ctx->render;
	struct sgxfive_tdfill f;

	if (!sgxfive_tdfill_take(ctx->sgx, &f))
		return;
	if (!sgxfive_tdfill_place(&f, r->tdva, r->td_pages)) {
		drm_warn(ctx->sgx->dev,
			 "sgxfive: tadata_fill page %u dropped: the context has %u\n",
			 f.page, r->td_pages);
		return;
	}
	wmb();	/* the write-combined page, before the kick */
	drm_info(ctx->sgx->dev, "sgxfive: tadata_fill page %u %s 0x%08x applied\n", f.page,
		 f.ramp ? "ramp from" : "with", f.value);
}

/* Before the kick: the armed pokes of the context's structures. */
static void sgxfive_render_poke(struct sgxfive_ctx *ctx)
{
	u32 *w[SGXFIVE_KPOKE_SPACES];

	sgxfive_render_tdfill(ctx);
	render_spaces(ctx, w);
	w[SGXFIVE_KPOKE_RD] = NULL;	/* poked in the job's image instead */
	sgxfive_kpoke_run(ctx->sgx, w);
}

/*
 * The newest render context of ctx's VM, the one a sharing context takes its
 * PB from, with a reference: the sharer keeps it until its own memory is
 * freed, so that a donor closed first keeps its context page and PB. A
 * context whose last reference is gone (its release has begun) is skipped.
 * NULL when there is none.
 */
struct sgxfive_ctx *sgxfive_render_donor(struct sgxfive_ctx *ctx)
{
	struct sgxfive *sgx = ctx->sgx;
	struct sgxfive_ctx *d, *found = NULL;

	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry_reverse(d, &sgx->ctx_list, link) {
		if (d == ctx || d->type != DRM_SGXFIVE_CTX_TYPE_RENDER || d->vm != ctx->vm ||
		    !READ_ONCE(d->render))
			continue;
		if (kref_get_unless_zero(&d->ref)) {
			found = d;
			break;
		}
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return found;
}

/*
 * With debugfs render_share_pb armed, the next render context runs on the PB of
 * the newest render context of its VM, whose directory maps that context's page (the
 * PB descriptor) and parameter slot: the HW context and the TA commands name the
 * donor's descriptor. The context keeps its own, unused, and a reference on the
 * donor until it is freed (sgxfive_render_free), whichever is closed first; it
 * sends no CLEANUP for the PB, the donor does when it goes. Returns whether it
 * shares.
 */
static bool sgxfive_render_share_pb(struct sgxfive_ctx *ctx, struct sgxfive_render *r)
{
	struct sgxfive *sgx = ctx->sgx;
	struct sgxfive_ctx *d;

	if (!sgxfive_take_share_pb(sgx))
		return false;
	d = sgxfive_render_donor(ctx);
	if (!d || !sgx_render_share_pb(&r->a, &d->render->a)) {
		if (d)
			sgxfive_ctx_put(d);
		drm_warn(sgx->dev,
			 "sgxfive: render_share_pb: no render context in this VM to share with\n");
		return false;
	}
	r->donor = d;
	drm_info(sgx->dev, "sgxfive: render context shares the PB descriptor at %#x\n",
		 r->a.pbdesc);
	return true;
}
#else
static void sgxfive_render_poke(struct sgxfive_ctx *ctx)
{
}

static bool sgxfive_render_share_pb(struct sgxfive_ctx *ctx, struct sgxfive_render *r)
{
	return false;
}
#endif

/*
 * At submission, in the context's job order: which kick of its scene the job
 * is. A geometry job leaves the scene open for the next; the first job after a
 * closed scene opens one.
 */
void sgxfive_render_job_scene(struct sgxfive_ctx *ctx, struct sgxfive_job *job, bool geometry)
{
	job->first = !ctx->scene_open;
	job->last = !geometry;
	ctx->scene_open = geometry;
}

/* At submission: the job's firmware image, into its hardware fence. */
void sgxfive_render_job_build(struct sgxfive_ctx *ctx, struct sgxfive_job *job)
{
	job->size = sgxfive_render_build(job->hw->cmd, &ctx->render->a, &job->rc,
					 job->seqno, job->first, job->last);
	job->hw->ta_only = !job->last;
}

/*
 * Under the device lock, with the context's previous render finished, the
 * words a render job's run writes before its kick, in order: its render
 * details (from its image cmd) and its background object (at bg) in place,
 * the RT data's size words for its target (its TE words were checked against
 * the same layout), its out-of-memory counters cleared (sgx_render_rtds_job;
 * neither with the debug option's rtds_keep), the kick in the RT data set's
 * pending count (the EDM counts every kick of a scene as it finishes, the
 * geometry kicks at the TA's end and the last at its render's, and cleans the
 * set up only when the two counts agree), its TA command at the TA CCB's
 * write offset, the armed pokes of its structures (debug option), so they
 * land on the kernel's words, and last the CCB's host write offset.
 */
void sgxfive_render_write(struct sgxfive_ctx *ctx, u32 *bg,
			  const struct drm_sgxfive_render_cmd *rc, const u8 *cmd, u32 size)
{
	u32 *ctl = rctx(ctx, SGXFIVE_CTX_CTL_OFF);

	memcpy(rctx(ctx, SGXFIVE_RCTX_RD_OFF), cmd + SGXFIVE_RENDER_IMG_RD,
	       SGX_RENDER_RD_SIZE);
	sgx_render_bgobj(bg, rc);
	*rctx(ctx, SGXFIVE_RCTX_BGPRI_OFF) = rc->bgobj_words[2];
	if (!sgxfive_rtds_keep(ctx->sgx)) {
		sgx_render_rtds_resize(rctx(ctx, SGXFIVE_RCTX_RTDS_OFF), rc->mte_screen);
		sgx_render_rtds_job(rctx(ctx, SGXFIVE_RCTX_RTDS_OFF));
	}
	*rctx(ctx, SGXFIVE_RCTX_PENDING_OFF) += sgx_render_executed(cmd);
	memcpy(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_CCB_OFF + ctx->woff), cmd,
	       SGX_RENDER_CMD_STRIDE);
	ctx->woff = (ctx->woff + size) & (SGX_RENDER_CCB_SIZE - 1);
	sgxfive_render_poke(ctx);
	WRITE_ONCE(ctl[0], ctx->woff);		/* host write offset */
}

/* Under the device lock, with the context's previous render finished: run the job. */
int sgxfive_render_run(struct sgxfive_ctx *ctx, struct sgxfive_job *job,
		       const struct sgxfive_fence *hw)
{
	struct sgxfive_render *r = ctx->render;

	sgxfive_render_write(ctx, render_bg(r), &job->rc, hw->cmd, job->size);
	return sgxfive_kick_render(ctx->sgx, r->a.ctx);
}

/*
 * The error of the context's render job the firmware just finished: the
 * only one in the firmware, whose RT data set no later job has touched yet.
 */
int sgxfive_render_job_error(struct sgxfive_ctx *ctx)
{
	return sgx_render_rtds_error(rctx(ctx, SGXFIVE_RCTX_RTDS_OFF));
}

/*
 * What the context's unfinished jobs add to its RT data set's executed count
 * when they run again, kick by kick (sgx_render_executed): a recovery reset's
 * pending count for the set it starts over.
 */
u32 sgxfive_render_replay_pending(struct sgxfive_ctx *ctx)
{
	struct sgxfive_fence *f;
	unsigned long flags;
	u32 n = 0;

	spin_lock_irqsave(&ctx->fence_lock, flags);
	list_for_each_entry(f, &ctx->pending, link)
		n += sgx_render_executed(f->cmd);
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	return n;
}

/*
 * With the firmware stopped (recovery reset), under the context list's lock:
 * the structures start over, the RT data set's pending count is what the jobs
 * run again add (sgxfive_render_replay_pending), and the unfinished job, if
 * any, is written back (its TA command into the TA CCB from offset 0, its
 * render details, background object and RT data size words from the copy in
 * its fence). Returns how many jobs to re-run.
 */
u32 sgxfive_render_reset(struct sgxfive_ctx *ctx)
{
	struct sgxfive_render *r = ctx->render;
	struct drm_sgxfive_render_cmd c = { 0 };
	struct sgxfive_fence *f;
	unsigned long flags;
	u32 n, i;

	sgxfive_render_init_mem(r);
	n = sgxfive_ctx_rewind(ctx);
	*rctx(ctx, SGXFIVE_RCTX_PENDING_OFF) = sgxfive_render_replay_pending(ctx);
	spin_lock_irqsave(&ctx->fence_lock, flags);
	f = list_first_entry_or_null(&ctx->pending, struct sgxfive_fence, link);
	if (f) {
		memcpy(rctx(ctx, SGXFIVE_RCTX_RD_OFF), f->cmd + SGXFIVE_RENDER_IMG_RD,
		       SGX_RENDER_RD_SIZE);
		/* The background object's inputs, as the command carries them. */
		for (i = 0; i < 3; i++)
			memcpy(&c.bgobj_words[i], f->cmd + SGX_RENDER_CMD_SPEC + 4 * i, 4);
		memcpy(&c.mte_screen, f->cmd + SGX_RENDER_CMD_TAREGS +
		       SGX_RENDER_TA_MTESCREEN, 4);
		sgx_render_bgobj(render_bg(r), &c);
		*rctx(ctx, SGXFIVE_RCTX_BGPRI_OFF) = c.bgobj_words[2];
		sgx_render_rtds_resize(rctx(ctx, SGXFIVE_RCTX_RTDS_OFF), c.mte_screen);
	}
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	return n;
}
