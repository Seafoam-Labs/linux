// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/psi.h>
#include <linux/uio.h>
#include <linux/sched/task.h>
#include <linux/delayacct.h>
#include <linux/zswap.h>
#include <linux/swap_ops.h>
#include <linux/kfifo.h>
#include <linux/backing-dev.h>
#include <linux/lru_cast.h>
#include "swap.h"
#include "swap_table.h"

int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent tree.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		cond_resched();

		first_block = probe_block;
		ret = bmap(inode, &first_block);
		if (ret || !first_block)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = probe_block + block_in_page;
			ret = bmap(inode, &block);
			if (ret || !block)
				goto bad_bmap;

			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
out:
	return ret;
bad_bmap:
	pr_err("swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

static bool is_folio_zero_filled(struct folio *folio)
{
	unsigned int pos, last_pos;
	unsigned long *data;
	unsigned int i;

	last_pos = PAGE_SIZE / sizeof(*data) - 1;
	for (i = 0; i < folio_nr_pages(folio); i++) {
		data = kmap_local_folio(folio, i * PAGE_SIZE);
		/*
		 * Check last word first, incase the page is zero-filled at
		 * the start and has non-zero data at the end, which is common
		 * in real-world workloads.
		 */
		if (data[last_pos]) {
			kunmap_local(data);
			return false;
		}
		for (pos = 0; pos < last_pos; pos++) {
			if (data[pos]) {
				kunmap_local(data);
				return false;
			}
		}
		kunmap_local(data);
	}

	return true;
}

static void swap_zeromap_folio_set(struct folio *folio)
{
	struct obj_cgroup *objcg = get_obj_cgroup_from_folio(folio);
	int nr_pages = folio_nr_pages(folio);
	struct swap_cluster_info *ci;
	swp_entry_t entry;
	unsigned int i;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);

	ci = swap_cluster_get_and_lock(folio);
	for (i = 0; i < folio_nr_pages(folio); i++) {
		entry = page_swap_entry(folio_page(folio, i));
		__swap_table_set_zero(ci, swp_cluster_offset(entry));
	}
	swap_cluster_unlock(ci);

	count_vm_events(SWPOUT_ZERO, nr_pages);
	if (objcg) {
		count_objcg_events(objcg, SWPOUT_ZERO, nr_pages);
		obj_cgroup_put(objcg);
	}
}

static void swap_zeromap_folio_clear(struct folio *folio)
{
	struct swap_cluster_info *ci;
	swp_entry_t entry;
	unsigned int i;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);

	ci = swap_cluster_get_and_lock(folio);
	for (i = 0; i < folio_nr_pages(folio); i++) {
		entry = page_swap_entry(folio_page(folio, i));
		__swap_table_clear_zero(ci, swp_cluster_offset(entry));
	}
	swap_cluster_unlock(ci);
}

/*
 * do_swapout() - Write a folio to swap space
 * @ctx: swap IO batching context (7.3: replaces the old swap_iocb **plug)
 * @folio: The folio to write out
 *
 * This function writes the folio to swap space, either using zswap or
 * the block/fs swap writeback path. It ensures that the folio is
 * unlocked and the reference count is decremented after the operation.
 */
static inline void do_swapout(struct swap_io_ctx *ctx, struct folio *folio)
{
	u64 t0 = lru_cast_evict_start();	/* CAST: zswap's compression */

	if (zswap_store(folio)) {
		lru_cast_evict_cpu(t0, folio_nr_pages(folio));
		count_mthp_stat(folio_order(folio), MTHP_STAT_ZSWPOUT);
		folio_unlock(folio);
	} else
		__swap_writepage(ctx, folio); /* Implies folio_unlock(folio) */

	/* Decrement the folio reference count */
	folio_put(folio);
}

