// SPDX-License-Identifier: GPL-2.0
/*
 * Adaptive Deadline I/O Scheduler (ADIOS), version 4
 *
 * A redesign on what ADIOS 1 to 3 taught: keep predicting each request's
 * device time from completions and bounding what is in flight by it, and
 * build fairness, I/O priorities and the idle class on that same quantity.
 *
 * Copyright (C) 2025-2026 Masahito Suzuki
 *
 * One currency: predicted device time.
 *
 * Every request is given a cost, the device time it is predicted to take,
 * learned from the completions of earlier requests. The cost is used three
 * times:
 *
 *  order    Requests are grouped into flows by the process that issued
 *           them, within the cgroup they belong to. A flow is charged the
 *           cost of what it dispatches, scaled by its weight, and the flow
 *           that was charged least goes next.
 *           Reads are charged a fraction of their cost, set by
 *           read_priority as in ADIOS, which is what makes a reader win
 *           against a writer of the same weight. Inside a flow
 *           requests go in deadline order: arrival for synchronous
 *           requests, arrival + target + cost for the rest, so short
 *           background requests pass long ones without starving them.
 *
 *  amount   A request is handed to the device only while the cost already
 *           in flight leaves room for it in the latency window. Whatever
 *           arrives next waits about one window at most.
 *
 *  waiting  A request, once handed over, cannot be taken back. When the
 *           flow that deserves the device most has just had a request
 *           completed and is expected to issue the next one sooner than
 *           the alternative would occupy the device, nothing is dispatched
 *           until it does. The wait is bounded by the flow's own measured
 *           think time and by the cost of the alternative.
 */
#include <linux/bio.h>
#include <linux/blk-cgroup.h>
#include <linux/blkdev.h>
#include <linux/cgroup.h>
#include <linux/hash.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/ioprio.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/version.h>

#include "elevator.h"
#include "blk.h"
#include "blk-cgroup.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"

#define ADIOS_VERSION "4.0.0-rc1"

/*
 * One source for every supported kernel. What the block layer changed
 * between versions is written against the newest interface; older kernels
 * get the newer helpers here, and the few places where behaviour itself
 * differs choose with #if where they are. Some changes reached stable
 * kernels after the release that introduced them:
 *
 *   6.13    hrtimer_setup()
 *   6.16    timer_container_of()
 *   6.16.4  ->init_sched() gets the elevator_queue, and puts it on failure
 *   6.17.3  ->depth_updated() gets the request_queue;
 *           blk_mq_set_min_shallow_depth()
 *   7.0     request_queue::async_depth
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
static inline void hrtimer_setup(struct hrtimer *timer,
				 enum hrtimer_restart (*function)(struct hrtimer *),
				 clockid_t clock_id, enum hrtimer_mode mode)
{
	hrtimer_init(timer, clock_id, mode);
	timer->function = function;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
#define timer_container_of(var, callback_timer, timer_fieldname) \
	from_timer(var, callback_timer, timer_fieldname)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 3)
static inline void blk_mq_set_min_shallow_depth(struct request_queue *q,
						unsigned int depth)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	queue_for_each_hw_ctx(q, hctx, i)
		sbitmap_queue_min_shallow_depth(&hctx->sched_tags->bitmap_tags,
						depth);
}
#endif

enum adios_op {
	ADIOS_READ,
	ADIOS_WRITE,
	ADIOS_DISCARD,
	ADIOS_OTHER,
	ADIOS_OPS,
};

enum adios_class {
	ADIOS_RT,
	ADIOS_BE,
	ADIOS_IDLE,
	ADIOS_CLASSES,
};

#define ADIOS_FLOW_BITS	6
#define ADIOS_FLOWS	(1U << ADIOS_FLOW_BITS)

/* The latency window: how much predicted device time may be in flight. */
static const u64 adios_window_default[2] = {
	16 * NSEC_PER_MSEC,		/* non-rotational */
	22 * NSEC_PER_MSEC,		/* rotational */
};

/* How long a request that is nobody's immediate concern may be put off. */
static const u64 adios_target_default[ADIOS_OPS] = {
	[ADIOS_READ]	=    2ULL * NSEC_PER_MSEC,
	[ADIOS_WRITE]	= 2000ULL * NSEC_PER_MSEC,
	[ADIOS_DISCARD]	= 8000ULL * NSEC_PER_MSEC,
	[ADIOS_OTHER]	= 0,
};

/*
 * A read is charged adios_prio_to_wmult[read_priority + 20] / 1024 of its
 * cost, the weights ADIOS 3 used for the same knob: 0 charges reads like
 * writes, each step up about 1.25 times less, 15 about a twenty-eighth.
 * Negative values favour writes. The default is higher than ADIOS 3's 8
 * because a write is also charged for the time it makes the next request
 * lose.
 */
#define ADIOS_CHARGE_ONE		1024
#define ADIOS_READ_PRIORITY	15

