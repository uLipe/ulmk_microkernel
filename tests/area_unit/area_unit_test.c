/* SPDX-License-Identifier: MIT */
/*
 * Area set unit tests — tests/area_unit/area_unit_test.c
 *
 * Exercises kernel/mem/area.c on the host against the real TLSF heap:
 * disjointness, lookup, window containment for both MPU encodings
 * (power-of-two NAPOT/PMSAv7 and granule base/limit) and the derivation
 * tree behind cascading revocation.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ulmk/microkernel.h>
#include <kernel/include/ulmk_area_internal.h>
#include <kernel/include/ulmk_mem_internal.h>

static int g_pass;
static int g_fail;

#define CHECK(cond, msg) \
	do { \
		if (cond) { \
			g_pass++; \
		} else { \
			printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); \
			g_fail++; \
		} \
	} while (0)

static uint8_t pool_buf[32768] __attribute__((aligned(64)));

#define MAX_DROPS 16
static struct ulmk_area_set *g_dropped[MAX_DROPS];
static unsigned g_ndropped;

void ulmk_kern_area_dropped(struct ulmk_area_set *s)
{
	if (g_ndropped < MAX_DROPS)
		g_dropped[g_ndropped] = s;
	g_ndropped++;
}

static unsigned g_nretired;

void ulmk_kern_area_retire(struct ulmk_area *a)
{
	g_nretired++;
	ulmk_heap_free((void *)a->base);
	ulmk_heap_free(a);
}

static bool dropped(const struct ulmk_area_set *s)
{
	unsigned i;

	for (i = 0u; i < g_ndropped && i < MAX_DROPS; i++) {
		if (g_dropped[i] == s)
			return true;
	}
	return false;
}

static void reset(void)
{
	ulmk_heap_init((uintptr_t)pool_buf, sizeof(pool_buf));
	g_ndropped = 0u;
}

#define RW (ULMK_PERM_READ | ULMK_PERM_WRITE)
#define RO ULMK_PERM_READ

static void test_insert_sorted_disjoint(void)
{
	struct ulmk_area_set s;
	struct ulmk_area *a;
	int rc;

	printf("insert: sorted, disjoint\n");
	reset();
	ulmk_area_set_init(&s);

	CHECK(ulmk_area_insert(&s, 0x2000, 0x100, RW, ULMK_REGION_HEAP, 0u,
			       NULL, NULL) == ULMK_OK, "insert middle");
	CHECK(ulmk_area_insert(&s, 0x1000, 0x100, RW, ULMK_REGION_HEAP, 0u,
			       NULL, NULL) == ULMK_OK, "insert low");
	CHECK(ulmk_area_insert(&s, 0x3000, 0x100, RW, ULMK_REGION_HEAP, 0u,
			       NULL, NULL) == ULMK_OK, "insert high");
	CHECK(s.count == 3u, "count 3");
	CHECK(s.head->base == 0x1000 && s.head->next->base == 0x2000 &&
	      s.head->next->next->base == 0x3000, "list sorted by base");

	rc = ulmk_area_insert(&s, 0x2000, 0x100, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "exact duplicate rejected");
	rc = ulmk_area_insert(&s, 0x1F80, 0x100, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "overlap with next rejected");
	rc = ulmk_area_insert(&s, 0x20C0, 0x100, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "overlap with previous rejected");
	rc = ulmk_area_insert(&s, 0x0F00, 0x3000, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "superset rejected");
	rc = ulmk_area_insert(&s, 0x2040, 0x40, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "subset rejected");
	rc = ulmk_area_insert(&s, 0x2100, 0x100, RW, 0, 0u, NULL, &a);
	CHECK(rc == ULMK_OK && a->base == 0x2100, "adjacent accepted");
	rc = ulmk_area_insert(&s, 0x4000, 0, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "zero size rejected");
	rc = ulmk_area_insert(&s, (uintptr_t)-64, 0x100, RW, 0, 0u, NULL, NULL);
	CHECK(rc == ULMK_EINVAL, "wrapping range rejected");
	CHECK(s.count == 4u, "failed inserts leave count alone");

	ulmk_area_set_release(&s);
	CHECK(s.count == 0u && !s.head, "release empties the set");
}

static void test_find(void)
{
	struct ulmk_area_set s;

	printf("find / find_base / range_ok\n");
	reset();
	ulmk_area_set_init(&s);
	ulmk_area_insert(&s, 0x1000, 0x100, RW, 0, 0u, NULL, NULL);
	ulmk_area_insert(&s, 0x2000, 0x100, RO, 0, 0u, NULL, NULL);

	CHECK(ulmk_area_find(&s, 0x0FFF) == NULL, "below first");
	CHECK(ulmk_area_find(&s, 0x1000)->base == 0x1000, "first byte");
	CHECK(ulmk_area_find(&s, 0x10FF)->base == 0x1000, "last byte");
	CHECK(ulmk_area_find(&s, 0x1100) == NULL, "one past end");
	CHECK(ulmk_area_find(&s, 0x2080)->base == 0x2000, "second area");
	CHECK(ulmk_area_find(&s, 0x9000) == NULL, "above all");

	CHECK(ulmk_area_find_base(&s, 0x2000) != NULL, "find_base hit");
	CHECK(ulmk_area_find_base(&s, 0x2040) == NULL, "find_base interior miss");

	CHECK(ulmk_area_range_ok(&s, 0x1000, 0x100, RW), "whole area RW");
	CHECK(!ulmk_area_range_ok(&s, 0x1080, 0x81, RW), "run past the end");
	CHECK(!ulmk_area_range_ok(&s, 0x2000, 4, RW), "write into RO area");
	CHECK(ulmk_area_range_ok(&s, 0x2000, 4, RO), "read RO area");
	CHECK(!ulmk_area_range_ok(&s, 0x10F0, 0xF20, RO),
	      "range spanning a hole");
	CHECK(ulmk_area_range_ok(&s, 0x5000, 0, RW), "empty range");
	ulmk_area_set_release(&s);
}

static unsigned rnd(unsigned n)
{
	return (unsigned)rand() % n;
}

/*
 * Property: for any area and any address in it the window holds the
 * address, never leaves the area, is encodable, and (pow2) is maximal.
 */
