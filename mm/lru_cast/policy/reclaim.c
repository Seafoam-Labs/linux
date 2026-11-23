// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the choice of victims.
 *
 * W holds the newcomers not yet weighed against M. While W is over its
 * share, its tail joins M (lru_cast_w_admit()), in front of reclaim's
 * candidates when its key is below theirs, so it is the first to go; and
 * what reclaim takes is M's lowest as a sweep finds it (policy/ring.c).
 * That is the model's gate -- W's tail meets M's lowest, the lower goes.
 * W is kept to its share as folios come in (policy/ring.c), not only once
 * reclaim runs: left to grow until then, W held all that came in since
 * boot, in the order it came, and reclaim admitted its oldest first -- the
 * folios read most since, a database warmed before a scan -- to take M's
 * lowest from among only those, while the scan waited in W unweighed.
 *
 * A folio leaving W for M has left what legacy reclaim calls the inactive
 * list, as an activated one has there, and ages the nonresident clock the
 * same way: refault distances are measured on it, and with it running on
 * evictions alone a refault would look twice as recent as it is.
 *
 * As folios come in, W may go over its share by LRU_CAST_W_SLACK pages before
 * its excess is admitted, all of it at once: the fixed part of an
 * admission, under the lock every fault adding to the LRU takes, is paid
 * once a slack's worth and not for every folio, and W stays within a few
 * dozen pages of where the model keeps it. Reclaim admits without slack,
 * LRU_CAST_ADMIT times the pages it asks for at most, so the lock is not held
 * over the whole of a large W; what is left waits for the next call, still
 * the oldest of W. A folio taken from
 * M loses PG_active, so what reclaim hands back without a verdict joins
 * W's head.
 *
 * Runs under the lruvec's lru_lock; the caller (mm/vmscan.c) passes in
 * what it knows of the scan and does the size accounting from the counts
 * this hands back.
 */
#include "policy/policy.h"

/*
 * @folio, at W's tail, was written to swap and its write has ended: it was
 * put there for reclaim to free (folio_rotate_reclaimable(), or reclaim's
 * own putback), and marked as it was (policy/ring.c).
 *
 * W's tail is where a folio goes on into M; a written one went with the
 * rest, to be found again by its key, and reclaim wrote another in its
 * place meanwhile: twice the pages written for those freed. Taken from W's
 * tail before anything of M, and not admitted into M, they are freed as
 * they are written.
 *
 * The mark is what tells it from a folio read ahead from swap and never
 * mapped, which is in the swap cache, unmapped and clean as well, and comes
 * to W's tail with memory to spare and no reclaim to take it: W's admission
 * stopped at those, W's cold folios were left behind them, and reclaim took
 * what a sweep of M found in their place -- a working set of 300 MiB under a
 * stream lost twenty thousand pages or more in 6 runs of 54. Read again or
 * mapped again or dirtied, a folio is no longer written and to be freed,
 * and is weighed as any other.
 */
static bool lru_cast_written(struct folio *folio)
{
	return folio_test_swapbacked(folio) && folio_test_swapcache(folio) &&
	       !folio_mapped(folio) && !folio_test_dirty(folio) &&
	       !folio_test_writeback(folio) && !folio_test_locked(folio) &&
	       lru_cast_marked_written(folio);
}

/*
 * W's mapped newcomers wait before they are weighed.
 *
 * A mapped page is read through its page tables, and all that is known of
 * that is what the walker finds. Its first walk gives the fault's own young
 * bit away (walk/young.h); only the walks after it tell whether the page was
 * used again. Weighed before then, the heap of a running process and a
 * stream never to be touched again weigh the same, and reclaim took both
 * alike: of a kernel build on tmpfs in 2 GiB it wrote a million anonymous
 * pages to swap, and 300 thousand of them were taken back from the swap
 * cache by their tasks before they were freed, where the multi-gen LRU wrote
 * three thousand. An unmapped page waits for nothing: every read of it is
 * recorded as it is made, and one read once has not been read since.
 *
 * They wait on lists by walk. A mapped newcomer read once is put on the
 * list of the walks ended as it came in (__lru_cast_add_folio()). When
 * LRU_CAST_LOOKS walks have ended since, all that is on that list has been
 * walked over as often, and the list as a whole joins the inactive list --
 * W's weighed part -- as its newest. Nothing on the inactive list waits, and
 * nothing that waits is in the way of what does not; no folio carries when
 * it came, where it is tells. (They waited at W's tail once, each with the
 * walk it came in during in its state word, and whatever came in behind one
 * that waited could neither join M nor be met with M's lowest.)
 *
 * The lists are turned here, wherever W is looked at; under the lru_lock.
 */
