// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- what a refault costs, per type: how long a task waited,
 * on average, for a page reclaim had evicted to come back in. Only
 * refaults count: the misses a split of memory changes are the pages it
 * evicted and wanted again -- a page read once costs the same under every
 * split and decides nothing. Only refaults a task waited for count, too: one read ahead in
 * time is a hit, and how many are depends on what reclaim evicted -- the
 * more of a type it takes, the more of its refaults come back in runs and
 * are read ahead, the cheaper the type would look and the more it would
 * take: a loop that feeds itself. The wait for a page not yet in is the
 * device's and the decompressor's, whatever the split. The waits are taken
 * where tasks wait, each a sample: anon's major faults, a swap-in always a
 * refault; and file's faults and read()s that wait for a page not yet in
 * (not there, or still being read), when it came back with its history
 * (policy/ghost.c).
 *
 * The estimate is the samples' sum over their number with one sample of
 * prior, the ratio swappiness gives:
 *
 *	cost = (prior + sum of waits) / (1 + waits)
 *
 * at every moment, nothing switching as samples come: with none it is the
 * prior, with many what is measured. Sum and number forget together by the
 * number of waits: once there are as many as the pages on the LRU -- a
 * cache's worth, the controllers' unit of time -- both are halved. A busy
 * system averages over its last turnover or two, an idle one forgets
 * nothing. The prior's scale is the two types' samples taken together
 * (1 ns with none), its ratio anon : file = (MAX_SWAPPINESS - s) : s.
 *
 * Waits and their number are counted per CPU, where they happen, and
 * gathered at each evaluation.
 */
#include <linux/swap.h>
#include "tune/tune.h"
#include "tune/cost.h"
#include "policy/policy.h"

static DEFINE_PER_CPU(u64, lru_cast_tune_wait[ANON_AND_FILE]);	/* ns */
static DEFINE_PER_CPU(u64, lru_cast_tune_refaults[ANON_AND_FILE]);

static DEFINE_SPINLOCK(lru_cast_tune_cost_lock);
/* forgetting */
static u64 lru_cast_tune_sum[ANON_AND_FILE], lru_cast_tune_n[ANON_AND_FILE];
/* gathered so far */
static u64 lru_cast_tune_last_wait[ANON_AND_FILE], lru_cast_tune_last_n[ANON_AND_FILE];

void __lru_cast_cost(int type, u64 t0)
{
	this_cpu_add(lru_cast_tune_wait[type], ktime_get_ns() - t0);
	this_cpu_inc(lru_cast_tune_refaults[type]);
}

/*
 * A page being read in is back from an eviction if it came back with its
 * history (policy/ghost.c): one new to the cache has read once, its entry.
 */
bool lru_cast_refaulted(struct folio *folio)
{
	return lru_cast_enabled() && (lru_cast_read(folio) >> 24) >= 2;
}


/* the CPUs' counts into the estimate, forgetting by the cache's worth */
void lru_cast_tune_cost_gather(void)
{
	u64 window = max(lru_cast_tune_type_pages(0) + lru_cast_tune_type_pages(1), 1024UL);
	unsigned long flags;
	int type, cpu;

	if (!spin_trylock_irqsave(&lru_cast_tune_cost_lock, flags))
		return;
	for (type = 0; type < ANON_AND_FILE; type++) {
		u64 w = 0, n = 0;

		for_each_possible_cpu(cpu) {
			w += per_cpu(lru_cast_tune_wait[type], cpu);
			n += per_cpu(lru_cast_tune_refaults[type], cpu);
		}
		lru_cast_tune_sum[type] += w - lru_cast_tune_last_wait[type];
		lru_cast_tune_n[type] += n - lru_cast_tune_last_n[type];
		lru_cast_tune_last_wait[type] = w;
		lru_cast_tune_last_n[type] = n;
		while (lru_cast_tune_n[type] >= window) {
			lru_cast_tune_sum[type] -= lru_cast_tune_sum[type] >> 1;
			lru_cast_tune_n[type] -= lru_cast_tune_n[type] >> 1;
		}
	}
	spin_unlock_irqrestore(&lru_cast_tune_cost_lock, flags);
}