static void test_window_property(void)
{
	struct ulmk_area a;
	ulmk_arch_region_t w;
	unsigned it;
	unsigned bad = 0u;
	unsigned bad_max = 0u;

	printf("window containment (%s, min=%u)\n",
	       ULMK_ARCH_MPU_WIN_POW2 ? "pow2" : "granule",
	       (unsigned)ULMK_ARCH_MPU_WIN_MIN);
	srand(1234);
	memset(&a, 0, sizeof(a));
	a.perms = RW;

	for (it = 0u; it < 200000u; it++) {
		uintptr_t addr;

		a.base = 0x80000000u + (uintptr_t)rnd(1u << 16) * 64u;
		a.size = (size_t)(1u + rnd(512)) * 64u;
		addr   = a.base + rnd((unsigned)a.size);

		if (ulmk_area_window(&a, addr, &w) != ULMK_OK) {
			bad++;
			continue;
		}
		if (addr < w.base || addr >= w.base + w.size ||
		    w.base < a.base || w.base + w.size > a.base + a.size ||
		    w.size < ULMK_ARCH_MPU_WIN_MIN ||
		    (w.base & (ULMK_ARCH_MPU_WIN_MIN - 1u)) ||
		    (w.size & (ULMK_ARCH_MPU_WIN_MIN - 1u))) {
			bad++;
			continue;
		}
#if ULMK_ARCH_MPU_WIN_POW2
		if ((w.size & (w.size - 1u)) || (w.base & (w.size - 1u))) {
			bad++;
			continue;
		}
		{
			size_t    d  = w.size << 1;
			uintptr_t db = addr & ~(uintptr_t)(d - 1u);

			if (db >= a.base && db + d <= a.base + a.size)
				bad_max++;
		}
#else
		if (w.base != a.base || w.size != a.size)
			bad++;
#endif
	}
	CHECK(bad == 0u, "every window inside its area and encodable");
	CHECK(bad_max == 0u, "pow2 window cannot be doubled");
}