void lru_cast_wait_turn(struct lruvec *lruvec, int type)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	struct list_head *w = &lruvec->lists[type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON];
	long now = lru_cast_walks(), was = k->wait_epoch, e;

	if (now == was)
		return;
	/* the lists hold the walks was - LOOKS + 1 .. was; those LOOKS walks old go, oldest first */
	for (e = was - LRU_CAST_LOOKS + 1; e <= was && e <= now - LRU_CAST_LOOKS; e++) {
		if (e < 0)
			continue;
		list_splice_init(&k->wait[e % LRU_CAST_WAITS], w);
		k->wait_nr[e % LRU_CAST_WAITS] = 0;
	}
	k->wait_epoch = now;
	/* a walk has been made: what comes in to wait from now asks for the next */
	k->wait_in = 0;
	lru_cast_wait_goal(lruvec, type);
}

/*
 * What comes in to wait asks for a walk once there is more of it since the
 * last than W has weighed (__lru_cast_add_folio()): W less what waits,
 * counted as it was put on the lists -- a folio freed or weighed since is
 * counted still, and the walk comes the sooner for it.
 *
 * Taken anew as they come in, every LRU_CAST_WAIT_LOOK pages of them, not
 * only when a walk has been made: what W had weighed at the last walk may be
 * long gone. A memcg whose stream was killed kept the measure of that stream,
 * and the task that came next, smaller than it, never asked: it was walked
 * over first when reclaim was at work on it.
 */
void lru_cast_wait_goal(struct lruvec *lruvec, int type)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	unsigned long waits = 0, nr_w;
	int i;

	nr_w = lru_cast_lru_size(lruvec, type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON);
	for (i = 0; i < LRU_CAST_WAITS; i++)
		waits += k->wait_nr[i];
	k->wait_goal = max(nr_w > waits ? nr_w - waits : 0UL, LRU_CAST_WAIT_LOOK);
}

/* the folio that has waited longest, if any waits */
static struct folio *lru_cast_wait_oldest(struct lru_cast *k)
{
	long e;

	for (e = (long)k->wait_epoch - LRU_CAST_LOOKS + 1; e <= (long)k->wait_epoch; e++)
		if (e >= 0 && !list_empty(&k->wait[e % LRU_CAST_WAITS]))
			return lru_to_folio(&k->wait[e % LRU_CAST_WAITS]);
	return NULL;
}

/*
 * W's excess joins M by key, its tail first, at most @budget pages, once
 * W is over its share by more than @slack; the lists' sizes, *@nr_w and
 * *@nr_m, follow, once per zone. Run where a folio enters W
 * (__lru_cast_add_folio()), so that W holds no more than its share, as in
 * the model, but for the slack; and again by reclaim, should the share
 * have shrunk. Under the lru_lock.
 */