/* no samples: the estimate back at its prior, the CPUs' counts so far left out */
void lru_cast_tune_cost_reset(void)
{
	unsigned long flags;
	int type, cpu;

	spin_lock_irqsave(&lru_cast_tune_cost_lock, flags);
	for (type = 0; type < ANON_AND_FILE; type++) {
		u64 w = 0, n = 0;

		for_each_possible_cpu(cpu) {
			w += per_cpu(lru_cast_tune_wait[type], cpu);
			n += per_cpu(lru_cast_tune_refaults[type], cpu);
		}
		WRITE_ONCE(lru_cast_tune_sum[type], 0);
		WRITE_ONCE(lru_cast_tune_n[type], 0);
		lru_cast_tune_last_wait[type] = w;
		lru_cast_tune_last_n[type] = n;
	}
	spin_unlock_irqrestore(&lru_cast_tune_cost_lock, flags);
}

/* the estimated cost of each type's refault, in ns */
static void lru_cast_tune_costs(u64 *ca, u64 *cf)
{
	int sw = clamp(READ_ONCE(vm_swappiness), 1, MAX_SWAPPINESS - 1);
	u64 sa = READ_ONCE(lru_cast_tune_sum[0]), na = READ_ONCE(lru_cast_tune_n[0]);
	u64 sf = READ_ONCE(lru_cast_tune_sum[1]), nf = READ_ONCE(lru_cast_tune_n[1]);
	u64 m = na + nf ? div64_u64(sa + sf, na + nf) : 1;

	/* (prior + sum) / (1 + n) */
	*ca = div64_u64(div_u64(m * (MAX_SWAPPINESS - sw), sw) + sa, 1 + na);
	*cf = div64_u64(m + sf, 1 + nf);
	*ca = max(*ca, 1ULL);
	*cf = max(*cf, 1ULL);
}

/*
 * The estimated costs, no further apart than swappiness can set them:
 * 1 : (MAX_SWAPPINESS - 1) either way. The bound is on how extreme the
 * measure may be, and the measure weighs the two types alike.
 */
static void lru_cast_tune_costs_bounded(u64 *ca, u64 *cf)
{
	lru_cast_tune_costs(ca, cf);
	*ca = clamp(*ca, max(div_u64(*cf, MAX_SWAPPINESS - 1), 1ULL),
		    *cf * (MAX_SWAPPINESS - 1));
}

/* the weights of anon's and file's misses: their costs, in 16 bits */
void lru_cast_tune_weights(u64 *wa, u64 *wf)
{
	u64 ca, cf;
	int shift;

	lru_cast_tune_costs_bounded(&ca, &cf);
	shift = max(fls64(max(ca, cf)) - 16, 0);
	*wa = max(ca >> shift, 1ULL);
	*wf = max(cf >> shift, 1ULL);
}

/* the swappiness the estimate amounts to: 200 * file / (anon + file) */
static unsigned int lru_cast_estimated_swappiness(void)
{
	u64 ca, cf;

	lru_cast_tune_costs_bounded(&ca, &cf);
	return div64_u64(MAX_SWAPPINESS * cf, ca + cf);
}

/* anon's share, the swappiness the estimated costs amount to, and the costs */
int lru_cast_tune_estimate_show(char *buf)
{
	int len = sysfs_emit(buf, "share %u/1000\nswappiness %u\n",
			     lru_cast_param_share(), lru_cast_estimated_swappiness());
	u64 ca, cf;

	lru_cast_tune_costs(&ca, &cf);
	return len + sysfs_emit_at(buf, len,
				   "cost anon %llu ns (%llu waits) file %llu ns (%llu waits)\n",
				   ca, READ_ONCE(lru_cast_tune_n[0]),
				   cf, READ_ONCE(lru_cast_tune_n[1]));
}