static void test_window_edges(void)
{
	struct ulmk_area a;
	ulmk_arch_region_t w;

	printf("window edges\n");
	memset(&a, 0, sizeof(a));
	a.perms = RW;
	a.type  = ULMK_REGION_HEAP;

	a.base = 0x1000;
	a.size = 0x100;
	CHECK(ulmk_area_window(&a, 0x0FFF, &w) == ULMK_EINVAL, "addr below");
	CHECK(ulmk_area_window(&a, 0x1100, &w) == ULMK_EINVAL, "addr past");
	CHECK(ulmk_area_window(&a, 0x1040, &w) == ULMK_OK &&
	      w.base == 0x1000 && w.size == 0x100 && w.perms == RW &&
	      w.type == ULMK_REGION_HEAP, "aligned pow2 area is one window");

	/* Heap block that is 64-aligned but straddles a 128 boundary. */
	a.base = 0x10C0;
	a.size = 0xC0;
	CHECK(ulmk_area_window(&a, 0x10C0, &w) == ULMK_OK &&
	      w.base >= 0x10C0 && w.base + w.size <= 0x1180,
	      "unaligned block stays inside");

	a.base = 0x1000 + ULMK_ARCH_MPU_WIN_MIN / 2u;
	a.size = ULMK_ARCH_MPU_WIN_MIN / 2u;
	CHECK(ulmk_area_window(&a, a.base, &w) == ULMK_EINVAL,
	      "area below the minimum granule");
}

static void test_revoke_cascade(void)
{
	struct ulmk_area_set s1;
	struct ulmk_area_set s2;
	struct ulmk_area_set s3;
	struct ulmk_area *a;
	struct ulmk_area *b;
	size_t free0;
	void *blk;

	printf("revoke cascades through the derivation tree\n");
	reset();
	free0 = ulmk_heap_free_bytes();
	ulmk_area_set_init(&s1);
	ulmk_area_set_init(&s2);
	ulmk_area_set_init(&s3);

	blk = ulmk_heap_alloc(256);
	ulmk_area_insert(&s1, (uintptr_t)blk, 256, RW, ULMK_REGION_HEAP,
			 ULMK_AREA_BACKED, NULL, &a);
	ulmk_area_insert(&s2, a->base, a->size, RO, ULMK_REGION_GRANT, 0u,
			 a, &b);
	ulmk_area_insert(&s3, b->base, b->size, RO, ULMK_REGION_GRANT, 0u,
			 b, NULL);
	CHECK(a->child == b && b->parent == a, "tree linked");

	g_ndropped = 0u;
	g_nretired = 0u;
	ulmk_area_revoke(a);
	CHECK(s1.count == 0u && s2.count == 0u && s3.count == 0u,
	      "origin and every descendant gone");
	CHECK(dropped(&s1) && dropped(&s2) && dropped(&s3),
	      "every holder told to drop its windows");
	CHECK(g_nretired == 1u, "backing handed to the kernel, not freed inline");
	CHECK(ulmk_heap_free_bytes() == free0, "backing and records freed");
}

static void test_revoke_middle(void)
{
	struct ulmk_area_set s1;
	struct ulmk_area_set s2;
	struct ulmk_area_set s3;
	struct ulmk_area_set s4;
	struct ulmk_area *a;
	struct ulmk_area *b;
	size_t free0;
	size_t free1;
	void *blk;

	printf("revoking one grant keeps its siblings\n");
	reset();
	free0 = ulmk_heap_free_bytes();
	ulmk_area_set_init(&s1);
	ulmk_area_set_init(&s2);
	ulmk_area_set_init(&s3);
	ulmk_area_set_init(&s4);

	blk = ulmk_heap_alloc(128);
	ulmk_area_insert(&s1, (uintptr_t)blk, 128, RW, ULMK_REGION_HEAP,
			 ULMK_AREA_BACKED, NULL, &a);
	ulmk_area_insert(&s2, a->base, 128, RO, ULMK_REGION_GRANT, 0u, a, &b);
	ulmk_area_insert(&s3, b->base, 128, RO, ULMK_REGION_GRANT, 0u, b, NULL);
	ulmk_area_insert(&s4, a->base, 128, RO, ULMK_REGION_GRANT, 0u, a, NULL);
	free1 = ulmk_heap_free_bytes();

	ulmk_area_revoke(b);
	CHECK(s1.count == 1u && s4.count == 1u, "origin and sibling kept");
	CHECK(s2.count == 0u && s3.count == 0u, "revoked grant and its child");
	CHECK(a->child && a->child->set == &s4 && !a->child->sibling,
	      "parent child list relinked");
	CHECK(ulmk_heap_free_bytes() > free1, "records returned");

	ulmk_area_revoke(a);
	CHECK(s4.count == 0u && ulmk_heap_free_bytes() == free0,
	      "all freed after origin revoke");
}