unsigned long lru_cast_w_admit(struct lruvec *lruvec, int type, unsigned long *nr_w,
			    unsigned long *nr_m, unsigned long slack, unsigned long budget)
{
	enum lru_list lru_w = type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	enum lru_list lru_m = lru_w + LRU_ACTIVE;
	struct list_head *w = &lruvec->lists[lru_w];
	unsigned long target = (*nr_w + *nr_m) * lru_cast_param_w(type) / 1000;
	unsigned long moved[MAX_NR_ZONES] = { 0 }, admitted = 0, nr_pages;
	int zid;

	lru_cast_wait_turn(lruvec, type);
	if (*nr_w <= target + slack)
		return 0;
	lru_cast_ring_prepare(lruvec, type);
	while (budget && *nr_w > target && !list_empty(w)) {
		struct folio *folio = lru_to_folio(w);
		struct list_head *prev = folio->lru.prev;

		/* the one after it, cold since it came in: asked for while this one is placed */
		if (prev != w)
			lru_cast_prefetch_frame(folio_pfn(list_entry(prev, struct folio, lru)), true);

		/* still being read in: it has not arrived, and waits in W until it has */
		if (folio_test_locked(folio))
			break;
		/* nor one written and to be freed: reclaim takes it from here */
		if (lru_cast_written(folio))
			break;

		nr_pages = folio_nr_pages(folio);
		list_del(&folio->lru);
		folio_set_active(folio);
		moved[folio_zonenum(folio)] += nr_pages;
		lru_cast_ring_admit(lruvec, folio, type);
		*nr_w -= min(*nr_w, nr_pages);
		*nr_m += nr_pages;
		budget -= min(budget, nr_pages);
		admitted += nr_pages;
	}
	for (zid = 0; admitted && zid < MAX_NR_ZONES; zid++) {
		if (!moved[zid])
			continue;
		lru_cast_update_lru_size(lruvec, lru_w, zid, -moved[zid]);
		lru_cast_update_lru_size(lruvec, lru_m, zid, moved[zid]);
	}
	if (admitted)
		lru_cast_age_nonresident(lruvec, admitted);
	return admitted;
}

unsigned long lru_cast_isolate(struct lruvec *lruvec, struct lru_cast_scan *s,
				struct list_head *dst)
{
	enum lru_list lru_w = s->type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	struct list_head *w = &lruvec->lists[lru_w];
	unsigned long nr_w = s->nr_w, nr_m = s->nr_m, scan = 0, nr_pages;
	struct lru_cast *k = &lruvec->lru_cast[s->type];
	unsigned long joined[MAX_NR_ZONES] = { 0 }, nr_joined = 0;
	struct folio *held = NULL;
	long held_bm = 0;
	LIST_HEAD(aside_w);
	LIST_HEAD(aside_c);
	LIST_HEAD(aside_m);
	int zid;

	lru_cast_w_admit(lruvec, s->type, &nr_w, &nr_m, 0, s->nr_to_scan * LRU_CAST_ADMIT);
	lru_cast_ring_prepare(lruvec, s->type);

