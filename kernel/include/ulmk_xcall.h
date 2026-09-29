/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Cross-CPU calls — kernel/include/ulmk_xcall.h
 *
 * Threads never migrate, so state a CPU is running (its current thread, its
 * live MPU windows) is only ever changed on that CPU: other CPUs ask it to.
 */

#ifndef UL_XCALL_H
#define UL_XCALL_H

#include <stdint.h>
#include <ulmk/config.h>

#if ULMK_CONFIG_ENABLE_SMP

typedef uint32_t (*ulmk_xcall_fn_t)(void *arg);

/*
 * Run @fn(@arg) on @cpu and return its result.  Synchronous: the caller spins
 * until the target has run it.  Must be called with no kernel lock held.
 */
uint32_t ulmk_xcall(uint32_t cpu, ulmk_xcall_fn_t fn, void *arg);

/* Run the request posted to this CPU, if any.  Called from IPI entry. */
void ulmk_xcall_service(void);

#endif /* ULMK_CONFIG_ENABLE_SMP */

#endif /* UL_XCALL_H */
