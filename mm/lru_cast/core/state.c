// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the per-folio state word.
 *
 * One 32-bit word per folio (its head page's, store.c): the clock's
 * tick of its last read, 8 bits, in the low byte; the gaps back to the two
 * reads before it, 3 bits each, as their logs (lru_cast_push()); and its read
 * count, 8 bits, saturating, in the high byte. A count of zero means the
 * folio is new to the cache. The word is reset when the page is freed and
 * copied when a folio is migrated or split.
 *
 * The ticks are the clock's (core/clock.c): pages taken in.
 *
 * In the same word, the owner tag: what the policy above says the folio
 * belongs to, 0 for nothing, so that a sweep of the array finds a set of
 * folios by their words alone (lru_cast_find_owner()). The core only keeps
 * it: cleared when the page is freed, carried to the folios a split
 * leaves, not to a migrated folio's new place, which is given one when it
 * is put back. Everything else here reads and writes the history around
 * it, and gives the word without it.
 */
#include "core/core.h"
#include "core/store.h"
#include "core/inl.h"

u32 lru_cast_read(struct folio *folio)
{
	u32 *p = lru_cast_word(folio_pfn(folio));

	return p ? READ_ONCE(*p) & ~LRU_CAST_OWNER_MASK : 0;
}

/* the word of the page frame @pfn, 0 if it has none */
u32 lru_cast_read_pfn(unsigned long pfn)
{
	u32 *p = lru_cast_word(pfn);

	return p ? READ_ONCE(*p) & ~LRU_CAST_OWNER_MASK : 0;
}

/* @folio's owner tag made @owner; the one it had */
u32 lru_cast_set_owner(struct folio *folio, u32 owner)
{
	u32 *p = lru_cast_word(folio_pfn(folio)), old;

	if (!p)
		return 0;
	old = READ_ONCE(*p);
	do {
		if ((old & LRU_CAST_OWNER_MASK) >> LRU_CAST_W_OWNER == owner)
			return owner;
	} while (!try_cmpxchg(p, &old, (old & ~LRU_CAST_OWNER_MASK) | owner << LRU_CAST_W_OWNER));
	return (old & LRU_CAST_OWNER_MASK) >> LRU_CAST_W_OWNER;
}

/* the out-of-line form of lru_cast_find_owner_inl() (core/inl.h) */
unsigned long lru_cast_find_owner(unsigned long pfn, unsigned long end, u32 owner, u32 *w)
{
	return lru_cast_find_owner_inl(pfn, end, owner, w);
}

/*
 * The first PFN in [@pfn, @end) whose owner tag is of a number in @lids
 * (lru_cast.lid; bit n for tags n << 1 and n << 1 | 1), its tag in
 * *@owner; @end if there is none.
 */
unsigned long lru_cast_find_owners(unsigned long pfn, unsigned long end,
				   const unsigned long *lids, u32 *owner)
{
	while (pfn < end) {
		unsigned long sec_end = min(end, section_nr_to_pfn(pfn_to_section_nr(pfn) + 1));
		u32 *p = lru_cast_word(pfn);

		if (!p) {
			pfn = sec_end;
			continue;
		}
		for (; pfn < sec_end; pfn++, p++) {
			u32 o = (READ_ONCE(*p) & LRU_CAST_OWNER_MASK) >> LRU_CAST_W_OWNER;

			if (o && test_bit(o >> 1, lids)) {
				*owner = o;
				return pfn;
			}
		}
	}
	return end;
}

/*
 * The words of [@pfn, @end) old enough at @tick marked stale
 * (lru_cast_mark()), lock-free: one read meanwhile is left as it is. How
 * many were marked. It may sleep, a section of words at a time: run by a
 * worker, it gives way where the kernel is not preemptible.
 */
unsigned long lru_cast_mark_stale(unsigned long pfn, unsigned long end, unsigned long tick)
{
	unsigned long marked = 0;

	while (pfn < end) {
		unsigned long sec_end = min(end, section_nr_to_pfn(pfn_to_section_nr(pfn) + 1));
		u32 *p = lru_cast_word(pfn);

		if (!p) {
			pfn = sec_end;
			continue;
		}
		for (; pfn < sec_end; pfn++, p++) {
			u32 v = READ_ONCE(*p), m = lru_cast_mark(v, tick);

			if (m && try_cmpxchg(p, &v, m))
				marked++;
		}
		cond_resched();
	}
	return marked;
}

/* @folio's history made @w; its tag kept */
void lru_cast_write(struct folio *folio, u32 w)
{
	u32 *p = lru_cast_word(folio_pfn(folio)), old;

	if (!p)
		return;
	old = READ_ONCE(*p);
	while (!try_cmpxchg(p, &old, (old & LRU_CAST_OWNER_MASK) | (w & ~LRU_CAST_OWNER_MASK)))
		;
}

/*
 * A read: push the tick, count it -- or, @kept, count it as the most there
 * are; lock-free. @p is @folio's word and @old what was read of it; @also,
 * flags of the folio set along with PG_rekey, in the one write.
 */
static __always_inline void lru_cast_record_word(struct folio *folio, u32 *p, u32 old,
						 enum lru_cast_read how, bool kept,
						 unsigned long also)
{
	u8 t = lru_cast_tick(folio_pgdat(folio), folio_is_file_lru(folio));
	unsigned long flags = BIT(PG_rekey) | also;
	u32 new;

	do {
		u32 n = kept ? 255 : min((old >> 24) + 1, 255u);

		new = lru_cast_push(old, t, n);
		/*
		 * Read as often as a word counts, and again in the same tick:
		 * the word would be written as it is. It is left alone, and
		 * its line with the CPUs reading it -- the text every process
		 * runs is read by them all, and written each time the line
		 * went from one to the next.
		 */
		if (new == old)
			break;
	} while (!try_cmpxchg(p, &old, new));
	/* after the word (the cmpxchg orders it): its key is to be taken again */
	if ((lru_cast_page_flags(&folio->page) & flags) != flags)
		set_mask_bits(lru_cast_page_flagsp(&folio->page), 0, flags);

	/* whoever watches reads sees this one (lru_cast_hook_read()) */
	lru_cast_hook_read(folio, how);
}

static void lru_cast_record(struct folio *folio, enum lru_cast_read how, bool kept)
{
	u32 *p = lru_cast_word(folio_pfn(folio));

	if (p)
		lru_cast_record_word(folio, p, READ_ONCE(*p), how, kept, 0);
}

void lru_cast_access(struct folio *folio)
{
	lru_cast_record(folio, LRU_CAST_READ, false);
}

/* a read known only by a young bit: the watchers may count it otherwise */
void lru_cast_access_young(struct folio *folio)
{
	lru_cast_record(folio, LRU_CAST_READ_YOUNG, false);
}

/*
 * Program text reclaim found in use (mm/vmscan.c), kept whatever its key:
 * its read counted as the most a word holds, so that its key is the top of
 * the ring now -- a read count of 255 is log2 255, about 8 half-lives,
 * above a read just now. With decay it comes down as any key does once the
 * text is no longer used; with none it stays above all read less.
 */
void lru_cast_access_kept(struct folio *folio)
{
	lru_cast_record(folio, LRU_CAST_READ_YOUNG, true);
}

/*
 * A folio entering the LRU from folio_add_lru(). New to the cache (no
 * reads recorded, nor any brought back from its shadow: ghost.c), the access that brought it in counts as its read, as a
 * miss does in the model. That access is seen once more --
 * folio_mark_accessed() after a read(), the young bit a fault leaves --
 * and counted again it would make a file streamed once look read twice
 * now, outranking all of M at the gate. PG_referenced, which legacy
 * reclaim keeps for the same first access and CAST otherwise leaves
 * alone, marks the entry's read as counted; the first sighting after it
 * clears the mark instead of counting (lru_cast_access_seen()).
 */
void lru_cast_enter(struct folio *folio)
{
	u32 *p, old;

	if (folio_test_unevictable(folio))
		return;
	/* the word read once: for what it says, and for the read pushed into it */
	p = lru_cast_word(folio_pfn(folio));
	old = p ? READ_ONCE(*p) : 0;
	if (old >> 24)
		return;
	lru_cast_installed(folio, folio_nr_pages(folio));
	if (p)
		lru_cast_record_word(folio, p, old, LRU_CAST_READ, false, BIT(PG_referenced));
	else
		folio_set_referenced(folio);
}

/* @folio, read once, is at W's tail for its write to swap having ended */
void lru_cast_mark_written(struct folio *folio)
{
	u32 w = lru_cast_read(folio);

	if ((w >> 24) == 1)
		lru_cast_write(folio, (w & ~LRU_CAST_W_GAPS) | LRU_CAST_WRITTEN << LRU_CAST_W_GAP);
}

bool lru_cast_marked_written(struct folio *folio)
{
	u32 w = lru_cast_read(folio);

	return (w >> 24) == 1 && (w & LRU_CAST_W_GAPS) >> LRU_CAST_W_GAP == LRU_CAST_WRITTEN;
}

/*
 * A sighting of a read -- folio_mark_accessed(), or a young bit found at
 * reclaim with @referenced the folio's PG_referenced as it was taken: the
 * first one after the folio entered the cache is the entry's own access,
 * counted already (see lru_cast_enter()).
 */
void lru_cast_access_seen(struct folio *folio, bool referenced)
{
	if (!referenced)
		lru_cast_access(folio);
}

/* a page freed: whatever comes next is new */
/*
 * A page freed, @order of them: their word cleared. A folio the cache had
 * taken in (its word not zero) leaves it, and the clock takes it back: what
 * leaves of itself makes no time (core/clock.c).
 */
void __lru_cast_reset(struct page *page, unsigned int order)
{
	u32 *p = lru_cast_word(page_to_pfn(page)), w;

	if (!p)
		return;
	/*
	 * Read since its key was last taken -- a folio of W always, never
	 * placed by one -- its word has those reads, and is not looked at to
	 * know: read here it was in no cache, the page having been in use
	 * since it was written, and every page freed waited for it. Written
	 * only, the write waits for the line and the CPU does not.
	 */
	if (test_bit(PG_rekey, lru_cast_page_flagsp(page))) {
		lru_cast_released(page_to_nid(page), 1UL << order);
		WRITE_ONCE(*p, 0);
		return;
	}
	w = READ_ONCE(*p);
	if (w & ~LRU_CAST_OWNER_MASK)
		lru_cast_released(page_to_nid(page), 1UL << order);
	if (w)
		WRITE_ONCE(*p, 0);
}

/* the folios a split leaves: they carry the history, and belong where it did */
void __lru_cast_copy(struct folio *dst, struct folio *src)
{
	u32 *s = lru_cast_word(folio_pfn(src)), *d = lru_cast_word(folio_pfn(dst));

	if (!d)
		return;
	WRITE_ONCE(*d, s ? READ_ONCE(*s) : 0);
}

/*
 * Migration: @src's history goes to @dst, which the clock counts taken in,
 * as @src, freed, is counted gone -- the folio moved, nothing came or went.
 */
void __lru_cast_migrate(struct folio *dst, struct folio *src)
{
	u32 w = lru_cast_read(src);

	lru_cast_write(dst, w);
	if (w)
		lru_cast_installed(dst, folio_nr_pages(dst));
}
