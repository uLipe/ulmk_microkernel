/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Kernel scheduler internal interface — kernel/include/ulmk_sched.h
 */

#ifndef UL_SCHED_H
#define UL_SCHED_H

#include <stdbool.h>
#include <ulmk/config.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <ulmk_arch.h>

/* =========================================================================
 * Scheduler class — vtable for pluggable scheduling policies.
 *
 * All callbacks are invoked with interrupts serialised.
 *
 * pick_next() / peek_next() return the highest-priority ready thread.
 * The idle thread (lowest priority sentinel) is always in the run queue,
 * so these never return NULL after ulmk_sched_start().
 * ========================================================================= */

typedef struct ulmk_sched_class {
	const char	*name;
	void		 (*init)(void);
	void		 (*enqueue)(ulmk_thread_t *t);
	void		 (*dequeue)(ulmk_thread_t *t);
	ulmk_thread_t	*(*pick_next)(void);
	ulmk_thread_t	*(*peek_next)(void);
} ulmk_sched_class_t;

/* Exported by kernel/sched/fifo_rt.c */
extern const ulmk_sched_class_t ulmk_fifo_rt_class;

/* Exported by kernel/sched/bitmap_rt.c — 256-level O(1) scheduler (default) */
extern const ulmk_sched_class_t ulmk_bitmap_rt_class;

/* =========================================================================
 * Scheduler core (kernel/sched/sched.c)
 *
 * Hot-path model (FreeRTOS/Zephyr-style deferred reschedule):
 *   handlers enqueue / block and return; ulmk_sched_trap_dispatch() at
 *   syscall/ISR exit performs the only context switch.
 * ========================================================================= */

/*
 * ulmk_sched_init      — initialise scheduler state; call before set_class/start.
 * ulmk_sched_set_class — install a scheduling policy and run its init hook.
 * ulmk_sched_start     — first switch from the startup frame; does not return.
 * ulmk_sched_resched   — immediate pick+switch (cold paths only: boot, panic).
 *                        Prefer trap-exit dispatch for syscall/IRQ handlers.
 */
void		 ulmk_sched_init(void);
void		 ulmk_sched_set_class(const ulmk_sched_class_t *cls);
void		 ulmk_sched_start(void);
void		 ulmk_sched_resched(void);

/*
 * Mark that trap/ISR exit must switch (yield rotation, explicit preempt).
 * Enqueue of a higher-priority thread also sets this automatically.
 */
void		 ulmk_sched_request_resched(void);

void		 ulmk_sched_enqueue(ulmk_thread_t *t);
void		 ulmk_sched_dequeue(ulmk_thread_t *t);
#if ULMK_CONFIG_ENABLE_SMP
/* Send deferred remote IPIs (call after releasing IPC locks). */
void		 ulmk_sched_kick_pending(void);
#else
static inline void ulmk_sched_kick_pending(void) { }
#endif
/*
 * Same as enqueue/dequeue — kernel/ISR gateways already mask IRQs.
 * Kept as explicit aliases for call sites that used to nest irq_save.
 */
void		 ulmk_sched_enqueue_locked(ulmk_thread_t *t);
void		 ulmk_sched_dequeue_locked(ulmk_thread_t *t);
ulmk_thread_t	*ulmk_sched_current(void);
/* Load @th's static map and pinned stack; drops the previous dynamic windows. */
void		 ulmk_sched_mpu_switch(const ulmk_thread_t *th);
ulmk_thread_t	*ulmk_sched_peek_next(void);

/*
 * ulmk_sched_trap_dispatch — called from arch trap/ISR exit.  from_isr=true
 * runs ISR preemption; from_isr=false runs syscall-exit preemption.
 *
 * Switches when current is not RUNNING (blocked/dead/suspended) or when
 * needs_resched / a higher-priority ready thread is present.
 */
void		 ulmk_sched_trap_dispatch(bool from_isr);

/*
 * ulmk_sched_set_dead_for_cleanup — register a dead thread for deferred
 * resource release after the next context switch.
 */
void		 ulmk_sched_set_dead_for_cleanup(ulmk_thread_t *th);

#endif /* UL_SCHED_H */
