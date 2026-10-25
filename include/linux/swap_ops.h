/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_SWAP_OPS_H
#define _MM_SWAP_OPS_H

#include <linux/swap.h> /* for SWAP_CLUSTER_MAX */

struct swap_iocb {
	union {
		struct kiocb	iocb;
		struct bio	bio;
	};
	struct bio_vec		bvecs[SWAP_CLUSTER_MAX];
	int			nr_bvecs;
	int			len;
};

struct swap_io_ctx {
	struct swap_iocb	*sio;
	struct swap_info_struct	*sis;
#ifdef CONFIG_LRU_CAST
	/*
	 * Suppress swap_add_folio()'s force-submit-per-folio on
	 * SWP_SYNCHRONOUS_IO devices so a batch of folios coalesces into one
	 * multi-segment bio. Only mm/page_io.c's kcompressd drain sets this:
	 * it is not the reclaimer, so nothing is waiting on the folios'
	 * writeback bit for reclaim progress. Every other swap_io_ctx is
	 * zero-initialised ("= {}") and therefore keeps stock behaviour.
	 * CAST only; the field does not exist under CONFIG_LRU_CAST=n.
	 */
	bool			defer_sync_submit;
#endif
};

/*
 * SWAP_OPS_F_REQUIRE_NOFS:
 *	When set, all reclaim operations must operated as GFS_NOFS and not
 *	just GFP_NOIO, as GFP_NOIO allocations could recourse into the
 *	file system backing this swap file.
 */
#define SWAP_OPS_F_REQUIRE_NOFS		(1U << 0)

struct swap_ops {
	unsigned int		flags;

	bool (*can_merge)(struct folio *folio, struct folio *prev_folio,
			size_t prev_folio_size, int rw);
	void (*submit_write)(struct swap_io_ctx *ctx);
	void (*submit_read)(struct swap_io_ctx *ctx);
};

void swap_fs_prepare_rw(struct swap_io_ctx *ctx, int rw, struct iov_iter *iter);
bool swap_fs_can_merge(struct folio *folio, struct folio *prev_folio,
		size_t prev_folio_size, int rw);
int swap_fs_activate(struct swap_info_struct *sis, const struct swap_ops *ops);

#endif /* _MM_SWAP_OPS_H */
