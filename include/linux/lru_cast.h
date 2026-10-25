/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- what the rest of the kernel sees (mm/lru_cast/).
 */
#ifndef _LINUX_LRU_CAST_H
#define _LINUX_LRU_CAST_H

#include <linux/types.h>
#include <linux/mmzone.h>
#include <linux/jump_label.h>
#include <linux/ktime.h>
#include <linux/xarray.h>

struct folio;
struct page;

/* lru_cast_pick(): the type a reclaim call takes from */
#define LRU_CAST_ANON		0
#define LRU_CAST_FILE		1
#define LRU_CAST_NONE		(-1)
#define LRU_CAST_EQUAL		3	/* both, by size */

#ifdef CONFIG_LRU_CAST

DECLARE_STATIC_KEY_TRUE(lru_cast_enabled_key);

/* CAST is on somewhere: for the hooks with no lruvec at hand */
static inline bool lru_cast_enabled(void)
{
	return static_branch_likely(&lru_cast_enabled_key);
}

#ifdef CONFIG_SWAP
/*
 * kcompressd (mm/page_io.c): kswapd hands its anon swap-outs to a thread
 * per node. /sys/kernel/mm/lru_cast/kcompressd, -100..100: 0 off; the
 * magnitude the queue's depth, at which kswapd writes a folio itself;
 * above 0 only while CAST is on, below 0 also with CAST off.
 */
DECLARE_STATIC_KEY_TRUE(kcompressd_enabled_key);
DECLARE_STATIC_KEY_FALSE(kcompressd_force_key);
extern int vm_kcompressd;

static inline bool kcompressd_active(void)
{
	if (!static_branch_likely(&kcompressd_enabled_key))
		return false;
	if (static_branch_unlikely(&kcompressd_force_key))
		return true;
	return lru_cast_enabled();
}
#endif

/* CAST runs this lruvec (stable under its lru_lock) */
static inline bool lru_cast_on(struct lruvec *lruvec)
{
	return READ_ONCE(lruvec->lru_cast_on);
}

/* what mm/vmscan.c knows of a scan, and the counts handed back */
struct lru_cast_scan {
	int type;
	int reclaim_idx;
	bool may_unmap;
	unsigned long nr_to_scan;
	unsigned long nr_w, nr_m;	/* the lruvec's inactive and active pages of the type */
	unsigned long nr_scanned, nr_taken;
	unsigned long taken_w[MAX_NR_ZONES], taken_m[MAX_NR_ZONES];
	unsigned long skipped[MAX_NR_ZONES];
};

/* the node's clean file is at the floor: reclaim takes no file */
bool lru_cast_floor_holds(struct pglist_data *pgdat);
int lru_cast_pick(struct lruvec *lruvec, int swappiness, int priority, bool anon_ok,
		   bool file_ok);
unsigned long lru_cast_floor_file_pages(struct pglist_data *pgdat);

bool __lru_cast_add_folio(struct lruvec *lruvec, struct folio *folio, bool tail);
unsigned long lru_cast_isolate(struct lruvec *lruvec, struct lru_cast_scan *s,
				struct list_head *dst);
bool lru_cast_still_victim(struct folio *folio);
void lru_cast_access(struct folio *folio);
void lru_cast_access_young(struct folio *folio);
void lru_cast_access_kept(struct folio *folio);
bool lru_cast_protfault(struct folio *folio);
void lru_cast_swapin(struct folio *folio);
void __lru_cast_cost(int type, u64 t0);
bool lru_cast_refaulted(struct folio *folio);

/* the start of a wait for a page to come in: 0 when CAST is off */
static inline u64 lru_cast_cost_start(void)
{
	return lru_cast_enabled() ? ktime_get_ns() : 0;
}

/* a wait since @t0 for a page of @type to come back in, if @refault */
static inline void lru_cast_cost(int type, u64 t0, bool refault)
{
	if (t0 && refault)
		__lru_cast_cost(type, t0);
}
void lru_cast_access_seen(struct folio *folio, bool referenced);
void lru_cast_enter(struct folio *folio);
void *lru_cast_eviction(struct folio *folio);
bool lru_cast_test_recent(void *shadow, bool file, bool *workingset);
void lru_cast_refault(struct folio *folio, void *shadow);
void __lru_cast_reset(struct page *page, unsigned int order);
void __lru_cast_copy(struct folio *dst, struct folio *src);
void __lru_cast_migrate(struct folio *dst, struct folio *src);
void lru_cast_evicted(struct pglist_data *pgdat, int type, unsigned long nr);
void lru_cast_init_lruvec(struct lruvec *lruvec);
void lru_cast_exit_lruvec(struct lruvec *lruvec);
void lru_cast_reparent(struct lruvec *child, struct lruvec *parent);
bool lru_cast_folio_on(struct folio *folio);