#ifdef CONFIG_LRU_CAST
/*
 * do_swapout_batch() - Write a drained kcompressd batch out to swap.
 *
 * 7.3 rework: mm/page_io.c now owns a generic swap_iocb batching layer
 * (swap_add_folio() + ->can_merge()), so the hand-rolled multi-segment
 * bio builder this used to carry (swap_writepage_bdev_sync_batch() and
 * __end_swap_bio_write_batch(), both against the now-deleted
 * swap_writepage_bdev_sync()) is gone. Instead we share ONE swap_io_ctx
 * across the whole drained batch and set ctx.defer_sync_submit, which
 * suppresses swap_add_folio()'s force-submit-per-folio on
 * SWP_SYNCHRONOUS_IO devices (zram). Contiguous same-device folios then
 * coalesce into one multi-segment bio exactly as before, using upstream's
 * own (stricter, blkcg-aware) merge test. The trailing swap_write_submit()
 * drains the tail; for SWP_SYNCHRONOUS_IO that is submit_bio_wait(), so
 * writeback is still cleared synchronously before we return, matching the
 * previous behaviour.
 *
 * The force-submit is safe to defer here only because kcompressd is not
 * itself the reclaimer: it runs on its own kthread with a reference the
 * enqueue side took, so nothing is waiting on the folio's writeback bit
 * to make reclaim progress.
 */
static void do_swapout_batch(struct folio **folios, unsigned int n)
{
	struct swap_io_ctx ctx = { .defer_sync_submit = true };
	unsigned int i;

	for (i = 0; i < n; i++)
		do_swapout(&ctx, folios[i]);

	swap_write_submit(&ctx);
}

/*
 * kcompressd_store() - Off-load folio compression to kcompressd
 * @folio: The folio to compress
 * @handoff: pageout()'s: the folio's reclaim may go with it
 *
 * This function attempts to off-load the compression of the folio to
 * kcompressd. If kcompressd is not available or the folio cannot be
 * compressed, it falls back to synchronous write.
 *
 * Returns true if the folio was successfully queued for compression,
 * false otherwise.
 */
static bool kcompressd_store(struct folio *folio, bool handoff)
{
	pg_data_t *pgdat = NODE_DATA(numa_node_id());

	/*
	 * Only kswapd's pageout() hands a folio over, its reclaim with it;
	 * kcompressd finishing that reclaim writes what it finds dirty itself
	 */
	if (!handoff || !current_is_kswapd() || current == pgdat->kcompressd)
		return false;

	/* Mode 0, or mode 1 with CAST off — short-circuit on the static branches. */
	if (!kcompressd_active())
		return false;

	/* kthread must be running */
	if (unlikely(!pgdat->kcompressd))
		return false;

	/* We can only off-load anon folios */
	if (!folio_test_anon(folio))
		return false;

	/* Fall back to synchronously return AOP_WRITEPAGE_ACTIVATE.
	 * folio_memcg -> obj_cgroup_memcg requires RCU read-side held to
	 * keep objcg from being freed by a concurrent memcg teardown
	 * (lockdep_assert_once in obj_cgroup_memcg). */
	{
		bool zswap_wb_ok;

		rcu_read_lock();
		zswap_wb_ok = mem_cgroup_zswap_writeback_enabled(folio_memcg(folio));
		rcu_read_unlock();
		if (!zswap_wb_ok)
			return false;
	}

	/* Swap device must be sync-efficient */
	if (!zswap_is_enabled() &&
		!data_race(__swap_entry_to_info(folio->swap)->flags & SWP_SYNCHRONOUS_IO))
		return false;

	/*
	 * The kfifo backing storage is sized at KCOMPRESSD_FIFO_SIZE (the
	 * compile-time max). The effective queue depth is |vm_kcompressd|;
	 * at that depth kswapd writes the folio itself. kswapd is the only
	 * producer, kcompressd the only consumer.
	 */
	if (kfifo_len(&pgdat->kcompressd_fifo) >=
	    abs(READ_ONCE(vm_kcompressd)) * sizeof(struct folio *))
		return false;

	/* Increment the folio reference count to avoid it being freed */
	folio_get(folio);

	/* Enqueue the folio for compression */
	if (unlikely(!kfifo_in(&pgdat->kcompressd_fifo, &folio, sizeof(folio)))) {
		folio_put(folio);
		return false;
	}
	wake_up_interruptible(&pgdat->kcompressd_wait);
	return true;
}
#else  /* !CONFIG_LRU_CAST */
static inline bool kcompressd_store(struct folio *folio, bool handoff)
{
	return false;
}
#endif

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
static int __swap_writeout(struct swap_io_ctx *ctx, struct folio *folio, bool handoff)
{
	unsigned long nr;
	u64 t0;
	int ret = 0;

	if (folio_free_swap(folio))
		goto out_unlock;

	/*
	 * Arch code may have to preserve more data than just the page
	 * contents, e.g. memory tags.
	 */
	ret = arch_prepare_to_swap(folio);
	if (ret) {
		folio_mark_dirty(folio);
		goto out_unlock;
	}
	/* CAST: what writing it out costs, if done here, by the CPU */
	t0 = lru_cast_evict_start();
	nr = folio_nr_pages(folio);

	/*
	 * Use the swap table zero mark to avoid doing IO for zero-filled
	 * pages. The zero mark is protected by the cluster lock, which is
	 * acquired internally by swap_zeromap_folio_set/clear.
	 */
	if (is_folio_zero_filled(folio)) {
		swap_zeromap_folio_set(folio);
		lru_cast_evict_cpu(t0, nr);
		goto out_unlock;
	}

	/*
	 * Clear bits this folio occupies in the zeromap to prevent zero data
	 * being read in from any previous zero writes that occupied the same
	 * swap entries.
	 */
	swap_zeromap_folio_clear(folio);

	/*
	 * Compression within zswap and zram might block rmap, unmap
	 * of both file and anon pages, try to do compression async
	 * if possible
	 */
	if (kcompressd_store(folio, handoff))
		return SWAP_WRITEPAGE_HANDED;

	if (zswap_store(folio)) {
		count_mthp_stat(folio_order(folio), MTHP_STAT_ZSWPOUT);
		lru_cast_evict_cpu(t0, nr);
		goto out_unlock;
	}

	rcu_read_lock();
	if (!mem_cgroup_zswap_writeback_enabled(folio_memcg(folio))) {
		rcu_read_unlock();
		folio_mark_dirty(folio);
		return AOP_WRITEPAGE_ACTIVATE;
	}
	rcu_read_unlock();

	__swap_writepage(ctx, folio);
	return 0;
out_unlock:
	folio_unlock(folio);
	return ret;
}

