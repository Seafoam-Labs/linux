/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Piece-Of-Cake (POC) CPU Selector: learned WF_SYNC honesty, the per-task
 * history and the verdicts drawn from it.
 *
 * Included once, from kernel/sched/fair.c ahead of dequeue_task_fair(),
 * whose sleep hook resolves a task's pending sync wake; the wakeup-time
 * hooks live at the end of poc_selector.c.  It defines per-CPU variables
 * and static functions, so no other file may include it.  Like
 * poc_selector.c, it is meant to be identical on every kernel POC is
 * ported to.
 */

/*
 * Learned WF_SYNC honesty.
 *
 * WF_SYNC promises that the waker sleeps right after the wakeup, and POC's
 * waker-yield hands the wakee the waker's CPU on that promise.  Callers that
 * wake at the end of a syscall (pipe_write(), sock_def_readable(), ...)
 * cannot know what userspace does next, so the promise is often broken and
 * the wakee then waits behind a waker that keeps running.
 *
 * Each task therefore remembers its last 16 sync wakes.  One was honest if the
 * waker slept after burning at most POC_SYNC_LIE_NS of its own CPU time, and a
 * lie once it burned more without sleeping.  Sync wakes issued within that
 * window of a pending one join it and are resolved with it, by the time from
 * the first: a waker that wakes several tasks back to back and then sleeps
 * kept its promise, one that then keeps running broke it.
 *
 * Whether a handoff pays is a cost comparison.  A kept promise gains about
 * what an honest waker may still run, POC_SYNC_LIE_NS: warm caches and no
 * idle exit.  A lie serialises the wakee behind the waker for as long as the
 * waker then runs, so each task also keeps the log2 length of its lies,
 * rounded up and averaged in the log domain.  A waker is trusted if
 * lies * 2^lie_log2 <= honest * POC_SYNC_LIE_NS over those 16 wakes
 * (poc_sync_waker_trusted()).  Only the order of magnitude matters: the
 * gain and the cost of a lie are an order apart or more.  Both estimates
 * err towards no handoff: a lie is rounded up to a power of two,
 * overstating it by up to 2x, and the waker's lie length overstates the
 * cost when the wakee runs shorter.  The wakee may take the waker's CPU
 * only when the waker is trusted and, if the wakee has issued sync wakes,
 * it is trusted as well (poc_sync_wakee_trusted()); otherwise an idle prev
 * core, or the normal search without waker-yield, is used.
 *
 * A wakee on its waker's CPU preempts it unless the waker is trusted and
 * may still have more sync wakes to issue: each task also remembers which of
 * its last 16 sync wakes had others join it (see check_preempt_wakeup_fair()).
 * A wake whose wakee may preempt the waker teaches nothing: the wakee answers
 * it, so even an honest waker need not block.  Such samples are dropped, and
 * every 16th wake that would preempt waits instead, to keep learning.
 */
#define POC_SYNC_GAIN_SHIFT	10
#define POC_SYNC_LIE_NS		(1ULL << POC_SYNC_GAIN_SHIFT)
#define POC_SYNC_LIE_LOG2_MAX	40

enum { POC_SYNC_NO, POC_SYNC_YES };

/* verdict of the wake being placed from this CPU, valid within select_task_rq_fair() */
static DEFINE_PER_CPU(u8, poc_sync_verdict);
/* wakee whose sync wake poc_select_wake_cpu() already noted, for wakeup_preempt */
static DEFINE_PER_CPU(struct task_struct *, poc_sync_noted);

/*
 * May select_idle_sibling() take its early recent_used_cpu shortcut?  Not
 * when this wake is to hand the waker's CPU to the wakee (Level 0w), which
 * the shortcut would pre-empt.
 */
static __always_inline bool poc_sync_recent_ok(void)
{
	return !static_branch_likely(&poc_selector_active) ||
	       __this_cpu_read(poc_sync_verdict) == POC_SYNC_NO;
}

/*
 * Can @t's promise to sleep right after a sync wake be taken?  Yes if, over
 * its last 16 sync wakes, what the kept ones gained outweighs what its lies
 * cost.
 */
static inline bool poc_sync_waker_trusted(struct task_struct *t)
{
	const struct poc_sync *ps = &t->poc_sync;
	unsigned int lies = hweight16(ps->hist);

	return (u64)lies << ps->lie_log2 <=
	       (u64)(16 - lies) << POC_SYNC_GAIN_SHIFT;
}

/*
 * May @t still have other tasks to wake before it sleeps?  Yes if any of its
 * last 16 sync wakes had others issued right behind it.
 */
static inline bool poc_sync_waker_busy(struct task_struct *t)
{
	return t->poc_sync.chain;
}

/*
 * May @p be placed on the CPU of its sync waker?  It will sync-wake the waker
 * back from there, so it must be a trusted waker itself.  A task that never
 * issued a sync wake is not judged: how its wakeups are handled is EEVDF's
 * business.
 */
static inline bool poc_sync_wakee_trusted(struct task_struct *p)
{
	return !p->poc_sync.sampled || poc_sync_waker_trusted(p);
}

/* @t's pending sync wake is resolved: @t ran @ran ns of its own CPU after it */
static inline void poc_sync_resolve(struct task_struct *t, u64 ran)
{
	struct poc_sync *ps = &t->poc_sync;
	bool lie = ran > POC_SYNC_LIE_NS;

	ps->mark = 0;
	if (ps->on_waker_cpu)
		return;
	ps->hist = (ps->hist << 1) | lie;
	ps->chain = (ps->chain << 1) | ps->joined;
	ps->sampled = true;
	if (lie) {
		int d = min(fls64(ran), POC_SYNC_LIE_LOG2_MAX) - ps->lie_log2;

		/* half way towards this lie, rounding towards it */
		ps->lie_log2 += d > 0 ? (d + 1) / 2 : (d - 1) / 2;
	}
}

/* @p is going to sleep */
static inline void poc_sync_note_sleep(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	u64 exec = se->sum_exec_runtime;

	if (!p->poc_sync.mark)
		return;
	/* read-only: leave update_curr() to whoever else needs it */
	if (cfs_rq_of(se)->curr == se) {
		s64 delta = (s64)(rq_clock_task(rq) - se->exec_start);

		exec += delta > 0 ? delta : 0;
	}
	poc_sync_resolve(p, exec - (p->poc_sync.mark - 1));
}