static const u32 adios_prio_to_wmult[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

/* A lower class is served anyway once its oldest request is this late. */
#define ADIOS_AGING		(10ULL * NSEC_PER_SEC)

/* Costs are learned from samples no longer than this. */
#define ADIOS_COST_MAX		(500 * NSEC_PER_MSEC)
/* A dispatch advances virtual time by at least this much. */
#define ADIOS_COST_MIN		NSEC_PER_USEC

/* A flow's turn lasts this part of the window: window >> ADIOS_TURN_SHIFT. */
#define ADIOS_TURN_SHIFT		5

#define ADIOS_THINK_MAX		(100 * NSEC_PER_MSEC)
/* Scheduling latency of the issuer that a wait must always allow for. */
#define ADIOS_THINK_SLACK		(50 * NSEC_PER_USEC)

#define ADIOS_FIT_PERIOD		msecs_to_jiffies(100)
#define ADIOS_FIT_SHIFT		3	/* old samples lose 1/8 per period */
#define ADIOS_FIT_FRAC		10	/* fractional bits of the aged sums */
#define ADIOS_FIT_XMAX		4096	/* pages; larger requests count as this */

/* A request's share of what it costs others is stored in 2^13 ns. */
#define ADIOS_INDUCED_UNIT		13

/*
 * Seek distance from the end of the last dispatched request, in buckets:
 * none, under 1 MiB, under 64 MiB, under 1 GiB, farther.
 */
#define ADIOS_SEEKS		5

/* How fast the corrections follow: each moves 1/2^shift of its error. */
#define ADIOS_EXCESS_SHIFT		4
#define ADIOS_SEEK_SHIFT		4
#define ADIOS_LEVEL_SHIFT		2
/* A line that saw no undisturbed sample for this many periods takes any. */
#define ADIOS_FIT_DRY		4

/* Per-request state, kept in the two pointers the block layer lends us. */
struct adios_rq {
	u32 own : 29;		/* predicted device time of its own, ns */
	u32 seek : 3;		/* seek bucket it was predicted for */
	u16 induced;		/* predicted time it costs others */
	u8  hash;		/* its flow, as an index into a class's flows */
	u8  cls : 2;
	u8  flags : 6;
};

#define ADIOS_RQ_QUEUED	0x1	/* in a flow's tree */
#define ADIOS_RQ_FLIGHT	0x2	/* counted in the window */

struct adios_flow {
	struct rb_root_cached rqs;	/* queued requests, by deadline */
	struct rb_node node;		/* in adios_data::active, by vkey */
	u64 vkey;		/* vtime when it was last put in place */
	u64 vtime;
	u32 turn;		/* device time dispatched since then, ns */
	u64 predicted;		/* device time the model expected of it, ns */
	u64 measured;		/* ... and what it took */
	u64 last_sync;		/* deadline of the last request it waited for */
	u64 idle_since;		/* when its last request completed, 0 if busy */
	u32 think;		/* typical gap before it issues the next, ns */
	u16 queued;
	u16 flight;
	u16 weight;		/* 1..8 from the ioprio, times the cgroup's / 100 */
	u8  cls;
	u8  last_op;
	bool active;
};

/* Running sums for a least-squares fit of cost = base + slope * pages. */
struct adios_sums {
	u64 n, x, y, xx, xy;
	u64 clean;		/* samples that followed one of their own flow */
};

struct adios_model {
	u64 base;		/* ns */
	u64 slope;		/* ns per page, 8 fractional bits */
	struct adios_sums sums;
	u8 dry;			/* periods without an undisturbed sample */
	bool lenient;		/* ... too many: take disturbed ones too */
	struct adios_sums __percpu *total;	/* per CPU, only ever added to */
	struct adios_sums __percpu *seen;	/* what the last fit had read */
};

struct adios_stats {
	u64 dispatched;
	u64 held_window;	/* dispatch declined: window full */
	u64 held_wait;		/* dispatch declined: waiting for a flow */
	u64 wait_hit;		/* the flow we waited for came back */
	u64 wait_miss;
	u64 aged;
};

struct adios_data {
	spinlock_t lock;
	struct request_queue *q;

	struct list_head head;		/* BLK_MQ_INSERT_AT_HEAD requests */
	struct rb_root_cached active[ADIOS_CLASSES];
	u64 vclock[ADIOS_CLASSES];
	u32 nr_queued;
	u32 busy[ADIOS_CLASSES];		/* flows with requests here or in flight */

	u64 flight_cost;
	u32 flight;
	u64 last_done;			/* completion time of the last request */
	u64 last_busy[ADIOS_CLASSES];	/* last completion per class */

	struct adios_flow *wait_for;	/* flow we may hold the device for */
	bool waiting;			/* ... and are holding it now */
	u64 wait_since;
	bool held;			/* a dispatch was declined */
	struct hrtimer wait_timer;

	u64 window;
	u64 target[ADIOS_OPS];
	u32 read_charge;		/* from read_priority */
	s8 read_priority;
	u32 async_depth;
	bool rotational;

	struct adios_model model[ADIOS_OPS][2];	/* [issued to a busy device] */
	sector_t head_pos;		/* end of the last dispatched request */
	s64 seek[ADIOS_OPS][ADIOS_SEEKS];	/* time a seek adds, by distance */
	s64 level[ADIOS_OPS];		/* what the rest still misses, lately */
	s64 excess[2][2];		/* time lost after a request of another
					 * flow: [its kind][own kind] */
	s64 induced[2];			/* by a read, by anything else; ns */
	struct adios_flow *prev;		/* whose request completed last, */
	u32 prev_induced;		/* what it was charged for costing others, */
	u8 prev_op;			/* and what it was */
	struct timer_list fit_timer;

	struct adios_stats stats;
	struct adios_flow flows[ADIOS_CLASSES][ADIOS_FLOWS];
};

static inline struct adios_rq *adios_rq(struct request *rq)
{
	return (struct adios_rq *)&rq->elv.priv;
}

static inline enum adios_op adios_op(blk_opf_t opf)
{
	switch (opf & REQ_OP_MASK) {
	case REQ_OP_READ:
		return ADIOS_READ;
	case REQ_OP_WRITE:
		return ADIOS_WRITE;
	case REQ_OP_DISCARD:
		return ADIOS_DISCARD;
	default:
		return ADIOS_OTHER;
	}
}

/*
 * Is somebody waiting for this request? Every read is, except readahead,
 * and so is whatever was submitted as synchronous.
 */
static inline bool adios_urgent(blk_opf_t opf)
{
	return op_is_sync(opf) && !(opf & REQ_RAHEAD);
}

static inline struct adios_flow *adios_flow(struct adios_data *ad,
					    struct request *rq)
{
	return &ad->flows[adios_rq(rq)->cls][adios_rq(rq)->hash];
}

/*
 * A flow is the I/O of one process in one cgroup: @cgid is the cgroup's id,
 * 0 for the root one, whose flows are therefore those of plain processes.
 */
static inline u8 adios_index(pid_t tgid, int cgid)
{
	return hash_32((u32)tgid ^ hash_32(cgid, 32), ADIOS_FLOW_BITS);
}

/*
 * cgroups
 *
 * The weight of a cgroup other than the root one, io.adios.weight,
 * scales the weight of each of its flows. Processes stay apart inside a
 * cgroup: a reader and a writer started next to each other land in the
 * same cgroup, and keeping them in one flow would put the reader in line
 * behind the writer. So a cgroup's share grows with the number of its
 * processes doing I/O, as it would outside a cgroup. The hierarchy is
 * flat: only the weight of the cgroup the I/O belongs to counts. Buffered
 * writes are written back by flusher threads but carry the cgroup that
 * dirtied the pages, so they are charged to a flow of that cgroup.
 */
#define ADIOS_CG_WEIGHT_DFL	100
#define ADIOS_CG_WEIGHT_MAX	10000

#ifdef CONFIG_BLK_CGROUP
struct adios_cgd {
	struct blkcg_policy_data cpd;
	unsigned int weight;
};

static struct blkcg_policy adios_policy;
/* Without a policy slot, all I/O is treated as the root cgroup's. */
static bool adios_policy_on;

static inline struct adios_cgd *adios_cgd(struct blkcg *blkcg)
{
	return container_of(blkcg_to_cpd(blkcg, &adios_policy), struct adios_cgd,
			    cpd);
}

/*
 * The id of the cgroup of @css and its weight; 0 for the root cgroup, and
 * when there was no policy slot, whose I/O goes by process alone.
 */
static int adios_cgroup(struct cgroup_subsys_state *css, unsigned int *weight)
{
	if (!adios_policy_on || !css || !css->parent)
		return 0;
	if (weight)
		*weight = READ_ONCE(adios_cgd(css_to_blkcg(css))->weight);
	return css->id;
}

/*
 * The flow current's next request will most likely go to. A bio may be
 * charged to another cgroup than its submitter's (writeback); then this
 * only guesses, which is all limiting the depth needs.
 */
static u8 adios_submitter(void)
{
	int cgid;

	rcu_read_lock();
	cgid = adios_cgroup(task_css(current, io_cgrp_id), NULL);
	rcu_read_unlock();
	return adios_index(task_tgid_nr(current), cgid);
}

static inline struct cgroup_subsys_state *adios_bio_css(struct bio *bio)
{
	return bio_blkcg_css(bio);
}

static struct blkcg_policy_data *adios_cpd_alloc(gfp_t gfp)
{
	struct adios_cgd *cgd = kzalloc(sizeof(*cgd), gfp);

	if (!cgd)
		return NULL;
	cgd->weight = ADIOS_CG_WEIGHT_DFL;
	return &cgd->cpd;
}

static void adios_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(container_of(cpd, struct adios_cgd, cpd));
}