int swap_writeout(struct swap_io_ctx *ctx, struct folio *folio)
{
	return __swap_writeout(ctx, folio, false);
}

int swap_writeout_handoff(struct swap_io_ctx *ctx, struct folio *folio)
{
	return __swap_writeout(ctx, folio, true);
}

#ifdef CONFIG_LRU_CAST
/*
 * Batch-drain size for kcompressd(): reuses the existing |vm_kcompressd|
 * queue-depth knob (no separate batch-size knob) clamped to a sane range
 * for the stack-allocated drain array.
 */
static inline unsigned int kcompressd_drain_depth(void)
{
	int d = abs(READ_ONCE(vm_kcompressd));

	if (d < 1)
		return 1;
	if (d > SWAP_CLUSTER_MAX)
		return SWAP_CLUSTER_MAX;
	return d;
}

/*
 * kcompressd() - Kernel thread for compressing folios
 * @p: Pointer to pg_data_t structure
 *
 * This function runs in a kernel thread and waits for folios to be
 * queued for compression. It drains its fifo in batches and dispatches
 * each batch through do_swapout_batch(), and then finishes their reclaim,
 * which kswapd handed over with them.
 */
int kcompressd(void *p)
{
	pg_data_t *pgdat = (pg_data_t *)p;
	struct folio *batch[SWAP_CLUSTER_MAX];
	LIST_HEAD(written);
	unsigned int i, n;

	/* * kcompressd runs with PF_MEMALLOC and PF_KSWAPD flags set to
	 * allow it to allocate memory for compression without being
	 * restricted by the current memory allocation context.
	 * Also PF_KSWAPD prevents Intel Graphics driver from crashing
	 * the system in i915_gem_shrinker.c:i915_gem_shrinker_scan()
	 */
	current->flags |= PF_MEMALLOC | PF_KSWAPD;

	/* stopped after kswapd: what it handed over is seen to first */
	for (;;) {
		wait_event_interruptible(pgdat->kcompressd_wait,
				!kfifo_is_empty(&pgdat->kcompressd_fifo) ||
				kthread_should_stop());

		for (;;) {
			n = kfifo_out_locked(&pgdat->kcompressd_fifo,
				batch,
				kcompressd_drain_depth() * sizeof(*batch),
				&pgdat->kcompressd_fifo_lock)
					/ sizeof(*batch);
			if (!n)
				break;
			do_swapout_batch(batch, n);
			/* kswapd's reclaim of them, finished: written, they go now */
			for (i = 0; i < n; i++)
				list_add_tail(&batch[i]->lru, &written);
			kcompressd_reclaim(&written, pgdat);
		}
		if (kthread_should_stop())
			break;
	}
	return 0;
}
#endif /* CONFIG_LRU_CAST */