/* a page freed: its reads are no one's; off, CAST lets the words go stale */
static inline void lru_cast_reset(struct page *page, unsigned int order)
{
	if (lru_cast_enabled())
		__lru_cast_reset(page, order);
}

/* migration, and the folios a split leaves: they carry the history */
static inline void lru_cast_copy(struct folio *dst, struct folio *src)
{
	if (lru_cast_enabled())
		__lru_cast_copy(dst, src);
}

/* migration: the history goes with the folio to its new place */
static inline void lru_cast_migrate(struct folio *dst, struct folio *src)
{
	if (lru_cast_enabled())
		__lru_cast_migrate(dst, src);
}

static inline bool lru_cast_add_folio(struct lruvec *lruvec, struct folio *folio, bool tail)
{
	if (!lru_cast_on(lruvec))
		return false;
	return __lru_cast_add_folio(lruvec, folio, tail);
}

struct page_vma_mapped_walk;
/* folio_referenced_one(): the folio's young bit, and its neighbours' (walk/lookaround.c) */
bool lru_cast_look_around(struct page_vma_mapped_walk *pvmw);

/*
 * Bit 0 of a shadow entry says whose it is: set, CAST's (ghost.c); clear,
 * legacy's (mm/workingset.c), whose fields sit above it. Across a switch
 * each side takes the other's as no history.
 */
#define LRU_CAST_SHADOW_KIND	1

static inline bool lru_cast_shadow(void *shadow)
{
	return xa_to_value(shadow) & 1;
}

#else /* !CONFIG_LRU_CAST */

#define LRU_CAST_SHADOW_KIND	0

static inline bool lru_cast_shadow(void *shadow)
{
	return false;
}

struct page_vma_mapped_walk;
static inline bool lru_cast_look_around(struct page_vma_mapped_walk *pvmw)
{
	return false;
}

static inline bool lru_cast_enabled(void)
{
	return false;
}

static inline bool lru_cast_on(struct lruvec *lruvec)
{
	return false;
}

static inline bool lru_cast_add_folio(struct lruvec *lruvec, struct folio *folio, bool tail)
{
	return false;
}

static inline bool lru_cast_still_victim(struct folio *folio)
{
	return false;
}

static inline unsigned long lru_cast_floor_file_pages(struct pglist_data *pgdat)
{
	return 0;
}

static inline bool lru_cast_floor_holds(struct pglist_data *pgdat)
{
	return false;
}

static inline int lru_cast_pick(struct lruvec *lruvec, int swappiness, int priority,
				 bool anon_ok, bool file_ok)
{
	return -1;
}

static inline void lru_cast_access(struct folio *folio)
{
}

static inline void lru_cast_access_young(struct folio *folio)
{
}

static inline void lru_cast_access_kept(struct folio *folio)
{
}

static inline bool lru_cast_protfault(struct folio *folio)
{
	return false;
}

static inline void lru_cast_swapin(struct folio *folio)
{
}


static inline u64 lru_cast_cost_start(void)
{
	return 0;
}

static inline void lru_cast_cost(int type, u64 t0, bool refault)
{
}

static inline bool lru_cast_refaulted(struct folio *folio)
{
	return false;
}

static inline void lru_cast_access_seen(struct folio *folio, bool referenced)
{
}

static inline void lru_cast_enter(struct folio *folio)
{
}

static inline void *lru_cast_eviction(struct folio *folio)
{
	return NULL;
}

static inline bool lru_cast_test_recent(void *shadow, bool file, bool *workingset)
{
	return false;
}

static inline void lru_cast_refault(struct folio *folio, void *shadow)
{
}

static inline void lru_cast_reset(struct page *page, unsigned int order)
{
}

static inline void lru_cast_copy(struct folio *dst, struct folio *src)
{
}

static inline void lru_cast_migrate(struct folio *dst, struct folio *src)
{
}

static inline void lru_cast_init_lruvec(struct lruvec *lruvec)
{
}

static inline void lru_cast_exit_lruvec(struct lruvec *lruvec)
{
}

static inline void lru_cast_reparent(struct lruvec *child, struct lruvec *parent)
{
}

static inline bool lru_cast_folio_on(struct folio *folio)
{
	return false;
}

#endif /* CONFIG_LRU_CAST */

#endif /* _LINUX_LRU_CAST_H */
