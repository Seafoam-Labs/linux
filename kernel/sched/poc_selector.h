/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Piece-Of-Cake (POC) CPU Selector: declarations and inline helpers shared
 * by the scheduler core.
 *
 * Included from kernel/sched/sched.h.  Like poc_selector.c, this file is
 * meant to be identical on every kernel POC is ported to; what differs per
 * kernel version lives in the hook sites.
 */
#ifndef _KERNEL_SCHED_POC_SELECTOR_H
#define _KERNEL_SCHED_POC_SELECTOR_H

#ifdef CONFIG_SCHED_POC_SELECTOR
extern struct static_key_true poc_selector_active;
#ifdef CONFIG_SCHED_CLASS_EXT
extern void poc_notify_scx(bool scx_active);
extern void poc_check_skip_fallback(void);
#else
static inline void poc_check_skip_fallback(void) {}
#endif
extern struct static_key_true sched_poc_aligned;
extern struct static_key_true sched_poc_smt_consecutive;
extern struct static_key_true sched_poc_smt_uniform;
extern struct static_key_false sched_poc_target_sticky;
extern struct static_key_true sched_poc_packed;
extern void __set_cpu_idle_state_poc(int cpu, int state);
extern struct static_key_true sched_poc_waker_yield;
void poc_aggregate(struct sched_domain_shared *sd_share);
void poc_commit_cpu(int cpu);
extern unsigned int poc_pull_budget;
extern void poc_sd_shared_init(struct sched_domain *sd, int sd_id);
static __always_inline void set_cpu_idle_state_poc(int cpu, int state)
{
	if (static_branch_likely(&poc_selector_active) &&
	    !sched_asym_cpucap_active())
		__set_cpu_idle_state_poc(cpu, state);
}

/*
 * POC_CTZ64 - Count trailing zeros (find first set bit)
 *
 * Architecture-optimized CTZ for POC idle CPU selection.
 * Returns 64 for input 0 (important for BSF-based implementations).
 */
#if defined(__x86_64__) && defined(__BMI__)
/* Tier 1: x86-64 with BMI1 - TZCNT is zero-safe */
#define POC_CTZ64(v) ((int)__builtin_ctzll(v))

#elif defined(__aarch64__)
/* Tier 1: ARM64 - RBIT+CLZ is zero-safe */
#define POC_CTZ64(v) ((int)__builtin_ctzll(v))

#elif defined(__riscv) && defined(__riscv_zbb)
/* Tier 1: RISC-V with Zbb - CTZ is zero-safe */
#define POC_CTZ64(v) ((int)__builtin_ctzll(v))

#elif defined(__x86_64__)
/* Tier 2: x86-64 without BMI1 - BSF needs zero check */
static __always_inline int poc_ctz64_bsf(u64 v)
{
	if (unlikely(!v))
		return 64;
	return (int)__builtin_ctzll(v);
}
#define POC_CTZ64(v) poc_ctz64_bsf(v)

#else
/* Tier 3: De Bruijn fallback for other architectures */
#define POC_DEBRUIJN_CTZ64_CONST 0x03F79D71B4CA8B09ULL
static const u8 poc_debruijn_ctz64_tab[64] = {
	 0,  1, 56,  2, 57, 49, 28,  3,
	61, 58, 42, 50, 38, 29, 17,  4,
	62, 47, 59, 36, 45, 43, 51, 22,
	53, 39, 33, 30, 24, 18, 12,  5,
	63, 55, 48, 27, 60, 41, 37, 16,
	46, 35, 44, 21, 52, 32, 23, 11,
	54, 26, 40, 15, 34, 20, 31, 10,
	25, 14, 19,  9, 13,  8,  7,  6,
};
static __always_inline int poc_debruijn_ctz64(u64 v)
{
	u64 lsb;
	u32 idx;

	if (unlikely(!v))
		return 64;
	lsb = v & (-(s64)v);
	idx = (u32)((lsb * POC_DEBRUIJN_CTZ64_CONST) >> 58);
	return (int)poc_debruijn_ctz64_tab[idx & 63];
}
#define POC_CTZ64(v) poc_debruijn_ctz64(v)

#endif /* POC_CTZ64 */

/*
 * POC helper: convert cpumask region to POC-relative u64
 *
 * Extracts the 64-bit region of @mask corresponding to this LLC's
 * CPU range and shifts it to align with POC's bit positions.
 *
 * Used by load balancer functions that need to intersect cpumasks
 * with POC idle bitmaps.
 */
static __always_inline u64 poc_cpumask_to_u64(const struct cpumask *mask,
					      struct sched_domain_shared *sd_share)
{
	int base = sd_share->poc_cpu_base;
	int base_word = base >> 6;
	int shift = sd_share->poc_affinity_shift;

	if (likely(shift == 0)) {
		/*
		 * Fast path: this LLC's own base is 64-aligned, so no
		 * cross-word shift is needed here even if some other LLC
		 * in the system is misaligned (sched_poc_aligned is a
		 * single system-wide flag and must not gate this branch).
		 */
		return cpumask_bits(mask)[base_word];
	} else {
		/* Slow path: shift required (e.g., Threadripper) */
		u64 lo = cpumask_bits(mask)[base_word];
		u64 hi = (base_word + 1 < BITS_TO_LONGS(nr_cpumask_bits)) ?
			 cpumask_bits(mask)[base_word + 1] : 0;
		return (lo >> shift) | (hi << (64 - shift));
	}
}
#endif /* CONFIG_SCHED_POC_SELECTOR */

#endif /* _KERNEL_SCHED_POC_SELECTOR_H */
