/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Memory handlers — kernel/mem/mem.c
 * Implements: kernel/syscall/syscall_router.h ulmk_kern_mem_* prototypes
 * Reference: docs/api_spec.md §9
 */

#include <stdint.h>
#include <stddef.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <kernel/syscall/syscall_router.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_sched.h>
#include <ulmk_arch.h>

/*
 * Add a region to a thread's MPU region list.
 * Returns ULMK_OK or ULMK_ENOSPC if the list is full.
 */
static int thread_add_region(ulmk_thread_t *th, uintptr_t base, size_t size,
			     uint32_t perms, uint8_t type)
{
	ulmk_arch_region_t *r;

	if (th->region_count >= ULMK_ARCH_MAX_REGIONS)
		return ULMK_ENOSPC;

	r        = &th->regions[th->region_count];
	r->base  = base;
	r->size  = size;
	r->perms = perms;
	r->type  = type;
	th->region_count++;
	return ULMK_OK;
}

/*
 * Remove a region from @th that starts at @base.
 * Returns ULMK_OK if found and removed, ULMK_EINVAL if not found.
 */
static int thread_remove_region(ulmk_thread_t *th, uintptr_t base)
{
	uint8_t i;
	uint8_t last;

	for (i = 0u; i < th->region_count; i++) {
		if (th->regions[i].base != base)
			continue;

		last = th->region_count - 1u;
		if (i != last)
			th->regions[i] = th->regions[last];
		th->region_count--;
		return ULMK_OK;
	}

	return ULMK_EINVAL;
}

/*
 * Heap syscall handlers — expose TLSF heap to userspace.
 * Allocated memory is NOT automatically granted as an MPU region;
 * caller must use ULMK_SYS_MMAP / ULMK_SYS_MEM_GRANT for that.
 */
uint32_t ulmk_kern_heap_alloc(uint32_t size)
{
	void *p = ulmk_heap_alloc((size_t)size);

	return (uint32_t)(uintptr_t)p;
}

uint32_t ulmk_kern_heap_free(uint32_t ptr)
{
	ulmk_heap_free((void *)(uintptr_t)ptr);
	return 0u;
}

uint32_t ulmk_kern_heap_aligned_alloc(uint32_t align, uint32_t size)
{
	void *p = ulmk_heap_aligned_alloc((size_t)align, (size_t)size);

	return (uint32_t)(uintptr_t)p;
}

uint32_t ulmk_kern_mem_map(uint32_t hint, uint32_t size,
			 uint32_t perms, uint32_t flags)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	void        *mem;
	uintptr_t    base;
	int          rc;

	if (!cur || size == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	if (flags & ULMK_MMAP_PERIPH) {
		/*
		 * Peripheral mapping: caller provides the MMIO base address.
		 * Requires ULMK_CAP_MAP_PERIPH (enforced in syscall_router.c).
		 */
		if (hint == 0u)
			return (uint32_t)(int32_t)ULMK_EINVAL;

		base = (uintptr_t)hint;
		rc   = thread_add_region(cur, base, size, perms, ULMK_REGION_PERIPH);
		if (rc != ULMK_OK)
			return (uint32_t)(int32_t)rc;

		/* Immediately apply the new region to PRS 1 DPRs. */
		ulmk_arch_mpu_switch(cur->regions, cur->region_count,
				     cur->privilege == ULMK_PRIV_KERNEL ? 0u : 1u);
		return (uint32_t)base;
	}

	if (flags & ULMK_MMAP_SHARED) {
		/*
		 * Shared physical window (external SDRAM, framebuffer, …).
		 * Same hint contract as PERIPH, but Normal non-cacheable MPU
		 * attrs — not Device.  Cap: ULMK_CAP_MAP_SHARED.
		 */
		if (hint == 0u)
			return (uint32_t)(int32_t)ULMK_EINVAL;

		base = (uintptr_t)hint;
		rc   = thread_add_region(cur, base, size, perms, ULMK_REGION_SHARED);
		if (rc != ULMK_OK)
			return (uint32_t)(int32_t)rc;

		ulmk_arch_mpu_switch(cur->regions, cur->region_count,
				     cur->privilege == ULMK_PRIV_KERNEL ? 0u : 1u);
		return (uint32_t)base;
	}

	if (flags & ULMK_MMAP_ANON) {
		mem = ulmk_heap_alloc(size);
		if (!mem)
			return (uint32_t)(int32_t)ULMK_ENOMEM;

		base = (uintptr_t)mem;
		rc   = thread_add_region(cur, base, size, perms, ULMK_REGION_HEAP);
		if (rc != ULMK_OK) {
			ulmk_heap_free(mem);
			return (uint32_t)(int32_t)rc;
		}

		/* Immediately apply the new region to PRS 1 DPRs. */
		ulmk_arch_mpu_switch(cur->regions, cur->region_count,
				     cur->privilege == ULMK_PRIV_KERNEL ? 0u : 1u);
		return (uint32_t)base;
	}

	return (uint32_t)(int32_t)ULMK_EINVAL;
}