#if defined(CONFIG_MEMCG) && defined(CONFIG_BLK_CGROUP)
static struct cgroup_subsys_state *folio_memcg_blkg_css(struct folio *folio)
{
	return cgroup_e_css(folio_memcg(folio)->css.cgroup, &io_cgrp_subsys);
}

static bool folio_blkg_can_merge(struct folio *folio, struct folio *prev_folio)
{
	bool can_merge = true;

	if (folio_memcg_charged(folio) != folio_memcg_charged(prev_folio))
		return false;
	if (folio_memcg_charged(folio)) {
		rcu_read_lock();
		if (folio_memcg_blkg_css(folio) !=
		    folio_memcg_blkg_css(prev_folio))
			can_merge = false;
		rcu_read_unlock();
	}
	return can_merge;
}

static void bio_associate_blkg_from_page(struct bio *bio, struct folio *folio)
{
	struct cgroup_subsys_state *css;

	if (!folio_memcg_charged(folio))
		return;
	rcu_read_lock();
	css = folio_memcg_blkg_css(folio);
	if (css && !css_tryget(css))
		css = NULL;
	rcu_read_unlock();

	bio_associate_blkg_from_css(bio, css);
	if (css)
		css_put(css);
}
#else
static bool folio_blkg_can_merge(struct folio *folio, struct folio *prev_folio)
{
	return true;
}
#define bio_associate_blkg_from_page(bio, folio)		do { } while (0)
#endif /* CONFIG_MEMCG && CONFIG_BLK_CGROUP */

static mempool_t *sio_pool;

int sio_pool_init(void)
{
	if (!sio_pool) {
		mempool_t *pool = mempool_create_kmalloc_pool(
			SWAP_CLUSTER_MAX, sizeof(struct swap_iocb));
		if (cmpxchg(&sio_pool, NULL, pool))
			mempool_destroy(pool);
	}
	if (!sio_pool)
		return -ENOMEM;
	return 0;
}

static bool swap_can_merge(struct swap_io_ctx *ctx, struct folio *folio,
		int rw)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	struct bio_vec *last_bv = &ctx->sio->bvecs[ctx->sio->nr_bvecs - 1];
	struct folio *prev_folio = bvec_folio(last_bv);
	size_t prev_folio_size = folio_size(prev_folio);

	if (ctx->sis != sis)
		return false;
	return sis->ops->can_merge(folio, prev_folio, prev_folio_size, rw);
}

#ifdef CONFIG_LRU_CAST
static inline bool swap_io_ctx_defer_sync(const struct swap_io_ctx *ctx)
{
	return ctx->defer_sync_submit;
}
#else
static inline bool swap_io_ctx_defer_sync(const struct swap_io_ctx *ctx)
{
	return false;
}
#endif

static void swap_add_folio(struct swap_io_ctx *ctx, struct folio *folio, int rw)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	struct swap_iocb *sio = ctx->sio;

	if (sio && !swap_can_merge(ctx, folio, rw)) {
		if (rw == WRITE)
			swap_write_submit(ctx);
		else
			swap_read_submit(ctx);
		sio = ctx->sio;
	}

	if (!sio) {
		ctx->sis = sis;
		ctx->sio = sio = mempool_alloc(sio_pool, GFP_NOIO);
		sio->nr_bvecs = 0;
		sio->len = 0;
	}
	bvec_set_folio(&sio->bvecs[sio->nr_bvecs], folio, folio_size(folio), 0);
	sio->len += folio_size(folio);

	/*
	 * Write out the iocb if we filled it, or if the device is synchronous.
	 *
	 * The latter is to work around expectations in the classic LRU code
	 * which make synchronous clearing of the folio writeback flag in the
	 * reclaim path beneficial.
	 */
	if (++sio->nr_bvecs == ARRAY_SIZE(sio->bvecs) ||
	    (rw == WRITE && (sis->flags & SWP_SYNCHRONOUS_IO) &&
	     !swap_io_ctx_defer_sync(ctx))) {
		if (rw == WRITE)
			swap_write_submit(ctx);
		else
			swap_read_submit(ctx);
	}
}

