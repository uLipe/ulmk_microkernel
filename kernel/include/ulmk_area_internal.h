/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Per-thread memory areas — kernel/include/ulmk_area_internal.h
 * Not part of the public API.
 *
 * An area is a range a thread may touch from userspace.  Areas are never
 * programmed up front: the MPU/PMP fault handler looks the faulting address
 * up here and loads a window on demand.  Every area either owns its range
 * (origin) or was derived from another area by grant or inheritance; the
 * derivation tree drives cascading revocation.
 */

#ifndef UL_AREA_INTERNAL_H
#define UL_AREA_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk_arch.h>

/* Origin owns a heap block that is freed with the last reference. */
#define ULMK_AREA_BACKED	(1u << 0)

struct ulmk_area_set;

struct ulmk_area {
	struct ulmk_area	*next;		/* holder list, sorted by base */
	struct ulmk_area	*parent;	/* derived from; NULL = origin */
	struct ulmk_area	*child;		/* first derived area */
	struct ulmk_area	*sibling;	/* next area derived from parent */
	struct ulmk_area_set	*set;		/* holder; NULL once orphaned */
	uintptr_t		 base;
	size_t			 size;
	uint32_t		 perms;
	uint8_t			 type;		/* ULMK_REGION_* */
	uint8_t			 flags;
};

struct ulmk_area_set {
	struct ulmk_area	*head;
	uint16_t		 count;
};

void ulmk_area_set_init(struct ulmk_area_set *s);

int ulmk_area_insert(struct ulmk_area_set *s, uintptr_t base, size_t size,
		     uint32_t perms, uint8_t type, uint8_t flags,
		     struct ulmk_area *parent, struct ulmk_area **out);

struct ulmk_area *ulmk_area_find(const struct ulmk_area_set *s,
				 uintptr_t addr);
struct ulmk_area *ulmk_area_find_base(const struct ulmk_area_set *s,
				      uintptr_t base);
bool ulmk_area_range_ok(const struct ulmk_area_set *s, uintptr_t addr,
			size_t len, uint32_t perms);

void ulmk_area_revoke(struct ulmk_area *a);
void ulmk_area_set_release(struct ulmk_area_set *s);
int ulmk_area_inherit(struct ulmk_area_set *dst,
		      const struct ulmk_area_set *src);

int ulmk_area_window(const struct ulmk_area *a, uintptr_t addr,
		     ulmk_arch_region_t *win);

/*
 * Called with the area lock held whenever an area leaves a live set, so the
 * owner of @s can drop any window still loaded for it.
 */
void ulmk_kern_area_dropped(struct ulmk_area_set *s);

/*
 * Called with the area lock held for a backing record that is out of every
 * set and has nothing derived from it.  The kernel frees the block and @a
 * once no CPU can still reach the block through a live window.
 */
void ulmk_kern_area_retire(struct ulmk_area *a);

#endif /* UL_AREA_INTERNAL_H */