uint32_t ulmk_kern_mem_unmap(uint32_t addr, uint32_t size)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_arch_region_t *r;
	uint8_t i;

	if (!cur || addr == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	for (i = 0u; i < cur->region_count; i++) {
		r = &cur->regions[i];
		if ((uint32_t)r->base != addr)
			continue;

		if (r->type == ULMK_REGION_HEAP)
			ulmk_heap_free((void *)(uintptr_t)addr);

		thread_remove_region(cur, (uintptr_t)addr);
		ulmk_arch_mpu_switch(cur->regions, cur->region_count,
				     cur->privilege == ULMK_PRIV_KERNEL ? 0u : 1u);
		(void)size;
		return (uint32_t)ULMK_OK;
	}

	return (uint32_t)(int32_t)ULMK_EINVAL;
}

uint32_t ulmk_kern_mem_grant(uint32_t addr, uint32_t size,
			   uint32_t target_tid, uint32_t perms)
{
	ulmk_thread_t *target;
	ulmk_thread_t *cur = ulmk_sched_current();
	ulmk_arch_region_t *r;
	uint8_t i;
	int     rc;

	if (!cur || addr == 0u || size == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	/* Verify caller owns the region being granted */
	for (i = 0u; i < cur->region_count; i++) {
		if ((uint32_t)cur->regions[i].base == addr)
			break;
	}
	if (i >= cur->region_count)
		return (uint32_t)(int32_t)ULMK_EPERM;

	r      = &cur->regions[i];
	target = ulmk_thread_by_tid((ulmk_tid_t)target_tid);
	if (!target)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	/* Grant read-only by default; caller may not grant more perms than held */
	uint32_t granted_perms = perms & r->perms;

	rc = thread_add_region(target, (uintptr_t)addr, r->size,
			       granted_perms, ULMK_REGION_SHARED);
	return (rc == ULMK_OK) ? (uint32_t)ULMK_OK : (uint32_t)(int32_t)rc;
}

/*
 * ulmk_kern_heap_extend — allocate an additional slab from user_pool and
 * add it as a new MPU DPR for the calling thread.
 * Requires the thread to already have a heap (attr.heap_size > 0).
 * Requires ULMK_PRIV_DRIVER (enforced by the syscall router).
 */
uint32_t ulmk_kern_heap_extend(uint32_t size)
{
	ulmk_thread_t *cur = ulmk_sched_current();
	void          *mem;
	int            rc;

	if (!cur || size == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;
	if (cur->heap_size == 0u)
		return (uint32_t)(int32_t)ULMK_EPERM;

	mem = ulmk_heap_alloc((size_t)size);
	if (!mem)
		return (uint32_t)(int32_t)ULMK_ENOMEM;

	rc = thread_add_region(cur, (uintptr_t)mem, (size_t)size,
			       ULMK_PERM_READ | ULMK_PERM_WRITE,
			       ULMK_REGION_HEAP);
	if (rc != ULMK_OK) {
		ulmk_heap_free(mem);
		return (uint32_t)(int32_t)rc;
	}

	ulmk_arch_mpu_switch(cur->regions, cur->region_count,
			     cur->privilege == ULMK_PRIV_KERNEL ? 0u : 1u);
	return (uint32_t)ULMK_OK;
}
