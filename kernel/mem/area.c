/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Per-thread memory areas — kernel/mem/area.c
 *
 * Callers hold g_ulmk_lock_area.  Records come from the kernel heap, which
 * takes g_ulmk_lock_mem internally (area → mem lock order).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk_arch.h>
#include <kernel/include/ulmk_area_internal.h>
#include <kernel/include/ulmk_mem_internal.h>

void ulmk_area_set_init(struct ulmk_area_set *s)
{
	s->head  = NULL;
	s->count = 0u;
}

static bool overlaps(uintptr_t a, size_t alen, uintptr_t b, size_t blen)
{
	return a < b + blen && b < a + alen;
}

int ulmk_area_insert(struct ulmk_area_set *s, uintptr_t base, size_t size,
		     uint32_t perms, uint8_t type, uint8_t flags,
		     struct ulmk_area *parent, struct ulmk_area **out)
{
	struct ulmk_area **link;
	struct ulmk_area *a;

	if (!s || size == 0u || base + size < base)
		return ULMK_EINVAL;

	/*
	 * Areas held by one thread never overlap: the fault handler resolves
	 * an address to exactly one area, so its permissions are unambiguous.
	 */
	link = &s->head;
	while (*link && (*link)->base < base) {
		if (overlaps((*link)->base, (*link)->size, base, size))
			return ULMK_EINVAL;
		link = &(*link)->next;
	}
	if (*link && overlaps((*link)->base, (*link)->size, base, size))
		return ULMK_EINVAL;

	a = (struct ulmk_area *)ulmk_heap_alloc(sizeof(*a));
	if (!a)
		return ULMK_ENOMEM;

	a->base    = base;
	a->size    = size;
	a->perms   = perms;
	a->type    = type;
	a->flags   = flags;
	a->set     = s;
	a->child   = NULL;
	a->parent  = parent;
	a->sibling = NULL;
	if (parent) {
		a->sibling    = parent->child;
		parent->child = a;
	}
	a->next = *link;
	*link   = a;
	s->count++;

	if (out)
		*out = a;
	return ULMK_OK;
}

struct ulmk_area *ulmk_area_find(const struct ulmk_area_set *s, uintptr_t addr)
{
	struct ulmk_area *a;

	for (a = s->head; a && a->base <= addr; a = a->next) {
		if (addr - a->base < a->size)
			return a;
	}
	return NULL;
}

struct ulmk_area *ulmk_area_find_base(const struct ulmk_area_set *s,
				      uintptr_t base)
{
	struct ulmk_area *a;

	for (a = s->head; a && a->base <= base; a = a->next) {
		if (a->base == base)
			return a;
	}
	return NULL;
}

bool ulmk_area_range_ok(const struct ulmk_area_set *s, uintptr_t addr,
			size_t len, uint32_t perms)
{
	const struct ulmk_area *a;

	if (len == 0u)
		return true;
	a = ulmk_area_find(s, addr);
	if (!a || (a->perms & perms) != perms)
		return false;
	return len <= a->size - (addr - a->base);
}

static void set_unlink(struct ulmk_area *a)
{
	struct ulmk_area_set *s = a->set;
	struct ulmk_area **link;

	if (!s)
		return;
	for (link = &s->head; *link; link = &(*link)->next) {
		if (*link == a) {
			*link = a->next;
			s->count--;
			break;
		}
	}
	a->next = NULL;
	a->set  = NULL;
	ulmk_kern_area_dropped(s);
}

static void parent_unlink(struct ulmk_area *a)
{
	struct ulmk_area **link;

	if (!a->parent)
		return;
	for (link = &a->parent->child; *link; link = &(*link)->sibling) {
		if (*link == a) {
			*link = a->sibling;
			break;
		}
	}
	a->sibling = NULL;
}

/*
 * Free @a, which is already out of its set and has no children, then walk up:
 * an orphaned ancestor only lived on for the areas derived from it.
 */
