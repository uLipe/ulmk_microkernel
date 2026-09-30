/* SPDX-License-Identifier: MIT */
/*
 * Memory allocator unit tests — tests/mem_unit/mem_unit_test.c
 *
 * Tests the TLSF allocator in kernel/mem/tlsf.c.
 * Runs on the host (cc), no TriCore toolchain required.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "../../kernel/include/ulmk_mem_internal.h"

static int g_pass;
static int g_fail;

#define CHECK(cond, msg) \
	do { \
		if (cond) { \
			printf("  [PASS] %s\n", msg); \
			g_pass++; \
		} else { \
			printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); \
			g_fail++; \
		} \
	} while (0)

/* Pool: 16 KiB — must be large enough for TLSF overhead + test allocs. */
static uint8_t pool_buf[16384] __attribute__((aligned(64)));

static void reset_pool(void)
{
	memset(pool_buf, 0xCC, sizeof(pool_buf));
	ulmk_heap_init((uintptr_t)pool_buf, sizeof(pool_buf));
}

static void test_basic_alloc(void)
{
	void *p;

	printf("test_basic_alloc\n");
	reset_pool();

	p = ulmk_heap_alloc(64);
	CHECK(p != NULL, "alloc 64 bytes returns non-NULL");
	CHECK(((uintptr_t)p % 64) == 0, "returned pointer is 64-byte aligned");

	p = ulmk_heap_alloc(128);
	CHECK(p != NULL, "alloc 128 bytes returns non-NULL");
	CHECK(((uintptr_t)p % 64) == 0, "128-byte alloc is 64-byte aligned");
}

static void test_alloc_write(void)
{
	uint8_t *p;
	size_t   i;

	printf("test_alloc_write\n");
	reset_pool();

	p = (uint8_t *)ulmk_heap_alloc(256);
	CHECK(p != NULL, "alloc 256 bytes");

	for (i = 0; i < 256; i++)
		p[i] = (uint8_t)i;

	for (i = 0; i < 256; i++) {
		if (p[i] != (uint8_t)i) {
			CHECK(0, "write-then-read pattern correct");
			return;
		}
	}
	CHECK(1, "write-then-read pattern correct");
}

static void test_free_and_realloc(void)
{
	void *p1;
	void *p2;

	printf("test_free_and_realloc\n");
	reset_pool();

	p1 = ulmk_heap_alloc(64);
	CHECK(p1 != NULL, "initial alloc succeeds");

	ulmk_heap_free(p1);

	p2 = ulmk_heap_alloc(64);
	CHECK(p2 != NULL, "alloc after free succeeds");
	(void)p2;
}

static void test_coalescing(void)
{
	void   *p1;
	void   *p2;
	void   *p3;
	void   *big;
	size_t  free_before;
	size_t  free_after;

	printf("test_coalescing\n");
	reset_pool();

	p1 = ulmk_heap_alloc(64);
	p2 = ulmk_heap_alloc(64);
	p3 = ulmk_heap_alloc(64);
	CHECK(p1 && p2 && p3, "three 64-byte allocs succeed");

	free_before = ulmk_heap_free_bytes();
	ulmk_heap_free(p1);
	ulmk_heap_free(p2);
	ulmk_heap_free(p3);
	free_after = ulmk_heap_free_bytes();

	CHECK(free_after > free_before, "free bytes increased after releasing blocks");

	big = ulmk_heap_alloc(128);
	CHECK(big != NULL, "alloc of larger region succeeds after coalescing");
	(void)big;
}

static void test_exhaustion(void)
{
	void   *p;
	size_t  free_start;
	size_t  allocated;

	printf("test_exhaustion\n");
	reset_pool();

	free_start = ulmk_heap_free_bytes();
	allocated  = 0u;

	while ((p = ulmk_heap_alloc(64)) != NULL)
		allocated += 64u;

	CHECK(allocated > 0u, "allocated at least one block before exhaustion");
	CHECK(allocated <= free_start, "allocated no more than pool capacity");

	p = ulmk_heap_alloc(64);
	CHECK(p == NULL, "alloc returns NULL when pool is exhausted");
}

