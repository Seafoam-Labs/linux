// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"

extern __u64 bpf_ctz64(__u64 x) __ksym __attribute__((bpf_fastcall));
extern __u64 bpf_popcnt64(__u64 x) __ksym __attribute__((bpf_fastcall));
extern __u64 bpf_clz64(__u64 x) __ksym __attribute__((bpf_fastcall));
extern __u64 bpf_mulhi64(__u64 x, __u64 y) __ksym;

static __always_inline __u64 mulhi_ref(__u64 x, __u64 y)
{
	__u64 x0 = (__u32)x, x1 = x >> 32, y0 = (__u32)y, y1 = y >> 32;
	__u64 mid = x1 * y0 + ((x0 * y0) >> 32);
	__u64 carry = mid >> 32;

	asm volatile("" : "+r" (carry));
	mid = (__u32)mid + x0 * y1;
	return x1 * y1 + carry + (mid >> 32);
}

SEC("syscall")
__success __retval(0)
int scalar_bitops_boundaries(void *ctx)
{
	int i;

	if (bpf_ctz64(0) != 64 || bpf_popcnt64(0) != 0 ||
	    bpf_ctz64(~0ULL) != 0 || bpf_popcnt64(~0ULL) != 64 ||
	    bpf_clz64(0) != 64 || bpf_clz64(~0ULL) != 0 ||
	    bpf_mulhi64(0, ~0ULL) != 0 ||
	    bpf_mulhi64(~0ULL, ~0ULL) != ~0ULL - 1)
		return 1;
	for (i = 0; i < 64; i++) {
		__u64 bit = 1ULL << i;

		if (bpf_ctz64(bit) != i || bpf_popcnt64(bit) != 1 ||
		    bpf_popcnt64(~bit) != 63 || bpf_clz64(bit) != 63 - i ||
		    bpf_mulhi64(bit, ~0ULL) != bit - 1)
			return 2;
	}
	return 0;
}

SEC("syscall")
__success __retval(0)
int scalar_bitops_random(void *ctx)
{
	__u64 x = 0xd1b54a32d192ed03ULL;
	int i;

	for (i = 0; i < 1024; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		if (bpf_ctz64(x) != (x ? __builtin_ctzll(x) : 64) ||
		    bpf_popcnt64(x) != __builtin_popcountll(x) ||
		    bpf_clz64(x) != (x ? __builtin_clzll(x) : 64) ||
		    bpf_mulhi64(x, ~x) != mulhi_ref(x, ~x))
			return 1;
	}
	return 0;
}

/* Mutable map data prevents verifier folding of preservation checks. */
volatile __u64 fastcall_values[] = { 17, 29, 43, 61 };

SEC("syscall")
__success __retval(0)
__naked int scalar_bitops_preserve(void)
{
	asm volatile (
		"r1 = %[fastcall_values] ll;"
		"r6 = *(u64 *)(r1 + 0); r7 = *(u64 *)(r1 + 8);"
		"r8 = *(u64 *)(r1 + 16); r9 = *(u64 *)(r1 + 24);"
		"r1 = 256; call bpf_ctz64;"
		"if r0 != 8 goto fail_%=;"
		"if r6 != 17 goto fail_%=;"
		"if r7 != 29 goto fail_%=;"
		"if r8 != 43 goto fail_%=;"
		"if r9 != 61 goto fail_%=;"
		"r1 = -1; call bpf_popcnt64;"
		"if r0 != 64 goto fail_%=;"
		"if r6 != 17 goto fail_%=;"
		"if r7 != 29 goto fail_%=;"
		"if r8 != 43 goto fail_%=;"
		"if r9 != 61 goto fail_%=;"
		"r1 = 256; call bpf_clz64;"
		"if r0 != 55 goto fail_%=;"
		"if r6 != 17 goto fail_%=;"
		"if r7 != 29 goto fail_%=;"
		"if r8 != 43 goto fail_%=;"
		"if r9 != 61 goto fail_%=;"
		"r1 = -1; r2 = -1; call bpf_mulhi64;"
		"if r0 != -2 goto fail_%=;"
		"if r6 != 17 goto fail_%=;"
		"if r7 != 29 goto fail_%=;"
		"if r8 != 43 goto fail_%=;"
		"if r9 != 61 goto fail_%=;"
		"r0 = 0; exit;"
		"fail_%=: r0 = 1; exit;"
		: : __imm_addr(fastcall_values) : __clobber_all);
}

#define FASTCALL_TEST(name, function, argument, expected) \
SEC("syscall") \
__success __retval(0) \
__naked int name(void) \
{ \
	asm volatile ( \
		"r1 = %[fastcall_values] ll;" \
		"r2 = *(u64 *)(r1 + 0); r3 = *(u64 *)(r1 + 8);" \
		"r4 = *(u64 *)(r1 + 16); r5 = *(u64 *)(r1 + 24);" \
		"r1 = " #argument ";" \
		"*(u64 *)(r10 - 8) = r2; *(u64 *)(r10 - 16) = r3;" \
		"*(u64 *)(r10 - 24) = r4; *(u64 *)(r10 - 32) = r5;" \
		"call " #function ";" \
		"r5 = *(u64 *)(r10 - 32); r4 = *(u64 *)(r10 - 24);" \
		"r3 = *(u64 *)(r10 - 16); r2 = *(u64 *)(r10 - 8);" \
		"if r0 != " #expected " goto fail_%=;" \
		"if r2 != 17 goto fail_%=; if r3 != 29 goto fail_%=;" \
		"if r4 != 43 goto fail_%=; if r5 != 61 goto fail_%=;" \
		"r0 = 0; exit; fail_%=: r0 = 1; exit;" \
		: : __imm_addr(fastcall_values) : __clobber_all); \
}

FASTCALL_TEST(scalar_fastcall_ctz, bpf_ctz64, 256, 8)
FASTCALL_TEST(scalar_fastcall_popcnt, bpf_popcnt64, -1, 64)
FASTCALL_TEST(scalar_fastcall_clz, bpf_clz64, 256, 55)

SEC("syscall")
__failure __msg("R3 !read_ok")
__naked int scalar_mulhi_is_not_fastcall(void)
{
	asm volatile (
		"r1 = -1; r2 = -1; r3 = 17; call bpf_mulhi64;"
		"r0 = r3; exit;"
		::: __clobber_all);
}

char LICENSE[] SEC("license") = "GPL";