static u64 adios_weight_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	return READ_ONCE(adios_cgd(css_to_blkcg(css))->weight);
}

/* Takes effect for each flow with its next request. */
static int adios_weight_write(struct cgroup_subsys_state *css,
			      struct cftype *cft, u64 val)
{
	if (val < 1 || val > ADIOS_CG_WEIGHT_MAX)
		return -ERANGE;
	WRITE_ONCE(adios_cgd(css_to_blkcg(css))->weight, val);
	return 0;
}

static struct cftype adios_cg_files[] = {
	{
		.name = "adios.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = adios_weight_read,
		.write_u64 = adios_weight_write,
	},
	{} /* terminate */
};

static struct blkcg_policy adios_policy = {
	.dfl_cftypes	= adios_cg_files,
	.cpd_alloc_fn	= adios_cpd_alloc,
	.cpd_free_fn	= adios_cpd_free,
};

static void __init adios_policy_register(void)
{
	int ret = blkcg_policy_register(&adios_policy);

	if (ret)
		pr_warn("adios: no cgroup support: %d\n", ret);
	adios_policy_on = !ret;
}

static void adios_policy_unregister(void)
{
	if (adios_policy_on)
		blkcg_policy_unregister(&adios_policy);
}
#else
static inline int adios_cgroup(struct cgroup_subsys_state *css,
			       unsigned int *weight)
{
	return 0;
}

static inline u8 adios_submitter(void)
{
	return adios_index(task_tgid_nr(current), 0);
}

static inline struct cgroup_subsys_state *adios_bio_css(struct bio *bio)
{
	return NULL;
}

static inline void adios_policy_register(void) {}
static inline void adios_policy_unregister(void) {}
#endif

/*
 * Cost model
 *
 * The cost of a request is the device time one more request of its kind
 * takes: what it takes itself, plus what it makes the next one lose.
 *
 * Its own time. A sample is the time the device spent on a request: from
 * the later of its issue and the previous completion to its own
 * completion. For a request issued to an idle device that is its service
 * time; for one issued while others were in flight it is the interval
 * between completions, which on a device that works on several requests
 * at once is much shorter. So each operation type has two lines, time =
 * base + slope * pages, one for either case, fitted by least squares to
 * exponentially aged samples. Only undisturbed samples count, those of a
 * request that followed another of the same flow, unless there have been
 * none lately.
 *
 * Two corrections follow what the lines cannot express: a table of what a
 * seek of each distance adds, and a fast running mean of what everything
 * else still misses, which follows the drive's state (how full its write
 * cache is) within a few requests.
 *
 * What it costs others. A request does not always pay for itself: a drive
 * with a write cache completes a write at once and does the work while the
 * next request waits; a head moved away has to come back. So whenever a
 * request follows one of another flow, the time it took beyond its own
 * estimate is learned per pair of kinds, (the one before, its own): a
 * write after a write and a write after a read are different matters. That
 * time is charged to the flow of the request before, and its running mean
 * is the second part of that kind's cost.
 *
 * Fitted offline to block traces of a disk, a small neural network did no
 * better than this once both learned online: what the drive does next
 * depends on state no input shows, and following it quickly is what
 * counts. Seek distance, the preceding kind and whether it came from the
 * same flow were the inputs that mattered.
 */
static inline unsigned int adios_kind(enum adios_op op)
{
	return op != ADIOS_READ;
}

static inline u64 adios_pages(unsigned int bytes)
{
	return min_t(u64, DIV_ROUND_UP(bytes, PAGE_SIZE), ADIOS_FIT_XMAX);
}

static inline u32 adios_cost(const struct adios_rq *ar)
{
	return ar->own + ((u32)ar->induced << ADIOS_INDUCED_UNIT);
}

/* The line for @op, as issued to a busy (@among) or an idle device. */
static const struct adios_model *adios_line(struct adios_data *ad,
					    enum adios_op op, bool among)
{
	const struct adios_model *m = &ad->model[op][among];

	/* Until a line has samples of its own the other one stands in. */
	return READ_ONCE(m->sums.n) ? m : &ad->model[op][!among];
}

/* The time the line for @op gives a request of @bytes. */
static inline u32 adios_line_time(struct adios_data *ad, enum adios_op op,
				  bool among, unsigned int bytes)
{
	const struct adios_model *m = adios_line(ad, op, among);

	return min_t(u64, ADIOS_COST_MAX, READ_ONCE(m->base) +
		     ((READ_ONCE(m->slope) * adios_pages(bytes)) >> 8));
}

static unsigned int adios_seek(struct adios_data *ad, struct request *rq)
{
	sector_t pos = blk_rq_pos(rq), head = ad->head_pos;
	u64 d = pos > head ? pos - head : head - pos;

	if (!d)
		return 0;
	if (d < SZ_1M >> SECTOR_SHIFT)
		return 1;
	if (d < SZ_64M >> SECTOR_SHIFT)
		return 2;
	if (d < SZ_1G >> SECTOR_SHIFT)
		return 3;
	return 4;
}

/*
 * Predict the cost of @rq if it is issued now to a busy (@among) or idle
 * device. Called with the lock held.
 */
static void adios_predict(struct adios_data *ad, struct request *rq, bool among)
{
	enum adios_op op = adios_op(rq->cmd_flags);
	struct adios_rq *ar = adios_rq(rq);
	s64 own;

	ar->seek = adios_seek(ad, rq);
	own = (s64)adios_line_time(ad, op, among, blk_rq_bytes(rq)) +
	      ad->seek[op][ar->seek] + ad->level[op];
	ar->own = clamp_t(s64, own, 0, ADIOS_COST_MAX);
	ar->induced = clamp_t(s64, ad->induced[adios_kind(op)], 0, ADIOS_COST_MAX) >>
		      ADIOS_INDUCED_UNIT;
}

static void adios_sample(struct adios_data *ad, enum adios_op op, bool among,
			 unsigned int bytes, u64 ns, u32 own, bool clean)
{
	struct adios_model *m = &ad->model[op][among];
	u64 pages = adios_pages(bytes);
	struct adios_sums *s;
	unsigned long flags;

	/* The timer also notices when a line gets no undisturbed samples. */
	if (!timer_pending(&ad->fit_timer))
		mod_timer(&ad->fit_timer, jiffies +
			  (READ_ONCE(m->sums.n) ? ADIOS_FIT_PERIOD : 1));

	if (ns > ADIOS_COST_MAX || !(clean || READ_ONCE(m->lenient)))
		return;
	/* One stall must not rewrite the line: trim it to 4x the estimate. */
	if (own && ns > 4ULL * own)
		ns = 4ULL * own;

	local_irq_save(flags);
	s = this_cpu_ptr(m->total);
	s->n++;
	s->x += pages;
	s->y += ns;
	s->xx += pages * pages;
	s->xy += pages * ns;
	s->clean += clean;
	local_irq_restore(flags);
}