void __swap_writepage(struct swap_io_ctx *ctx, struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (unlikely(folio_test_pmd_mappable(folio))) {
		count_memcg_folio_events(folio, THP_SWPOUT, 1);
		count_vm_event(THP_SWPOUT);
	}
#endif
	count_mthp_stat(folio_order(folio), MTHP_STAT_SWPOUT);
	count_memcg_folio_events(folio, PSWPOUT, folio_nr_pages(folio));
	count_vm_events(PSWPOUT, folio_nr_pages(folio));

	folio_start_writeback(folio);
	folio_unlock(folio);
	swap_add_folio(ctx, folio, WRITE);
}

/*
 * Return the count of contiguous swap entries that share the same
 * zeromap status as the starting entry. If is_zerop is not NULL,
 * it will return the zeromap status of the starting entry.
 *
 * Context: Caller must ensure the cluster containing the entries
 * that will be checked won't be freed.
 */
static int swap_zeromap_batch(swp_entry_t entry, int max_nr,
			      bool *is_zerop)
{
	int i;
	bool is_zero;
	unsigned int ci_start = swp_cluster_offset(entry);
	struct swap_cluster_info *ci = __swap_entry_to_cluster(entry);

	VM_WARN_ON_ONCE(ci_start + max_nr > SWAPFILE_CLUSTER);

	rcu_read_lock();
	is_zero = __swap_table_test_zero(ci, ci_start);
	for (i = 1; i < max_nr; i++)
		if (is_zero != __swap_table_test_zero(ci, ci_start + i))
			break;
	rcu_read_unlock();
	if (is_zerop)
		*is_zerop = is_zero;

	return i;
}

static bool swap_read_folio_zeromap(struct folio *folio)
{
	int nr_pages = folio_nr_pages(folio);
	struct obj_cgroup *objcg;
	bool is_zeromap;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);

	/*
	 * Swapping in a large folio that is partially in the zeromap is not
	 * currently handled. Return true without marking the folio uptodate so
	 * that an IO error is emitted (e.g. do_swap_page() will sigbus).
	 * Folio lock stabilizes the cluster and map, so the check is safe.
	 */
	if (WARN_ON_ONCE(swap_zeromap_batch(folio->swap, nr_pages,
			 &is_zeromap) != nr_pages))
		return true;

	if (!is_zeromap)
		return false;

	objcg = get_obj_cgroup_from_folio(folio);
	count_vm_events(SWPIN_ZERO, nr_pages);
	if (objcg) {
		count_objcg_events(objcg, SWPIN_ZERO, nr_pages);
		obj_cgroup_put(objcg);
	}

	folio_zero_range(folio, 0, folio_size(folio));
	folio_mark_uptodate(folio);
	return true;
}

void swap_read_folio(struct swap_io_ctx *ctx, struct folio *folio)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	bool synchronous = sis->flags & SWP_SYNCHRONOUS_IO;
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags;
	bool in_thrashing;

	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio) && !synchronous, folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_uptodate(folio), folio);

	/*
	 * Count submission time as memory stall and delay. When the device
	 * is congested, or the submitting cgroup IO-throttled, submission
	 * can be a significant part of overall IO time.
	 */
	if (workingset) {
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
	}
	delayacct_swapin_start();

	/*
	 * CAST: the page a fault waits for, read now -- by this CPU, unless it
	 * is read from a device that queues it (lru_cast_wait_swap_queued())
	 */
	lru_cast_wait_swap_read(folio->swap.val, folio_nr_pages(folio));

	if (swap_read_folio_zeromap(folio)) {
		folio_unlock(folio);
		goto finish;
	}

	if (zswap_load(folio) != -ENOENT)
		goto finish;

	/* We have to read from slower devices. Increase zswap protection. */
	zswap_folio_swapin(folio);

	if (!synchronous || sis->ops != &swap_bdev_ops) {
		if (lru_cast_wait_swap_mine(folio->swap.val, folio_nr_pages(folio)))
			lru_cast_wait_swap_queued(inode_to_bdi(file_inode(sis->swap_file))->id);
	}
	swap_add_folio(ctx, folio, READ);

