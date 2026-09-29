/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Memory handlers — kernel/mem/mem.c
 * Implements: kernel/syscall/syscall_router.h ulmk_kern_mem_* prototypes
 * Reference: docs/api_spec.md §9
 *
 * Mapping only records an area.  The MPU/PMP is loaded lazily from
 * ulmk_kern_mem_fault() when the thread first touches it, so the number of
 * areas a thread holds is not bounded by the hardware slot count.
 */

#include <stdint.h>
#include <stddef.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <kernel/syscall/syscall_router.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_area_internal.h>
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_klock.h>
#include <kernel/include/ulmk_sched.h>
#include <ulmk_arch.h>

#define MEM_PERMS	(ULMK_PERM_READ | ULMK_PERM_WRITE | ULMK_PERM_EXEC)

static ulmk_thread_t *set_owner(struct ulmk_area_set *s)
{
	return (ulmk_thread_t *)((uint8_t *)s - offsetof(ulmk_thread_t, areas));
}

/*
 * Windows are only ever live on the CPU running their thread, and a switch
 * drops them, so leaving the set is enough unless that thread is current.
 */
void ulmk_kern_area_dropped(struct ulmk_area_set *s)
{
	if (set_owner(s) == ulmk_sched_current())
		ulmk_arch_mpu_flush();
}

bool ulmk_kern_mem_fault(uintptr_t addr, uint32_t access)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_arch_irq_key_t key;
	struct ulmk_area *a;
	ulmk_arch_region_t win;
	bool ok = false;

	if (!cur || cur->privilege == ULMK_PRIV_KERNEL)
		return false;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	a = ulmk_area_find(&cur->areas, addr);
	if (a && (a->perms & access) == access &&
	    ulmk_area_window(a, addr, &win) == ULMK_OK)
		ok = ulmk_arch_mpu_load(&win);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);
	return ok;
}

uint32_t ulmk_kern_mem_map(uint32_t hint, uint32_t size,
			   uint32_t perms, uint32_t flags)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_arch_irq_key_t key;
	uintptr_t base;
	size_t    len = size;
	uint8_t   type;
	uint8_t   aflags = 0u;
	int       rc;

	if (!cur || size == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	if (flags & ULMK_MMAP_PERIPH) {
		type = ULMK_REGION_PERIPH;
	} else if (flags & ULMK_MMAP_SHARED) {
		type = ULMK_REGION_SHARED;
	} else if (flags & ULMK_MMAP_ANON) {
		type   = ULMK_REGION_HEAP;
		aflags = ULMK_AREA_BACKED;
	} else {
		return (uint32_t)(int32_t)ULMK_EINVAL;
	}

	if (aflags & ULMK_AREA_BACKED) {
		/* The block is granule-rounded anyway; let windows use it all. */
		len  = (len + ULMK_ARCH_REGION_ALIGN - 1u) &
		       ~(size_t)(ULMK_ARCH_REGION_ALIGN - 1u);
		base = (uintptr_t)ulmk_heap_alloc(len);
		if (!base)
			return (uint32_t)(int32_t)ULMK_ENOMEM;
	} else {
		if (hint == 0u)
			return (uint32_t)(int32_t)ULMK_EINVAL;
		base = (uintptr_t)hint;
	}

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	rc = ulmk_area_insert(&cur->areas, base, len, perms & MEM_PERMS, type,
			      aflags, NULL, NULL);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);

	if (rc != ULMK_OK) {
		if (aflags & ULMK_AREA_BACKED)
			ulmk_heap_free((void *)base);
		return (uint32_t)(int32_t)rc;
	}
	return (uint32_t)base;
}

/*
 * Dropping an area takes everything derived from it along; for an origin
 * that includes the backing block once the last grant is gone.
 */
uint32_t ulmk_kern_mem_unmap(uint32_t addr, uint32_t size)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_arch_irq_key_t key;
	struct ulmk_area *a;

	(void)size;
	if (!cur || addr == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	a = ulmk_area_find_base(&cur->areas, (uintptr_t)addr);
	if (a)
		ulmk_area_revoke(a);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);

	return a ? (uint32_t)ULMK_OK : (uint32_t)(int32_t)ULMK_EINVAL;
}

uint32_t ulmk_kern_mem_grant(uint32_t addr, uint32_t size,
			     uint32_t target_tid, uint32_t perms)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_thread_t *target;
	ulmk_arch_irq_key_t key;
	struct ulmk_area *a;
	uint8_t type;
	int rc;

	(void)size;
	if (!cur || addr == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	target = ulmk_thread_by_tid((ulmk_tid_t)target_tid);
	if (!target)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	a = ulmk_area_find_base(&cur->areas, (uintptr_t)addr);
	if (!a) {
		rc = ULMK_EPERM;
	} else {
		/*
		 * The alias keeps the owner's memory type: an arch that derives
		 * cacheability from it would otherwise give two views of one
		 * page different policies.  Heap becomes GRANT only to mark
		 * that the holder does not own the block.
		 */
		type = (a->type == ULMK_REGION_HEAP) ? ULMK_REGION_GRANT :
						       a->type;
		rc = ulmk_area_insert(&target->areas, a->base, a->size,
				      perms & a->perms, type, 0u, a, NULL);
	}
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);

	return (uint32_t)(int32_t)rc;
}

uint32_t ulmk_kern_mem_revoke(uint32_t addr, uint32_t target_tid)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_thread_t *target;
	ulmk_arch_irq_key_t key;
	struct ulmk_area *a;
	struct ulmk_area *t;
	struct ulmk_area *p;
	int rc = ULMK_OK;

	if (!cur || addr == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	target = ulmk_thread_by_tid((ulmk_tid_t)target_tid);
	if (!target)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	a = ulmk_area_find_base(&cur->areas, (uintptr_t)addr);
	t = ulmk_area_find_base(&target->areas, (uintptr_t)addr);
	if (!a) {
		rc = ULMK_EPERM;
	} else if (!t) {
		rc = ULMK_EINVAL;
	} else {
		for (p = t->parent; p && p != a; p = p->parent)
			;
		if (p)
			ulmk_area_revoke(t);
		else
			rc = ULMK_EPERM;
	}
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);

	return (uint32_t)(int32_t)rc;
}

void ulmk_mem_thread_release(ulmk_thread_t *th)
{
	ulmk_arch_irq_key_t key;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	ulmk_area_set_release(&th->areas);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);
}

int ulmk_mem_thread_inherit(ulmk_thread_t *child, const ulmk_thread_t *parent)
{
	ulmk_arch_irq_key_t key;
	int rc;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_area);
	rc = ulmk_area_inherit(&child->areas, &parent->areas);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_area, key);
	return rc;
}