/* Refit one line with the samples since the last time. */
static void adios_fit(struct adios_model *m)
{
	struct adios_sums *s = &m->sums, d = {};
	u64 mx, my, vxx, exy, slope;
	int cpu;

	/* The other CPUs keep adding: read, and remember what was read. */
	for_each_possible_cpu(cpu) {
		struct adios_sums t = *per_cpu_ptr(m->total, cpu);
		struct adios_sums *o = per_cpu_ptr(m->seen, cpu);

		d.n += t.n - o->n;
		d.x += t.x - o->x;
		d.y += t.y - o->y;
		d.xx += t.xx - o->xx;
		d.xy += t.xy - o->xy;
		d.clean += t.clean - o->clean;
		*o = t;
	}
	if (d.clean) {
		m->dry = 0;
		WRITE_ONCE(m->lenient, false);
	} else if (m->dry < ADIOS_FIT_DRY && ++m->dry == ADIOS_FIT_DRY) {
		WRITE_ONCE(m->lenient, true);
	}
	if (!d.n)
		return;

	/* Aged sums carry ADIOS_FIT_FRAC fractional bits, so all age alike. */
	s->n += (d.n << ADIOS_FIT_FRAC) - (s->n >> ADIOS_FIT_SHIFT);
	s->x += (d.x << ADIOS_FIT_FRAC) - (s->x >> ADIOS_FIT_SHIFT);
	s->y += (d.y << ADIOS_FIT_FRAC) - (s->y >> ADIOS_FIT_SHIFT);
	s->xx += (d.xx << ADIOS_FIT_FRAC) - (s->xx >> ADIOS_FIT_SHIFT);
	s->xy += (d.xy << ADIOS_FIT_FRAC) - (s->xy >> ADIOS_FIT_SHIFT);

	mx = mul_u64_u64_div_u64(s->x, 1 << 8, s->n);	/* pages, 8 frac bits */
	my = div64_u64(s->y, s->n);			/* ns */
	vxx = mul_u64_u64_div_u64(s->xx, 1 << 16, s->n);	/* 16 frac bits */
	exy = mul_u64_u64_div_u64(s->xy, 1 << 8, s->n);	/* 8 frac bits */

	slope = m->slope;
	/*
	 * The slope is only identifiable when sizes vary; otherwise keep
	 * the old one and let the base absorb the mean.
	 */
	if (vxx > mx * mx + (mx * mx >> 6) && exy > mx * my)
		slope = mul_u64_u64_div_u64(exy - mx * my, 1 << 16,
					    vxx - mx * mx);
	if (((slope * mx) >> 16) > my)
		slope = div64_u64(my << 16, max_t(u64, mx, 1));

	WRITE_ONCE(m->slope, slope);
	WRITE_ONCE(m->base, my - ((slope * mx) >> 16));
}

static void adios_fit_timer(struct timer_list *t)
{
	struct adios_data *ad = timer_container_of(ad, t, fit_timer);

	for (int op = 0; op < ADIOS_OPS; op++) {
		adios_fit(&ad->model[op][0]);
		adios_fit(&ad->model[op][1]);
	}
}

/*
 * Flows
 */
