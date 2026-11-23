/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- what the walker and the look-around share, inside walk/:
 * whether a young bit is theirs to take, how a taken one is recorded, and
 * the bloom of PMDs reclaim found young (bloom.c).
 */
#ifndef _MM_LRU_CAST_WALK_YOUNG_H
#define _MM_LRU_CAST_WALK_YOUNG_H

#include "core/core.h"

/*
 * May @folio's young bit be taken -- cleared and recorded? Not when its
 * last read is stamped with the current tick: a state word tells reads
 * apart by tick, so recording another now adds nothing, and the bit is
 * left for reclaim's references check, which needs it. Clearing it would
 * cost the task its next access to the page, where the CPU sets the bit
 * again (some hundred ns a page), for nothing.
 */
static inline bool lru_cast_young_take(struct folio *folio)
{
	u32 w = lru_cast_read(folio);

	/*
	 * The young bit of the fault that brought the folio in, the mark
	 * still on it: taken whenever it is met. It tells nothing and is
	 * given away (lru_cast_young_record()), and until it is, the next one
	 * cannot be told from it. Left until the folio was a tick old, it was
	 * taken with memory full and reclaim at work, and the folio stood read
	 * once and not young until its task touched it again: a working set
	 * read every 100 ms lost a third of itself to a stream in one run of
	 * thirty, and more often the faster the folios came in.
	 */
	if (folio_test_referenced(folio))
		return true;
	/*
	 * Read once: whether it is in use at all is told as soon as it is
	 * seen, tick or no tick. A tick is a quarter of the node's memory
	 * taken in; in a memcg of an eighth of the node, or with no pages
	 * coming in, none passes, and a working set walked over four times
	 * stood read once beside the stream that came after it. How often a
	 * folio is read is still counted by the tick: by the walk it was
	 * counted by how often the walker walked, and a loop over more than
	 * memory, where the page longest unread is the next to be read, was
	 * a fifth slower for the order being kept so well.
	 */
	if ((w >> 24) == 1)
		return true;
	return !(w >> 24) ||
	       lru_cast_age(w, lru_cast_tick(folio_pgdat(folio), folio_is_file_lru(folio)));
}

/*
 * A young bit taken from @folio. The first after the folio entered the
 * cache may be the fault that brought it, counted already: it only clears
 * the mark (lru_cast_access_seen()). False if nothing was recorded.
 */
static inline bool lru_cast_young_record(struct folio *folio)
{
	if (folio_test_clear_referenced(folio))
		return false;
	lru_cast_access_young(folio);
	return true;
}

/*
 * The frame of the PTE @pte, if it is young, pulled toward L1 (@l1) or L3
 * (core/prefetch.h): it is one the loop will record a read of soon.
 */
static inline void lru_cast_young_ahead(pte_t *pte, bool l1)
{
	pte_t val = ptep_get(pte);

	if (pte_present(val) && pte_young(val))
		lru_cast_prefetch_frame(pte_pfn(val), l1);
}

/* bloom.c */
void lru_cast_bloom_set(struct mm_struct *mm, unsigned long addr);
bool lru_cast_bloom_test(struct mm_struct *mm, unsigned long addr);
void lru_cast_bloom_swap(void);

#endif /* _MM_LRU_CAST_WALK_YOUNG_H */