static void release_up(struct ulmk_area *a)
{
	struct ulmk_area *p;

	while (a) {
		p = a->parent;
		parent_unlink(a);
		if (a->flags & ULMK_AREA_BACKED)
			ulmk_kern_area_retire(a);
		else
			ulmk_heap_free(a);
		if (!p || p->set || p->child)
			break;
		a = p;
	}
}

void ulmk_area_revoke(struct ulmk_area *a)
{
	struct ulmk_area *n;
	struct ulmk_area *p;

	if (!a)
		return;

	/* Post-order over the subtree, always detaching the first child. */
	n = a;
	for (;;) {
		while (n->child)
			n = n->child;
		if (n == a)
			break;
		p = n->parent;
		p->child   = n->sibling;
		n->sibling = NULL;
		set_unlink(n);
		ulmk_heap_free(n);
		n = p;
	}

	set_unlink(a);
	release_up(a);
}

/*
 * A dying holder does not revoke what it handed out: areas derived from its
 * set keep their range, and the orphaned record keeps the backing alive
 * until the last of them goes.
 */
void ulmk_area_set_release(struct ulmk_area_set *s)
{
	struct ulmk_area *a;

	while ((a = s->head) != NULL) {
		set_unlink(a);
		if (!a->child)
			release_up(a);
	}
}

int ulmk_area_inherit(struct ulmk_area_set *dst,
		      const struct ulmk_area_set *src)
{
	struct ulmk_area *a;
	int rc;

	for (a = src->head; a; a = a->next) {
		rc = ulmk_area_insert(dst, a->base, a->size, a->perms,
				      a->type, 0u, a, NULL);
		if (rc != ULMK_OK)
			return rc;
	}
	return ULMK_OK;
}

/*
 * Largest window the MPU can encode that holds @addr and stays inside @a.
 * Never rounds outward: a window grown past the area hands the thread
 * whatever the allocator placed next to it.
 */
int ulmk_area_window(const struct ulmk_area *a, uintptr_t addr,
		     ulmk_arch_region_t *win)
{
	uintptr_t lo;
	uintptr_t b;
	uintptr_t nb;
	size_t    s;

	if (!a || addr < a->base || addr - a->base >= a->size)
		return ULMK_EINVAL;

	lo = a->base;
#if ULMK_ARCH_MPU_WIN_POW2
	s = ULMK_ARCH_MPU_WIN_MIN;
	b = addr & ~(uintptr_t)(s - 1u);
	if (a->size < s || b < lo || b - lo > a->size - s)
		return ULMK_EINVAL;
	while (s <= ((size_t)-1 >> 1)) {
		nb = addr & ~(uintptr_t)((s << 1) - 1u);
		if (a->size < (s << 1) || nb < lo ||
		    nb - lo > a->size - (s << 1))
			break;
		s <<= 1;
		b  = nb;
	}
#else
	b  = (lo + ULMK_ARCH_MPU_WIN_MIN - 1u) &
	     ~(uintptr_t)(ULMK_ARCH_MPU_WIN_MIN - 1u);
	nb = (lo + a->size) & ~(uintptr_t)(ULMK_ARCH_MPU_WIN_MIN - 1u);
	if (addr < b || addr >= nb)
		return ULMK_EINVAL;
#ifdef ULMK_ARCH_MPU_WIN_MAX
	/*
	 * A window costs the arch per granule it covers, so one fault loads
	 * only the stretch around @addr; the rest faults in on first touch.
	 */
	if (nb - b > ULMK_ARCH_MPU_WIN_MAX) {
		b = addr & ~(uintptr_t)(ULMK_ARCH_MPU_WIN_MIN - 1u);
		if (nb - b > ULMK_ARCH_MPU_WIN_MAX)
			nb = b + ULMK_ARCH_MPU_WIN_MAX;
	}
#endif
	s = nb - b;
#endif

	win->base  = b;
	win->size  = s;
	win->perms = a->perms;
	win->type  = a->type;
	return ULMK_OK;
}