	while (scan < s->nr_to_scan) {
		struct folio *folio;
		bool from_m = true, from_c = false;
		long bm = 0;

		/*
		 * W's cold: the unmapped folios of the file LRU, in the order
		 * they came (struct lru_cast). Every read of one is recorded as
		 * it is made, so one read once is known to be that, and there is
		 * nothing to wait for: its tail meets M's lowest, the lower of
		 * the two goes, and nothing read once joins M. One read again
		 * since has its place in M; one mapped since, that the walker
		 * has yet to tell of, waits with the others that do.
		 *
		 * Joined to M as the rest of W is, the pages of a file read
		 * once were where a sweep came by them and not where reclaim
		 * needed one, and a sweep that found fewer than its 128 of them
		 * in its 4096 frames handed out what else was there: of 300 MiB
		 * of files read over and over beside a stream, a tenth to a
		 * fifth were evicted a run, four in five of them with a key
		 * above tau.
		 */
		if (!list_empty(&k->cold) && !folio_test_locked(lru_to_folio(&k->cold))) {
			struct folio *c = lru_to_folio(&k->cold);

			nr_pages = folio_nr_pages(c);
			if ((lru_cast_read(c) >> 24) >= 2) {
				list_del(&c->lru);
				folio_set_active(c);
				joined[folio_zonenum(c)] += nr_pages;
				nr_joined += nr_pages;
				lru_cast_ring_admit(lruvec, c, s->type);
				nr_w -= min(nr_w, nr_pages);
				nr_m += nr_pages;
				scan += nr_pages;
				s->nr_scanned += nr_pages;
				continue;
			}
			if (folio_mapped(c)) {
				list_move(&c->lru, &k->wait[k->wait_epoch % LRU_CAST_WAITS]);
				k->wait_nr[k->wait_epoch % LRU_CAST_WAITS] += nr_pages;
				scan += nr_pages;
				s->nr_scanned += nr_pages;
				continue;
			}
			if (!held && nr_m)
				held = lru_cast_ring_min(lruvec, s->type, &held_bm);
			if (held && held_bm < lru_cast_ring_key(lruvec, c, s->type)) {
				folio = held;
				bm = held_bm;
				held = NULL;
				WRITE_ONCE(k->victim, bm);
			} else {
				folio = c;
				from_m = false;
				from_c = true;
			}
			goto got;
		}

		/* written, its write ended: freed before anything else is written */
		if (!list_empty(w) && lru_cast_written(lru_to_folio(w))) {
			folio = lru_to_folio(w);
			from_m = false;
			goto got;
		}
		/*
		 * The weighed part of W is in the order its folios came, and
		 * its tail is its oldest, not its coldest. One at the tail that
		 * was read again has its place in M, as one of W's cold has,
		 * and is not met with M's lowest: left there, a working set's
		 * folio with the key of M's candidates -- the same working set
		 * -- stood between reclaim and the stream read once behind it,
		 * and M's candidates went, all of them.
		 */
		if (!list_empty(w) && !folio_test_locked(lru_to_folio(w)) &&
		    (lru_cast_read(lru_to_folio(w)) >> 24) >= 2) {
			struct folio *c = lru_to_folio(w);

			nr_pages = folio_nr_pages(c);
			list_del(&c->lru);
			folio_set_active(c);
			joined[folio_zonenum(c)] += nr_pages;
			nr_joined += nr_pages;
			lru_cast_ring_admit(lruvec, c, s->type);
			nr_w -= min(nr_w, nr_pages);
			nr_m += nr_pages;
			scan += nr_pages;
			s->nr_scanned += nr_pages;
			continue;
		}
		if (held) {
			folio = held;
			bm = held_bm;
			held = NULL;
		} else {
			folio = nr_m ? lru_cast_ring_min(lruvec, s->type, &bm) : NULL;
		}
		/*
		 * W's tail is still at the gate, weighed and not admitted, more
		 * of W being due than one call admits. With its key below M's lowest
		 * it is the one to go and M's goes back -- or what is in W would
		 * be kept at the cost of all that is in M, a working set read a
		 * thousand times for a stream that came in a moment ago. Met
		 * only when it waited, a stream's folio the walker had seen
		 * young once more than its fault no longer waited, and in a
		 * memcg whose M was its working set and no more, that working
		 * set was handed out by M's sweep, all of it, before the stream.
		 */
		if (folio && !list_empty(w)) {
			struct folio *last = lru_to_folio(w);

			if (!folio_test_locked(last) &&
			    lru_cast_ring_key(lruvec, last, s->type) < bm) {
				list_move(&folio->lru, &aside_m);
				nr_m -= min(nr_m, (unsigned long)folio_nr_pages(folio));
				folio = NULL;
			}
		}
		/*
		 * What waits is not weighed, and reclaim does not wait for it
		 * to be: the walks are asked for ahead of it (walk/walk.c). The
		 * one that has waited longest goes with its key below that of
		 * what would go otherwise -- M's lowest, or the weighed tail of
		 * W -- or with nothing else to take. One seen in use while it
		 * waited waits no more: it is weighed, and not in the way of
		 * those behind it.
		 */
		{
			struct folio *wt = lru_cast_wait_oldest(k);
			long other;
			int n;

			for (n = 0; wt && n < 2 * SWAP_CLUSTER_MAX && (lru_cast_read(wt) >> 24) >= 2; n++) {
				list_move(&wt->lru, w);
				wt = lru_cast_wait_oldest(k);
			}
			other = folio ? bm : list_empty(w) ? LONG_MAX :
				lru_cast_ring_key(lruvec, lru_to_folio(w), s->type);
			if (wt && !folio_test_locked(wt) &&
			    ((!folio && list_empty(w)) ||
			     lru_cast_ring_key(lruvec, wt, s->type) < other)) {
				if (folio) {
					list_move(&folio->lru, &aside_m);
					nr_m -= min(nr_m, (unsigned long)folio_nr_pages(folio));
				}
				folio = wt;
				from_m = false;
				goto got;
			}
		}
		if (folio) {
			WRITE_ONCE(lruvec->lru_cast[s->type].victim, bm);
		} else if (!list_empty(w)) {
			folio = lru_to_folio(w);
			from_m = false;
		} else {
			break;
		}

got:
		nr_pages = folio_nr_pages(folio);
		s->nr_scanned += nr_pages;
		if (from_m)
			nr_m -= min(nr_m, nr_pages);
		else
			nr_w -= min(nr_w, nr_pages);

		/* not eligible: set aside, back where it was at the end */
		if (folio_zonenum(folio) > s->reclaim_idx) {
			s->skipped[folio_zonenum(folio)] += nr_pages;
			goto aside;
		}
		scan += nr_pages;
		if (!folio_test_lru(folio) || (!s->may_unmap && folio_mapped(folio)))
			goto aside;
		/*
		 * Pinned for a device (FOLL_LONGTERM: CUDA, io_uring, RDMA): the
		 * unmap fails, and the folio would go back to M under the same
		 * key, still the lowest, for reclaim to take again -- the CPU
		 * does not read it, so nothing lifts it. Lift it, as program
		 * text in use is: it is in use, and a victim must be one that
		 * can go. (How much of a type is pinned, lru_cast_pick() reads
		 * off the node's pin counts.)
		 */
		if (unlikely(folio_maybe_dma_pinned(folio))) {
			lru_cast_access_kept(folio);
			goto aside;
		}
		if (unlikely(!folio_try_get(folio)))
			goto aside;
		if (!folio_test_clear_lru(folio)) {
			folio_put(folio);
			goto aside;
		}

		list_move(&folio->lru, dst);
		if (from_m) {
			s->taken_m[folio_zonenum(folio)] += nr_pages;
			folio_clear_active(folio);
			lru_cast_ring_out(folio);
		} else {
			s->taken_w[folio_zonenum(folio)] += nr_pages;
			folio_set_rekey(folio);	/* not placed by a key: reclaim asks it */
		}
		s->nr_taken += nr_pages;
		continue;
aside:
		list_move(&folio->lru, from_m ? &aside_m : from_c ? &aside_c : &aside_w);
	}

