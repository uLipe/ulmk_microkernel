/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Kernel heap and section-placement utilities — kernel/include/ulmk_mem_internal.h
 * Not part of the public API.
 */

#ifndef UL_MEM_INTERNAL_H
#define UL_MEM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Kernel section placement attributes (approach B: explicit annotation).
 *
 * Defined as no-ops for now; all kernel globals land in the default BSS/DATA
 * sections, which are already in kernel-only SRAM (protected by PRS selection
 * in the MPU: only PRS 0/kernel can access the full address space).
 *
 * When production linker scripts are wired up with explicit .kernel.bss
 * and .kernel.data regions, these can be activated by defining:
 *   #define UL_KERNEL_DATA  __attribute__((section(".kernel.data")))
 *   #define UL_KERNEL_BSS   __attribute__((section(".kernel.bss")))
 * ========================================================================= */

#define UL_KERNEL_DATA  /* placed in default .data */
#define UL_KERNEL_BSS   /* placed in default .bss  */

/* =========================================================================
 * TLSF heap API — backed by kernel/mem/tlsf.c
 *
 * All allocations are aligned to 64 bytes so they can be used directly as
 * MPU regions without re-alignment.  Supports pools up to ~512 MB.
 * ========================================================================= */

void   ulmk_heap_init(uintptr_t base, size_t size);
void  *ulmk_heap_alloc(size_t size);
void   ulmk_heap_free(void *ptr);
void  *ulmk_heap_aligned_alloc(size_t align, size_t size);
size_t ulmk_heap_free_bytes(void);

struct ulmk_thread;

/* Area-set lifetime for a TCB; both take the area lock. */
void ulmk_mem_thread_release(struct ulmk_thread *th);
int  ulmk_mem_thread_inherit(struct ulmk_thread *child,
			     const struct ulmk_thread *parent);

#endif /* UL_MEM_INTERNAL_H */