static void adios_flow_enqueue(struct adios_data *ad, struct adios_flow *f)
{
	struct rb_node **p = &ad->active[f->cls].rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	f->vkey = f->vtime;
	f->turn = 0;
	while (*p) {
		struct adios_flow *o = rb_entry(*p, struct adios_flow, node);

		parent = *p;
		if ((s64)(f->vkey - o->vkey) < 0) {
			p = &parent->rb_left;
		} else {
			p = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&f->node, parent, p);
	rb_insert_color_cached(&f->node, &ad->active[f->cls], leftmost);
	f->active = true;
}

static void adios_flow_dequeue(struct adios_data *ad, struct adios_flow *f)
{
	rb_erase_cached(&f->node, &ad->active[f->cls]);
	f->active = false;
}

static inline struct adios_flow *adios_flow_first(struct adios_data *ad,
						  int cls)
{
	struct rb_node *n = rb_first_cached(&ad->active[cls]);

	return n ? rb_entry(n, struct adios_flow, node) : NULL;
}

static inline struct request *adios_flow_head(struct adios_flow *f)
{
	struct rb_node *n = rb_first_cached(&f->rqs);

	return n ? rb_entry(n, struct request, rb_node) : NULL;
}

/*
 * Charge @f for @ns of device time spent on an operation of type @op.
 * @ns is negative when an earlier charge turned out too high. An active
 * flow keeps its place until adios_flow_settle() moves it.
 */
static inline void adios_charge(struct adios_data *ad, struct adios_flow *f,
				enum adios_op op, s64 ns)
{
	s64 charge = op == ADIOS_READ ? ad->read_charge : ADIOS_CHARGE_ONE;

	f->vtime += div_s64(ns * charge, f->weight);
}

/* Put an active flow where its charges say it belongs. */
static inline void adios_flow_settle(struct adios_data *ad,
				     struct adios_flow *f)
{
	if (f->active && f->vkey != f->vtime) {
		adios_flow_dequeue(ad, f);
		adios_flow_enqueue(ad, f);
	}
}

/* How far behind the class's clock a returning flow may start. */
static inline u64 adios_credit(struct adios_data *ad, struct adios_flow *f)
{
	return div_u64(ad->window * ADIOS_CHARGE_ONE, f->weight);
}

static void adios_add(struct adios_data *ad, struct request *rq, u64 now)
{
	struct adios_flow *f = adios_flow(ad, rq);
	struct rb_node **p = &f->rqs.rb_root.rb_node, *parent = NULL;
	enum adios_op op = adios_op(rq->cmd_flags);
	bool leftmost = true;
	u64 key;

	if (adios_urgent(rq->cmd_flags)) {
		/* Requests one issuer waits for keep their order. */
		key = max(rq->start_time_ns, f->last_sync);
		f->last_sync = key;
	} else {
		key = rq->start_time_ns + ad->target[op] + adios_cost(adios_rq(rq));
	}
	rq->fifo_time = key;

	while (*p) {
		struct request *o = rb_entry(*p, struct request, rb_node);

		parent = *p;
		if ((s64)(key - o->fifo_time) < 0) {
			p = &parent->rb_left;
		} else {
			p = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&rq->rb_node, parent, p);
	rb_insert_color_cached(&rq->rb_node, &f->rqs, leftmost);
	adios_rq(rq)->flags |= ADIOS_RQ_QUEUED;
	ad->nr_queued++;

	if (!f->queued && !f->flight)
		ad->busy[f->cls]++;
	if (!f->queued++ && !f->flight && f->idle_since) {
		/* It was idle: learn how long it takes to come back. */
		u64 gap = min_t(u64, now - f->idle_since, ADIOS_THINK_MAX);

		if (gap < f->think)
			f->think = (f->think + gap) >> 1;
		else
			f->think += (gap - f->think) >> 3;
		f->idle_since = 0;
	}

	if (!f->active) {
		u64 floor = ad->vclock[f->cls] - adios_credit(ad, f);

		if ((s64)(f->vtime - floor) < 0)
			f->vtime = floor;
		adios_flow_enqueue(ad, f);
	}
}

/* Take a queued request out of its flow. The flow's place is the caller's. */
static void adios_unlink(struct adios_data *ad, struct adios_flow *f,
			 struct request *rq)
{
	rb_erase_cached(&rq->rb_node, &f->rqs);
	RB_CLEAR_NODE(&rq->rb_node);
	adios_rq(rq)->flags &= ~ADIOS_RQ_QUEUED;
	ad->nr_queued--;
	f->queued--;

	elv_rqhash_del(ad->q, rq);
	if (ad->q->last_merge == rq)
		ad->q->last_merge = NULL;
}

static void adios_del(struct adios_data *ad, struct request *rq)
{
	struct adios_flow *f = adios_flow(ad, rq);

	if (!(adios_rq(rq)->flags & ADIOS_RQ_QUEUED))
		return;
	adios_unlink(ad, f, rq);
	if (!f->queued) {
		adios_flow_dequeue(ad, f);
		if (!f->flight)
			ad->busy[f->cls]--;
	}
}

/* The request is no longer the device's: give its cost back to the window. */
static void adios_landed(struct adios_data *ad, struct request *rq, u64 now,
			 bool completed)
{
	struct adios_flow *f = adios_flow(ad, rq);

	if (!(adios_rq(rq)->flags & ADIOS_RQ_FLIGHT))
		return;
	adios_rq(rq)->flags &= ~ADIOS_RQ_FLIGHT;

	ad->flight--;
	ad->flight_cost -= adios_cost(adios_rq(rq));
	f->flight--;
	if (!f->flight && !f->queued)
		ad->busy[f->cls]--;
	if (!completed)
		return;

	ad->last_busy[f->cls] = now;
	if (!f->flight && !f->queued) {
		f->idle_since = now;
		/* An issuer that waited for this is likely to follow up. */
		if (op_is_sync(rq->cmd_flags))
			ad->wait_for = f;
	}
}

/*
 * Dispatch
 */
static void adios_kick(struct adios_data *ad)
{
	if (ad->held) {
		ad->held = false;
		blk_mq_run_hw_queues(ad->q, true);
	}
}

static enum hrtimer_restart adios_wait_timer(struct hrtimer *t)
{
	struct adios_data *ad = container_of(t, struct adios_data, wait_timer);
	unsigned long flags;

	spin_lock_irqsave(&ad->lock, flags);
	adios_kick(ad);
	spin_unlock_irqrestore(&ad->lock, flags);
	return HRTIMER_NORESTART;
}

/* The flow to serve next: the least charged one of the best class. */
static struct adios_flow *adios_pick(struct adios_data *ad, u64 now)
{
	struct adios_flow *pick = NULL;

	for (int cls = ADIOS_CLASSES - 1; cls >= 0; cls--) {
		struct adios_flow *f = adios_flow_first(ad, cls);

		if (!f)
			continue;
		/* A better class goes first, unless this one waited too long. */
		if (pick && (s64)(now - (adios_flow_head(pick)->start_time_ns +
					 ADIOS_AGING)) >= 0) {
			ad->stats.aged++;
			return pick;
		}
		pick = f;
	}
	return pick;
}

/*
 * Should the device be kept free for @w rather than given @rq of @f?
 * Returns how much longer to wait, 0 for no.
 */
static u64 adios_wait(struct adios_data *ad, struct adios_flow *w,
		      struct adios_flow *f, struct request *rq, u64 now)
{
	u64 limit, waited;

	if (w == f || w->queued || w->flight || !w->idle_since)
		return 0;
	/* It must deserve the device more than the alternative does. */
	if (w->cls > f->cls ||
	    (w->cls == f->cls && (s64)(w->vtime - f->vtime) >= 0))
		return 0;

	/*
	 * Waiting costs the device the issuer's think time; not waiting costs
	 * the issuer the alternative's service time. Wait only when the first
	 * is the smaller, and never longer than the second.
	 */
	limit = adios_cost(adios_rq(rq));
	if (w->think + ADIOS_THINK_SLACK >= limit)
		return 0;
	limit = min_t(u64, limit, 2ULL * w->think + ADIOS_THINK_SLACK);

	waited = now - w->idle_since;
	return waited < limit ? limit - waited : 0;
}

static struct request *adios_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct adios_data *ad = hctx->queue->elevator->elevator_data;
	struct adios_flow *f, *w;
	struct request *rq;
	unsigned long flags;
	u64 now = ktime_get_ns(), left, window;
	enum adios_op op;

	spin_lock_irqsave(&ad->lock, flags);

	rq = list_first_entry_or_null(&ad->head, struct request, queuelist);
	if (rq) {
		list_del_init(&rq->queuelist);
		goto out;
	}
	if (!ad->nr_queued)
		goto out;

	f = adios_pick(ad, now);
	rq = adios_flow_head(f);
	adios_predict(ad, rq, ad->flight);

	w = ad->wait_for;
	if (w) {
		left = adios_wait(ad, w, f, rq, now);
		if (left) {
			ad->stats.held_wait++;
			if (!ad->waiting) {
				ad->waiting = true;
				ad->wait_since = now;
			}
			ad->held = true;
			hrtimer_start(&ad->wait_timer, ns_to_ktime(left),
				      HRTIMER_MODE_REL);
			rq = NULL;
			goto out;
		}
		if (ad->waiting) {
			/*
			 * The device was kept free for @w: that time was
			 * @w's, whether or not it came back, and @w pays for
			 * it as for the work it was kept free for.
			 */
			adios_charge(ad, w, w->last_op, now - ad->wait_since);
			adios_flow_settle(ad, w);
			ad->waiting = false;
			if (w == f)
				ad->stats.wait_hit++;
			else
				ad->stats.wait_miss++;
			/* The charge may have changed who is next. */
			f = adios_pick(ad, now);
			rq = adios_flow_head(f);
			adios_predict(ad, rq, ad->flight);
		}
		ad->wait_for = NULL;
	}

	/*
	 * The idle class only gets what would otherwise go unused: a small
	 * part of the window, and only once the others have been quiet for
	 * a whole window.
	 */
	window = ad->window;
	if (f->cls == ADIOS_IDLE &&
	    (s64)(now - (rq->start_time_ns + ADIOS_AGING)) < 0) {
		u64 busy = max(ad->last_busy[ADIOS_RT], ad->last_busy[ADIOS_BE]);

		window >>= 2;
		if (now - busy < ad->window) {
			ad->held = true;
			hrtimer_start(&ad->wait_timer,
				      ns_to_ktime(ad->window - (now - busy)),
				      HRTIMER_MODE_REL);
			rq = NULL;
			goto out;
		}
	}

	if (ad->flight && ad->flight_cost + adios_cost(adios_rq(rq)) > window) {
		ad->stats.held_window++;
		ad->held = true;
		rq = NULL;
		goto out;
	}

	op = adios_op(rq->cmd_flags);
	if ((s64)(f->vtime - ad->vclock[f->cls]) > 0)
		ad->vclock[f->cls] = f->vtime;
	adios_unlink(ad, f, rq);
	if (!f->queued)
		adios_flow_dequeue(ad, f);
	adios_charge(ad, f, op, max_t(u32, adios_cost(adios_rq(rq)), ADIOS_COST_MIN));
	f->last_op = op;
	/*
	 * A flow keeps its turn for a small part of the window before it is
	 * moved back in line: on a fast device that is hundreds of requests
	 * without touching the tree, on a slow one it is every request.
	 */
	f->turn += adios_cost(adios_rq(rq));
	if (f->turn >= ad->window >> ADIOS_TURN_SHIFT)
		adios_flow_settle(ad, f);

	adios_rq(rq)->flags |= ADIOS_RQ_FLIGHT;
	ad->flight++;
	ad->flight_cost += adios_cost(adios_rq(rq));
	f->flight++;
	ad->stats.dispatched++;
out:
	if (rq && blk_rq_sectors(rq))
		ad->head_pos = blk_rq_pos(rq) + blk_rq_sectors(rq);
	spin_unlock_irqrestore(&ad->lock, flags);
	if (rq)
		rq->rq_flags |= RQF_STARTED;
	return rq;
}

static bool adios_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct adios_data *ad = hctx->queue->elevator->elevator_data;

	return READ_ONCE(ad->nr_queued) || !list_empty_careful(&ad->head);
}