static void test_null_free(void)
{
	printf("test_null_free\n");
	reset_pool();

	ulmk_heap_free(NULL);
	CHECK(1, "free(NULL) does not crash");
}

static void test_multiple_sizes(void)
{
	void  *ptrs[8];
	size_t sizes[8] = {64, 128, 256, 64, 512, 64, 128, 64};
	int    i;

	printf("test_multiple_sizes\n");
	reset_pool();

	for (i = 0; i < 8; i++) {
		ptrs[i] = ulmk_heap_alloc(sizes[i]);
		CHECK(ptrs[i] != NULL, "alloc of varying size succeeds");
	}

	for (i = 0; i < 8; i += 2)
		ulmk_heap_free(ptrs[i]);

	for (i = 0; i < 4; i++) {
		void *p = ulmk_heap_alloc(64);
		CHECK(p != NULL, "realloc into freed holes succeeds");
		(void)p;
	}
}

static void test_alignment_varied_sizes(void)
{
	int   odd_sizes[] = {1, 7, 33, 65, 100, 63};
	int   n = (int)(sizeof(odd_sizes) / sizeof(odd_sizes[0]));
	void *p;
	int   i;

	printf("test_alignment_varied_sizes\n");
	reset_pool();

	for (i = 0; i < n; i++) {
		p = ulmk_heap_alloc((size_t)odd_sizes[i]);
		CHECK(p != NULL, "alloc of non-multiple-of-64 size");
		CHECK(((uintptr_t)p % 64) == 0,
		      "result is 64-byte aligned for odd size");
		(void)p;
	}
}

static void test_aligned_alloc(void)
{
	void *p;

	printf("test_aligned_alloc\n");
	reset_pool();

	p = ulmk_heap_aligned_alloc(128, 64);
	CHECK(p != NULL, "aligned_alloc(128, 64) returns non-NULL");
	CHECK(((uintptr_t)p % 128) == 0, "result is 128-byte aligned");
	ulmk_heap_free(p);
}

/*
 * Power-of-two MPUs pin a thread stack as one naturally aligned window, so
 * stacks come from aligned_alloc(size, size).  The carve must keep the block
 * chain intact: the tail past the aligned payload goes back to the pool and
 * freeing everything restores the whole heap.
 */
static void test_aligned_alloc_stack_pattern(void)
{
	size_t free0;
	void  *s[4];
	void  *small[4];
	int    i;
	int    ok = 1;

	printf("test_aligned_alloc_stack_pattern\n");
	reset_pool();
	free0 = ulmk_heap_free_bytes();

	for (i = 0; i < 4; i++) {
		small[i] = ulmk_heap_alloc(64u + (size_t)i * 64u);
		s[i] = ulmk_heap_aligned_alloc(1024u, 1024u);
		if (!s[i] || ((uintptr_t)s[i] & 1023u))
			ok = 0;
	}
	CHECK(ok, "four 1 KiB stacks, each 1 KiB aligned");
	if (!ok)
		return;

	for (i = 0; i < 4; i++)
		memset(s[i], 0xA5, 1024u);
	for (i = 0; i < 4; i++) {
		ulmk_heap_free(s[i]);
		ulmk_heap_free(small[i]);
	}
	CHECK(ulmk_heap_free_bytes() == free0, "all bytes back after free");

	/* Good-fit search rounds up a sub-class; 3/4 of the pool still needs
	 * the carved pieces merged back. */
	s[0] = ulmk_heap_alloc(free0 / 4u * 3u);
	CHECK(s[0] != NULL, "pool coalesced back into one block");
	ulmk_heap_free(s[0]);
}

int main(void)
{
	printf("=== mem_unit: TLSF allocator tests ===\n");

	test_basic_alloc();
	test_alloc_write();
	test_free_and_realloc();
	test_coalescing();
	test_exhaustion();
	test_null_free();
	test_multiple_sizes();
	test_alignment_varied_sizes();
	test_aligned_alloc();
	test_aligned_alloc_stack_pattern();

	printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
	return (g_fail == 0) ? 0 : 1;
}
