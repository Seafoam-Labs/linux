// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- which type to take from, and the clean-file floor.
 *
 * Reclaim loses what it evicts: a refault, if the page is read again. How
 * much memory each type should have to lose least is measured, not
 * guessed: the controllers (tune/) keep each type's miss-ratio curve on a
 * sample and sets anon's share of memory at the split that costs least,
 * anon's misses weighed against file's by what their refaults cost --
 * swappiness in its first meaning, (MAX_SWAPPINESS - s) : s. Reclaim then
 * takes from the type over its share -- before the curves can compare,
 * the split memory had when reclaim first chose, so that each type pays
 * for its own. On the
 * last pass before OOM (priority 0) it takes from both by their sizes:
 * the type over its share may have nothing left to give.
 *
 * Three values of swappiness are promises, kept outside the measure:
 *
 *	0	never swap: file only, and at the floor nothing (then OOM)
 *	1	file first, down to the floor, and only then anon
 *	MAX	anon first, while swap lasts, and only then file
 *
 * Nothing else weighs in: a cache streamed once costs little to lose, anon
 * in use much, and the curves show it -- by measure, where cache_trim_mode,
 * file_is_tiny or a cost-weighted share of the scan each tried to by rule.
 *
 * One rule stands apart, as a promise rather than a measure: the floor.
 * No reclaim takes clean file while the node's clean file (file pages less
 * dirty ones) is at or below clean_min_ratio percent of its present pages
 * and the node still has anon to take instead; then file is not on offer,
 * and with anon unreclaimable nothing is: zone_reclaimable_pages() leaves
 * the floor out of its count, so the stock retry logic reaches the OOM
 * killer. The floor is the node's, and only reclaim forced by a cgroup's
 * limit (memory.max, memory.high, its own or an ancestor's) goes below it:
 * such a cgroup reclaims its own pages to stay within the limit whatever
 * the node's cache, and held to the node's floor it would OOM with
 * reclaimable file. All other reclaim holds it -- reclaim for the node, and
 * proactive reclaim (memory.reclaim) of any cgroup, which fails rather than
 * OOMs when there is nothing it may take. An OOM victim's own reclaim
 * takes file. There is no floor of a cgroup's own.
 *
 * mm/vmscan.c holds the floor the way le9uo does: prepare_scan_control()
 * sets sc->clean_below_min once per node pass, and get_scan_count() takes
 * file off the offer and zeroes its scan whatever decided it.
 */
#include <linux/oom.h>
#include <linux/swap.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include "policy/policy.h"

static unsigned int lru_cast_clean_min_ratio = 10;

/* reclaim calls that took anon, took file, took nothing, took both by size */
static atomic_long_t lru_cast_picks[4];

/* node passes the floor held for */
static atomic_long_t lru_cast_floor_held;

static bool lru_cast_has_anon(struct pglist_data *pgdat)
{
	return node_page_state(pgdat, NR_ACTIVE_ANON) +
	       node_page_state(pgdat, NR_INACTIVE_ANON) > 0;
}

/* the floor in pages, and the node's clean file */
static void lru_cast_floor(struct pglist_data *pgdat, unsigned long *clean,
			unsigned long *floor)
{
	unsigned long file = node_page_state(pgdat, NR_ACTIVE_FILE) +
			     node_page_state(pgdat, NR_INACTIVE_FILE);
	unsigned long dirty = node_page_state(pgdat, NR_FILE_DIRTY);

	*clean = file > dirty ? file - dirty : 0;
	*floor = pgdat->node_present_pages * READ_ONCE(lru_cast_clean_min_ratio) / 100;
}

unsigned long lru_cast_floor_file_pages(struct pglist_data *pgdat)
{
	unsigned long clean, floor;

	if (!READ_ONCE(lru_cast_clean_min_ratio) || !lru_cast_has_anon(pgdat))
		return 0;
	lru_cast_floor(pgdat, &clean, &floor);
	return min(clean, floor);
}

/* Strictly: clean file AT the floor is protected. */
bool lru_cast_floor_holds(struct pglist_data *pgdat)
{
	unsigned long clean, floor;

	if (!READ_ONCE(lru_cast_clean_min_ratio) || unlikely(tsk_is_oom_victim(current)))
		return false;
	lru_cast_floor(pgdat, &clean, &floor);
	if (clean > floor || !lru_cast_has_anon(pgdat))
		return false;
	atomic_long_inc(&lru_cast_floor_held);
	return true;
}

/*
 * The share first wanted before the curves can compare: held at the split
 * memory has now, each type keeping a tenth, as the curves leave a type
 * one until they have seen it read (tune/share.c)
 */
static noinline void lru_cast_hold_share(void)
{
	unsigned long anon = global_node_page_state(NR_INACTIVE_ANON) +
			     global_node_page_state(NR_ACTIVE_ANON);
	unsigned long file = global_node_page_state(NR_INACTIVE_FILE) +
			     global_node_page_state(NR_ACTIVE_FILE);

	lru_cast_param_hold_share(clamp(DIV_ROUND_CLOSEST(anon * 1000, max(anon + file, 1UL)),
				     100UL, 900UL));
}

int lru_cast_pick(struct lruvec *lruvec, int swappiness, int priority, bool anon_ok,
		   bool file_ok)
{
	unsigned long anon = lruvec_page_state(lruvec, NR_INACTIVE_ANON) +
			     lruvec_page_state(lruvec, NR_ACTIVE_ANON);
	unsigned long file = lruvec_page_state(lruvec, NR_INACTIVE_FILE) +
			     lruvec_page_state(lruvec, NR_ACTIVE_FILE);
	int pick;

	if (swappiness <= 0)
		anon_ok = false;		/* never swap: at the floor, nothing */
	anon_ok = anon_ok && anon;
	file_ok = file_ok && file;
	if (anon_ok && file_ok) {
		if (swappiness == 1)		/* file to the floor first */
			pick = LRU_CAST_FILE;
		else if (swappiness >= MAX_SWAPPINESS)	/* anon while swap lasts */
			pick = LRU_CAST_ANON;
		else if (!priority)	/* the last pass before OOM: both, by size */
			pick = LRU_CAST_EQUAL;
		else {	/* the type over its share gives */
			if (unlikely(!lru_cast_param_share_held()))
				lru_cast_hold_share();
			pick = (u64)anon * 1000 > (u64)lru_cast_param_share() * (anon + file) ?
			       LRU_CAST_ANON : LRU_CAST_FILE;
		}
	} else if (anon_ok) {
		pick = LRU_CAST_ANON;
	} else if (file_ok) {
		pick = LRU_CAST_FILE;
	} else {
		pick = LRU_CAST_NONE;
	}
	atomic_long_inc(&lru_cast_picks[pick == LRU_CAST_NONE ? 2 :
					 pick == LRU_CAST_EQUAL ? 3 : pick]);
	return pick;
}

unsigned int lru_cast_floor_ratio(void)
{
	return READ_ONCE(lru_cast_clean_min_ratio);
}

int lru_cast_set_floor_ratio(unsigned int percent)
{
	if (percent > 100)
		return -EINVAL;
	WRITE_ONCE(lru_cast_clean_min_ratio, percent);
	return 0;
}

int lru_cast_floor_held_show(char *buf)
{
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&lru_cast_floor_held));
}

int lru_cast_balance_show(char *buf)
{
	return sysfs_emit(buf, "anon %ld\nfile %ld\nnone %ld\nequal %ld\n",
			  atomic_long_read(&lru_cast_picks[0]),
			  atomic_long_read(&lru_cast_picks[1]),
			  atomic_long_read(&lru_cast_picks[2]),
			  atomic_long_read(&lru_cast_picks[3]));
}