finish:
	if (workingset) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}
	delayacct_swapin_end();
}

static void swap_write_end(struct swap_iocb *sio, bool failed)
{
	int p;

	for (p = 0; p < sio->nr_bvecs; p++) {
		struct page *page = sio->bvecs[p].bv_page;

		if (failed) {
			set_page_dirty(page);
			ClearPageReclaim(page);
		}
		end_page_writeback(page);
	}
	mempool_free(sio, sio_pool);
}

static void swap_fs_write_complete(struct kiocb *iocb, long ret)
{
	struct swap_iocb *sio = container_of(iocb, struct swap_iocb, iocb);
	bool failed = ret != sio->len;

	if (failed) {
		struct page *page = sio->bvecs[0].bv_page;

		/*
		 * In the case of swap-over-nfs, this can be a temporary failure
		 * if the system has limited memory for allocating transmit
		 * buffers.  Mark the page dirty and avoid
		 * folio_rotate_reclaimable but rate-limit the messages.
		 */
		pr_err_ratelimited("Write error %ld on dio swapfile (%llu)\n",
				   ret, swap_dev_pos(page_swap_entry(page)));
	}

	swap_write_end(sio, failed);
}

static void end_swap_bio_write(struct bio *bio)
{
	struct swap_iocb *sio = container_of(bio, struct swap_iocb, bio);
	bool failed = !!bio->bi_status;

	if (failed)
		pr_alert_ratelimited("Write-error on swap-device (%u:%u:%llu)\n",
				     MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
				     (unsigned long long)bio->bi_iter.bi_sector);
	bio_uninit(bio);
	swap_write_end(sio, failed);
}

/* CAST: the device a swap bio queues on */
static inline u64 swap_bio_dev(struct bio *bio)
{
	return bio->bi_bdev->bd_disk->bdi->id;
}

/* CAST: a swap-out queued on its device, ended (swap_bdev_submit_write()) */
static void end_swap_bio_write_queued(struct bio *bio)
{
	struct swap_iocb *sio = container_of(bio, struct swap_iocb, bio);

	lru_cast_evict_io_end(swap_bio_dev(bio), sio->len >> PAGE_SHIFT);
	end_swap_bio_write(bio);
}

static void swap_read_end(struct swap_iocb *sio, bool failed)
{
	int p;

	for (p = 0; p < sio->nr_bvecs; p++) {
		struct folio *folio = bvec_folio(&sio->bvecs[p]);

		if (!failed) {
			count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN);
			count_memcg_folio_events(folio, PSWPIN,
					folio_nr_pages(folio));
			folio_mark_uptodate(folio);
		}
		folio_unlock(folio);
	}

	if (!failed)
		count_vm_events(PSWPIN, sio->len >> PAGE_SHIFT);

	mempool_free(sio, sio_pool);
}

static void swap_fs_read_complete(struct kiocb *iocb, long ret)
{
	struct swap_iocb *sio = container_of(iocb, struct swap_iocb, iocb);
	bool failed = ret != sio->len;

	if (failed)
		pr_alert_ratelimited("Read-error on swap-device\n");
	swap_read_end(sio, failed);
}

static void swap_bio_read_end_io(struct bio *bio)
{
	struct swap_iocb *sio = container_of(bio, struct swap_iocb, bio);
	bool failed = !!bio->bi_status;

	if (failed)
		pr_alert_ratelimited("Read-error on swap-device (%u:%u:%llu)\n",
				     MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
				     (unsigned long long)bio->bi_iter.bi_sector);
	bio_uninit(bio);
	swap_read_end(sio, failed);
}