static void test_holder_death(void)
{
	struct ulmk_area_set s1;
	struct ulmk_area_set s2;
	struct ulmk_area_set s3;
	struct ulmk_area *a;
	struct ulmk_area *b;
	size_t free0;
	size_t free_orphan;
	void *blk;

	printf("holder death orphans, last reference frees\n");
	reset();
	free0 = ulmk_heap_free_bytes();
	ulmk_area_set_init(&s1);
	ulmk_area_set_init(&s2);
	ulmk_area_set_init(&s3);

	blk = ulmk_heap_alloc(512);
	ulmk_area_insert(&s1, (uintptr_t)blk, 512, RW, ULMK_REGION_HEAP,
			 ULMK_AREA_BACKED, NULL, &a);
	ulmk_area_insert(&s2, a->base, 512, RW, ULMK_REGION_GRANT, 0u, a, &b);
	ulmk_area_insert(&s3, b->base, 512, RO, ULMK_REGION_GRANT, 0u, b, NULL);

	/* Owner dies first: the block must outlive it for its grantees. */
	ulmk_area_set_release(&s1);
	free_orphan = ulmk_heap_free_bytes();
	CHECK(s1.count == 0u && a->set == NULL, "origin orphaned");
	CHECK(s2.count == 1u && s3.count == 1u, "grants keep their range");
	CHECK(ulmk_area_find(&s3, (uintptr_t)blk + 8) != NULL,
	      "grandchild still resolves");

	/* Middle holder dies: its record lives on for the grandchild. */
	ulmk_area_set_release(&s2);
	CHECK(b->set == NULL && s3.count == 1u, "middle orphaned");
	CHECK(ulmk_heap_free_bytes() == free_orphan, "nothing freed yet");

	/* Last reference: the whole chain and the backing go. */
	ulmk_area_set_release(&s3);
	CHECK(ulmk_heap_free_bytes() == free0, "chain and backing freed");
}

static void test_inherit(void)
{
	struct ulmk_area_set parent;
	struct ulmk_area_set child;
	struct ulmk_area *a;
	void *b1;
	void *b2;
	size_t free0;

	printf("inherit derives every parent area\n");
	reset();
	free0 = ulmk_heap_free_bytes();
	ulmk_area_set_init(&parent);
	ulmk_area_set_init(&child);

	b1 = ulmk_heap_alloc(64);
	b2 = ulmk_heap_alloc(128);
	ulmk_area_insert(&parent, (uintptr_t)b1, 64, RW, ULMK_REGION_HEAP,
			 ULMK_AREA_BACKED, NULL, &a);
	ulmk_area_insert(&parent, (uintptr_t)b2, 128, RO, ULMK_REGION_HEAP,
			 ULMK_AREA_BACKED, NULL, NULL);

	CHECK(ulmk_area_inherit(&child, &parent) == ULMK_OK, "inherit ok");
	CHECK(child.count == 2u, "child holds both");
	CHECK(ulmk_area_find(&child, (uintptr_t)b2)->perms == RO,
	      "perms carried over");
	CHECK(ulmk_area_find(&child, (uintptr_t)b1)->parent == a,
	      "derived from the parent's record");
	CHECK(!(ulmk_area_find(&child, (uintptr_t)b1)->flags &
		ULMK_AREA_BACKED), "alias never owns the block");

	ulmk_area_revoke(a);
	CHECK(child.count == 1u && !ulmk_area_find(&child, (uintptr_t)b1),
	      "revoking the parent's area reaches the child");

	ulmk_area_set_release(&parent);
	ulmk_area_set_release(&child);
	CHECK(ulmk_heap_free_bytes() == free0, "no leak");
}

int main(void)
{
	test_insert_sorted_disjoint();
	test_find();
	test_window_property();
	test_window_edges();
	test_revoke_cascade();
	test_revoke_middle();
	test_holder_death();
	test_inherit();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