/*
 * Arrival
 */
static inline enum adios_class adios_class(unsigned int ioprio)
{
	switch (IOPRIO_PRIO_CLASS(ioprio)) {
	case IOPRIO_CLASS_RT:
		return ADIOS_RT;
	case IOPRIO_CLASS_IDLE:
		return ADIOS_IDLE;
	default:
		return ADIOS_BE;
	}
}

/*
 * Requests wait for a tag before they ever get here, and whoever holds
 * the tags decides who is seen. So no flow may keep more requests waiting
 * here than its part of the tags, counting one part more than there are
 * busy flows so that a newcomer finds one free; the idle class gets an
 * eighth of a part while anyone else is busy. A flow whose requests go
 * straight through is not held back by this. Beyond that, synchronous
 * reads may use every tag and everything else stops a quarter short.
 */
static void adios_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	struct adios_data *ad = data->q->elevator->elevator_data;
	enum adios_class cls = adios_class(get_current_ioprio());
	const struct adios_flow *f = &ad->flows[cls][adios_submitter()];
	unsigned int others = READ_ONCE(ad->busy[ADIOS_RT]) +
			      READ_ONCE(ad->busy[ADIOS_BE]);
	unsigned int part = data->q->nr_requests /
			    (others + READ_ONCE(ad->busy[ADIOS_IDLE]) + 1);

	if (cls == ADIOS_IDLE && others)
		part >>= 3;
	if (READ_ONCE(f->queued) >= max(part, 1U))
		data->shallow_depth = 1;
	else if (!(op_is_sync(opf) && !op_is_write(opf)))
		data->shallow_depth = ad->async_depth;
}

/* Everything but synchronous reads stops a quarter short of the tags. */
static unsigned int adios_default_async_depth(struct request_queue *q)
{
	unsigned int nr = q->nr_requests;

	return max(1U, nr - max(1U, nr / 4));
}

/*
 * Since 7.0 that depth is request_queue's async_depth, which the user may
 * change in /sys/block/<dev>/queue/async_depth; ADIOS only sets its default.
 */
static void adios_set_depth(struct request_queue *q)
{
	struct adios_data *ad = q->elevator->elevator_data;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	ad->async_depth = q->async_depth;
#else
	ad->async_depth = adios_default_async_depth(q);
#endif
	blk_mq_set_min_shallow_depth(q, 1);
}

/* The queue's size, or its async_depth, changed. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 3)
static void adios_depth_updated(struct request_queue *q)
{
	adios_set_depth(q);
}
#else
static void adios_depth_updated(struct blk_mq_hw_ctx *hctx)
{
	adios_set_depth(hctx->queue);
}
#endif

static void adios_prepare_request(struct request *rq)
{
	struct adios_rq *ar = adios_rq(rq);

	BUILD_BUG_ON(sizeof(*ar) > sizeof(rq->elv.priv));
	ar->own = 0;
	ar->induced = 0;
	ar->hash = adios_index(task_tgid_nr(current), 0);
	ar->cls = ADIOS_BE;
	ar->flags = 0;
}

static void adios_classify(struct adios_data *ad, struct request *rq)
{
	struct adios_rq *ar = adios_rq(rq);
	unsigned int prio = req_get_ioprio(rq);
	unsigned int level = IOPRIO_PRIO_LEVEL(prio), weight, cg_weight;
	int cgid = 0;
	struct adios_flow *f;

	ar->cls = adios_class(prio);
	if (IOPRIO_PRIO_CLASS(prio) == IOPRIO_CLASS_NONE)
		level = IOPRIO_BE_NORM;
	weight = IOPRIO_NR_LEVELS - level;
	if (rq->bio)
		cgid = adios_cgroup(adios_bio_css(rq->bio), &cg_weight);
	if (cgid)
		weight = max(1U, weight * cg_weight / ADIOS_CG_WEIGHT_DFL);
	/* Requests are inserted by the task that issued them. */
	ar->hash = adios_index(task_tgid_nr(current), cgid);
	f = adios_flow(ad, rq);
	f->cls = ar->cls;
	f->weight = weight;
}

static void adios_insert_requests(struct blk_mq_hw_ctx *hctx,
				  struct list_head *list, blk_insert_t insert_flags)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	unsigned long flags;
	LIST_HEAD(free);
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&ad->lock, flags);
	while (!list_empty(list)) {
		struct request *rq =
			list_first_entry(list, struct request, queuelist);

		list_del_init(&rq->queuelist);

		if (insert_flags & BLK_MQ_INSERT_AT_HEAD) {
			list_add_tail(&rq->queuelist, &ad->head);
			continue;
		}
		if (blk_mq_sched_try_insert_merge(q, rq, &free))
			continue;

		adios_predict(ad, rq, false);
		adios_classify(ad, rq);
		adios_add(ad, rq, now);

		if (rq_mergeable(rq)) {
			elv_rqhash_add(q, rq);
			if (!q->last_merge)
				q->last_merge = rq;
		}
	}
	spin_unlock_irqrestore(&ad->lock, flags);

	blk_mq_free_requests(&free);
}

/*
 * Merging. A bio joins a request only of its own flow, so that the charge
 * goes to whoever caused the work.
 */
static bool adios_allow_merge(struct request_queue *q, struct request *rq,
			      struct bio *bio)
{
	int cgid = adios_cgroup(adios_bio_css(bio), NULL);

	return adios_rq(rq)->hash == adios_index(task_tgid_nr(current), cgid);
}

static bool adios_bio_merge(struct request_queue *q, struct bio *bio,
			    unsigned int nr_segs)
{
	struct adios_data *ad = q->elevator->elevator_data;
	struct request *free = NULL;
	unsigned long flags;
	bool ret;

	if (!spin_trylock_irqsave(&ad->lock, flags))
		return false;
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free);
	spin_unlock_irqrestore(&ad->lock, flags);

	if (free)
		blk_mq_free_request(free);
	return ret;
}

static void adios_request_merged(struct request_queue *q, struct request *rq,
				 enum elv_merge type)
{
	struct adios_data *ad = q->elevator->elevator_data;

	/* It grew; the deadline it was given stays. */
	adios_predict(ad, rq, false);
}

static void adios_requests_merged(struct request_queue *q, struct request *rq,
				  struct request *next)
{
	struct adios_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);
	adios_del(ad, next);
	adios_request_merged(q, rq, ELEVATOR_BACK_MERGE);
}

/*
 * Departure
 */
/*
 * A request completed after @ns of device time. Settle the account: the
 * flow was charged a prediction when the request was dispatched, and now
 * pays the difference to what it really took. If it followed a request of
 * another flow, the time beyond its own line is that request's doing, as
 * far as the mean says, and is not its to pay.
 */