static void swap_bdev_submit_write(struct swap_io_ctx *ctx)
{
	struct swap_iocb *sio = ctx->sio;
	struct bio *bio = &sio->bio;

	bio_init(bio, ctx->sis->bdev, sio->bvecs, ARRAY_SIZE(sio->bvecs),
			REQ_OP_WRITE | REQ_SWAP);
	bio->bi_iter.bi_size = sio->len;
	bio->bi_iter.bi_sector = swap_folio_sector(bio_first_folio_all(bio));
	bio_associate_blkg_from_page(bio, bio_first_folio_all(bio));

	if (ctx->sis->flags & SWP_SYNCHRONOUS_IO) {
		/* CAST: the device's work, done by this CPU */
		u64 t0 = lru_cast_evict_start();
		unsigned long nr = sio->len >> PAGE_SHIFT;

		submit_bio_wait(bio);
		end_swap_bio_write(bio);
		lru_cast_evict_cpu(t0, nr);
	} else {
		bio->bi_end_io = end_swap_bio_write_queued;
		/* CAST: the write queues on the device, until end_swap_bio_write_queued() */
		lru_cast_evict_io_begin(swap_bio_dev(bio));
		submit_bio(bio);
	}
}

static void swap_bdev_submit_read(struct swap_io_ctx *ctx)
{
	struct swap_iocb *sio = ctx->sio;
	struct bio *bio = &sio->bio;

	bio_init(bio, ctx->sis->bdev, sio->bvecs, ARRAY_SIZE(sio->bvecs),
			REQ_OP_READ);
	bio->bi_iter.bi_size = sio->len;
	bio->bi_iter.bi_sector = swap_folio_sector(bio_first_folio_all(bio));

	if (ctx->sis->flags & SWP_SYNCHRONOUS_IO) {
		/*
		 * Keep this task valid during swap readpage because the oom
		 * killer may attempt to access it in the page fault retry
		 * time check.
		 */
		get_task_struct(current);
		submit_bio_wait(bio);
		swap_bio_read_end_io(bio);
		put_task_struct(current);
	} else {
		bio->bi_end_io = swap_bio_read_end_io;
		submit_bio(bio);
	}
}

static bool swap_bdev_can_merge(struct folio *folio, struct folio *prev_folio,
		size_t prev_folio_size, int rw)
{
	if (swap_folio_sector(folio) !=
	    swap_folio_sector(prev_folio) + (prev_folio_size >> SECTOR_SHIFT))
		return false;
	if (rw == WRITE && !folio_blkg_can_merge(folio, prev_folio))
		return false;
	return true;
}

const struct swap_ops swap_bdev_ops = {
	.submit_write		= swap_bdev_submit_write,
	.submit_read		= swap_bdev_submit_read,
	.can_merge		= swap_bdev_can_merge,
};

void swap_fs_prepare_rw(struct swap_io_ctx *ctx, int rw, struct iov_iter *iter)
{
	struct swap_iocb *sio = ctx->sio;

	init_sync_kiocb(&sio->iocb, ctx->sis->swap_file);
	sio->iocb.ki_pos = swap_dev_pos(bvec_folio(&sio->bvecs[0])->swap);
	if (rw == WRITE)
		sio->iocb.ki_complete = swap_fs_write_complete;
	else
		sio->iocb.ki_complete = swap_fs_read_complete;

	iov_iter_bvec(iter, rw == WRITE ? ITER_SOURCE : ITER_DEST,
			sio->bvecs, sio->nr_bvecs, sio->len);
}
EXPORT_SYMBOL_GPL(swap_fs_prepare_rw);

bool swap_fs_can_merge(struct folio *folio, struct folio *prev_folio,
		size_t prev_folio_size, int rw)
{
	return swap_dev_pos(folio->swap) ==
		swap_dev_pos(prev_folio->swap) + prev_folio_size;
}
EXPORT_SYMBOL_GPL(swap_fs_can_merge);

int swap_fs_activate(struct swap_info_struct *sis, const struct swap_ops *ops)
{
	sis->ops = ops;
	return add_swap_extent(sis, 0, sis->max, 0);
}
EXPORT_SYMBOL_GPL(swap_fs_activate);

void swap_write_submit(struct swap_io_ctx *ctx)
{
	if (!ctx->sio)
		return;
	count_vm_events(NRSWPOUT, 1);
	ctx->sis->ops->submit_write(ctx);
	ctx->sio = NULL;
	ctx->sis = NULL;
}

void swap_read_submit(struct swap_io_ctx *ctx)
{
	if (!ctx->sio)
		return;
	count_vm_events(NRSWPIN, 1);
	ctx->sis->ops->submit_read(ctx);
	ctx->sio = NULL;
	ctx->sis = NULL;
}