	if (held)
		lru_cast_ring_unmin(lruvec, s->type);
	for (zid = 0; nr_joined && zid < MAX_NR_ZONES; zid++) {
		if (!joined[zid])
			continue;
		lru_cast_update_lru_size(lruvec, lru_w, zid, -joined[zid]);
		lru_cast_update_lru_size(lruvec, lru_w + LRU_ACTIVE, zid, joined[zid]);
	}
	if (nr_joined)
		lru_cast_age_nonresident(lruvec, nr_joined);
	list_splice(&aside_w, w);
	list_splice(&aside_c, &k->cold);
	while (!list_empty(&aside_m)) {
		struct folio *folio = lru_to_folio(&aside_m);

		list_del(&folio->lru);
		lru_cast_ring_put(lruvec, folio, s->type);
	}
	if (s->nr_taken)
		lru_cast_evicted(lruvec_pgdat(lruvec), s->type, s->nr_taken);
	return s->nr_taken;
}

/*
 * Reclaim found @folio young and credited the read. It stays only if that
 * lifted it above the key reclaim was taking at; still at or below,
 * it is as good a victim as before -- the same decision as picking M's
 * lowest again after the credit. Without this, a cache whose every folio is
 * in use would keep them all and make no progress.
 */
bool lru_cast_still_victim(struct folio *folio)
{
	int type = folio_is_file_lru(folio);
	bool still;

	/* not read since its key was taken at or below that one */
	if (!folio_test_rekey(folio))
		return true;
	/* the folio is isolated: its memcg is held, its lruvec read under RCU */
	rcu_read_lock();
	still = lru_cast_bucket(folio, type) <=
		READ_ONCE(folio_lruvec(folio)->lru_cast[type].victim);
	rcu_read_unlock();
	return still;
}