static void adios_settle(struct adios_data *ad, struct request *rq, u64 ns,
			 bool among)
{
	struct adios_flow *f = adios_flow(ad, rq), *p = ad->prev;
	struct adios_rq *ar = adios_rq(rq);
	enum adios_op op = adios_op(rq->cmd_flags);
	unsigned int bytes = blk_rq_stats_sectors(rq) << SECTOR_SHIFT;
	u32 line = adios_line_time(ad, op, among, bytes);
	s64 *seek = &ad->seek[op][ar->seek], *level = &ad->level[op];
	bool clean = !p || p == f;
	s64 t = ns, excess = 0, blame = 0, pred;

	if (!clean) {
		unsigned int before = adios_kind(ad->prev_op);
		s64 *cell = &ad->excess[before][adios_kind(op)];
		s64 *induced = &ad->induced[before];

		excess = *cell;
		*cell += (t - line - *seek - excess) >> ADIOS_EXCESS_SHIFT;
		*induced += (*cell - *induced) >> 3;
		/* The flow before pays for it, in place of what it paid ahead. */
		blame = clamp_t(s64, excess, 0, t);
		adios_charge(ad, p, ad->prev_op, blame - ad->prev_induced);
	} else if (p) {
		/* It followed one of its own: that one cost nobody anything. */
		adios_charge(ad, p, ad->prev_op, -(s64)ad->prev_induced);
	}
	adios_charge(ad, f, op, t - blame - ar->own);

	/* What the model expected, before it learns from this one. */
	pred = line + excess + *seek + *level;
	f->predicted += max_t(s64, pred, 0);
	f->measured += t;

	*level += (t - pred) >> ADIOS_LEVEL_SHIFT;
	*seek += (t - line - excess - *seek) >> ADIOS_SEEK_SHIFT;

	ad->prev = f;
	ad->prev_op = op;
	ad->prev_induced = (u32)ar->induced << ADIOS_INDUCED_UNIT;

	adios_sample(ad, op, among, bytes, ns, line, clean);
}

static void adios_completed_request(struct request *rq, u64 now)
{
	struct adios_data *ad = rq->q->elevator->elevator_data;
	unsigned long flags;
	u64 since;

	if (!(adios_rq(rq)->flags & ADIOS_RQ_FLIGHT))
		return;

	spin_lock_irqsave(&ad->lock, flags);
	since = max(rq->io_start_time_ns, ad->last_done);
	if (adios_op(rq->cmd_flags) != ADIOS_OTHER && rq->io_start_time_ns &&
	    now > since && now - since <= ADIOS_COST_MAX)
		adios_settle(ad, rq, now - since,
			     ad->last_done > rq->io_start_time_ns);
	ad->last_done = now;
	adios_landed(ad, rq, now, true);
	adios_kick(ad);
	spin_unlock_irqrestore(&ad->lock, flags);
}

static void adios_requeue_request(struct request *rq)
{
	struct adios_data *ad = rq->q->elevator->elevator_data;
	unsigned long flags;

	spin_lock_irqsave(&ad->lock, flags);
	adios_landed(ad, rq, 0, false);
	adios_kick(ad);
	spin_unlock_irqrestore(&ad->lock, flags);
}

static void adios_finish_request(struct request *rq)
{
	/* Freed without completing, e.g. after a failed issue. */
	if (unlikely(adios_rq(rq)->flags & ADIOS_RQ_FLIGHT))
		adios_requeue_request(rq);
}

/*
 * Setup
 */
static void adios_free(struct adios_data *ad)
{
	for (int op = 0; op < ADIOS_OPS; op++) {
		for (int k = 0; k < 2; k++) {
			free_percpu(ad->model[op][k].total);
			free_percpu(ad->model[op][k].seen);
		}
	}
	kvfree(ad);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 4)
static int adios_init_sched(struct request_queue *q, struct elevator_queue *eq)
#else
static int adios_init_sched(struct request_queue *q, struct elevator_type *e)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 4)
	struct elevator_queue *eq;
#endif
	struct adios_data *ad;
	int op;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 4)
	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;
#endif

	ad = kvzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad)
		goto put_eq;

	for (op = 0; op < ADIOS_OPS; op++) {
		ad->target[op] = adios_target_default[op];
		for (int k = 0; k < 2; k++) {
			ad->model[op][k].total = alloc_percpu(struct adios_sums);
			ad->model[op][k].seen = alloc_percpu(struct adios_sums);
			if (!ad->model[op][k].total || !ad->model[op][k].seen)
				goto free_ad;
		}
	}

	spin_lock_init(&ad->lock);
	INIT_LIST_HEAD(&ad->head);
	for (int cls = 0; cls < ADIOS_CLASSES; cls++) {
		ad->active[cls] = RB_ROOT_CACHED;
		for (int i = 0; i < ADIOS_FLOWS; i++) {
			ad->flows[cls][i].rqs = RB_ROOT_CACHED;
			ad->flows[cls][i].cls = cls;
			ad->flows[cls][i].weight =
				IOPRIO_NR_LEVELS - IOPRIO_BE_NORM;
		}
	}

	ad->q = q;
	ad->rotational = !!(q->limits.features & BLK_FEAT_ROTATIONAL);
	ad->window = adios_window_default[ad->rotational];
	ad->read_priority = ADIOS_READ_PRIORITY;
	ad->read_charge = adios_prio_to_wmult[ADIOS_READ_PRIORITY + 20];
	ad->async_depth = q->nr_requests;

	hrtimer_setup(&ad->wait_timer, adios_wait_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	timer_setup(&ad->fit_timer, adios_fit_timer, 0);

	/* One dispatcher for the whole queue, not one per hardware queue. */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);
	blk_stat_enable_accounting(q);

	eq->elevator_data = ad;
	q->elevator = eq;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	q->async_depth = adios_default_async_depth(q);
#endif
	adios_set_depth(q);
	return 0;

free_ad:
	adios_free(ad);
put_eq:
	/* Since 6.16.4 blk_mq_init_sched() puts the elevator_queue itself. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 4)
	kobject_put(&eq->kobj);
#endif
	return -ENOMEM;
}

static void adios_exit_sched(struct elevator_queue *e)
{
	struct adios_data *ad = e->elevator_data;

	hrtimer_cancel(&ad->wait_timer);
	timer_shutdown_sync(&ad->fit_timer);
	blk_stat_disable_accounting(ad->q);

	WARN_ON_ONCE(ad->nr_queued || !list_empty(&ad->head));

	adios_free(ad);
}

/*
 * sysfs
 */
#define ADIOS_SHOW(name, fmt, expr)					\
static ssize_t adios_##name##_show(struct elevator_queue *e, char *page)	\
{									\
	struct adios_data *ad = e->elevator_data;				\
	return sysfs_emit(page, fmt "\n", expr);			\
}

#define ADIOS_STORE(name, field, lo, hi)					\
static ssize_t adios_##name##_store(struct elevator_queue *e,		\
				 const char *page, size_t count)	\
{									\
	struct adios_data *ad = e->elevator_data;				\
	u64 v;								\
	int ret = kstrtou64(page, 10, &v);				\
									\
	if (ret)							\
		return ret;						\
	if (v < (lo) || v > (hi))					\
		return -EINVAL;						\
	ad->field = v;							\
	return count;							\
}

ADIOS_SHOW(window, "%llu", ad->window)
ADIOS_STORE(window, window, NSEC_PER_USEC, 10ULL * NSEC_PER_SEC)
ADIOS_SHOW(read_priority, "%d", ad->read_priority)

static ssize_t adios_read_priority_store(struct elevator_queue *e,
					 const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long flags;
	int prio, ret = kstrtoint(page, 10, &prio);

	if (ret)
		return ret;
	if (prio < -20 || prio > 19)
		return -EINVAL;
	spin_lock_irqsave(&ad->lock, flags);
	ad->read_priority = prio;
	ad->read_charge = adios_prio_to_wmult[prio + 20];
	spin_unlock_irqrestore(&ad->lock, flags);
	return count;
}
ADIOS_SHOW(target_read, "%llu", ad->target[ADIOS_READ])
ADIOS_STORE(target_read, target[ADIOS_READ], 0, 60ULL * NSEC_PER_SEC)
ADIOS_SHOW(target_write, "%llu", ad->target[ADIOS_WRITE])
ADIOS_STORE(target_write, target[ADIOS_WRITE], 0, 60ULL * NSEC_PER_SEC)
ADIOS_SHOW(target_discard, "%llu", ad->target[ADIOS_DISCARD])
ADIOS_STORE(target_discard, target[ADIOS_DISCARD], 0, 60ULL * NSEC_PER_SEC)

static ssize_t adios_adios_version_show(struct elevator_queue *e, char *page)
{
	return sysfs_emit(page, "%s\n", ADIOS_VERSION);
}

static ssize_t adios_model_show(struct elevator_queue *e, char *page)
{
	static const char * const name[ADIOS_OPS] = {
		"read", "write", "discard", "other",
	};
	struct adios_data *ad = e->elevator_data;
	ssize_t len = 0;

	for (int op = 0; op < ADIOS_OPS; op++) {
		for (int k = 0; k < 2; k++) {
			const struct adios_model *m = &ad->model[op][k];

			len += sysfs_emit_at(page, len,
					     "%-7s %-5s base %llu ns, slope %llu ns/page%s\n",
					     name[op], k ? "among" : "alone",
					     m->base, m->slope >> 8,
					     !m->sums.n ? " (no samples)" :
					     m->lenient ? " (disturbed)" : "");
		}
	}
	for (int op = 0; op < ADIOS_OPS; op++) {
		if (!ad->model[op][0].sums.n && !ad->model[op][1].sums.n)
			continue;
		len += sysfs_emit_at(page, len, "%-7s level %lld ns, seek",
				     name[op], ad->level[op]);
		for (int b = 0; b < ADIOS_SEEKS; b++)
			len += sysfs_emit_at(page, len, " %lld", ad->seek[op][b]);
		len += sysfs_emit_at(page, len, " ns\n");
	}
	len += sysfs_emit_at(page, len,
			     "after a read: a read loses %lld ns, a write %lld ns\n"
			     "after a write: a read loses %lld ns, a write %lld ns\n"
			     "a read costs others %lld ns, anything else %lld ns\n",
			     ad->excess[0][0], ad->excess[0][1],
			     ad->excess[1][0], ad->excess[1][1],
			     max_t(s64, ad->induced[0], 0),
			     max_t(s64, ad->induced[1], 0));
	return len;
}

static ssize_t adios_stats_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;
	struct adios_stats *s = &ad->stats;

	return sysfs_emit(page,
			  "dispatched %llu\nheld_window %llu\nheld_wait %llu\n"
			  "wait_hit %llu\nwait_miss %llu\naged %llu\n"
			  "queued %u\nflight %u\nflight_cost %llu\n"
			  "busy %u %u %u\n",
			  s->dispatched, s->held_window, s->held_wait,
			  s->wait_hit, s->wait_miss, s->aged,
			  ad->nr_queued, ad->flight, ad->flight_cost,
			  ad->busy[ADIOS_RT], ad->busy[ADIOS_BE], ad->busy[ADIOS_IDLE]);
}

/* Every flow that holds requests or was seen recently, one per line. */
static ssize_t adios_flows_show(struct elevator_queue *e, char *page)
{
	static const char * const name[ADIOS_CLASSES] = { "rt", "be", "idle" };
	struct adios_data *ad = e->elevator_data;
	unsigned long flags;
	ssize_t len = 0;

	spin_lock_irqsave(&ad->lock, flags);
	for (int cls = 0; cls < ADIOS_CLASSES; cls++) {
		for (int i = 0; i < ADIOS_FLOWS; i++) {
			const struct adios_flow *f = &ad->flows[cls][i];

			if (!f->queued && !f->flight && !f->idle_since)
				continue;
			len += sysfs_emit_at(page, len,
					     "%-4s %2d weight %u queued %u flight %u behind %lld think %u predicted/measured %llu/1000%s\n",
					     name[cls], i, f->weight, f->queued,
					     f->flight,
					     (s64)(ad->vclock[cls] - f->vtime),
					     f->think,
					     f->measured ?
					     div64_u64(f->predicted * 1000,
						       f->measured) : 0,
					     f == ad->wait_for ? " waited-for" : "");
		}
	}
	spin_unlock_irqrestore(&ad->lock, flags);
	return len;
}

#define ADIOS_ATTR_RW(name) \
	__ATTR(name, 0644, adios_##name##_show, adios_##name##_store)
#define ADIOS_ATTR_RO(name) __ATTR(name, 0444, adios_##name##_show, NULL)

static struct elv_fs_entry adios_attrs[] = {
	ADIOS_ATTR_RW(window),
	ADIOS_ATTR_RW(read_priority),
	ADIOS_ATTR_RW(target_read),
	ADIOS_ATTR_RW(target_write),
	ADIOS_ATTR_RW(target_discard),
	ADIOS_ATTR_RO(model),
	ADIOS_ATTR_RO(stats),
	ADIOS_ATTR_RO(flows),
	ADIOS_ATTR_RO(adios_version),
	__ATTR_NULL
};

static struct elevator_type adios_sched = {
	.ops = {
		.init_sched		= adios_init_sched,
		.exit_sched		= adios_exit_sched,
		.depth_updated		= adios_depth_updated,
		.limit_depth		= adios_limit_depth,
		.allow_merge		= adios_allow_merge,
		.bio_merge		= adios_bio_merge,
		.request_merged		= adios_request_merged,
		.requests_merged	= adios_requests_merged,
		.prepare_request	= adios_prepare_request,
		.insert_requests	= adios_insert_requests,
		.dispatch_request	= adios_dispatch_request,
		.has_work		= adios_has_work,
		.completed_request	= adios_completed_request,
		.requeue_request	= adios_requeue_request,
		.finish_request		= adios_finish_request,
	},
	.elevator_attrs = adios_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-adios-iosched");

static int __init adios_init(void)
{
	int ret;

	pr_info("Adaptive Deadline I/O Scheduler %s by Masahito Suzuki\n",
		ADIOS_VERSION);
	adios_policy_register();
	ret = elv_register(&adios_sched);
	if (ret)
		adios_policy_unregister();
	return ret;
}

static void __exit adios_exit(void)
{
	elv_unregister(&adios_sched);
	adios_policy_unregister();
}

module_init(adios_init);
module_exit(adios_exit);

MODULE_AUTHOR("Masahito Suzuki");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Adaptive Deadline I/O Scheduler");
