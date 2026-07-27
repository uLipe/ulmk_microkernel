/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Scheduler core — kernel/sched/sched.c
 * Dispatches through ulmk_sched_class_t vtable; manages per-CPU current/idle.
 *
 * Context switches from syscall/IRQ hot paths happen only in
 * ulmk_sched_trap_dispatch() at trap/ISR exit.
 *
 * IRQs are already masked for the whole kernel/ISR gateway on every arch;
 * this file does not nest irq_save/restore.  Cross-CPU shared state uses
 * spin_lock_irqsave at the call sites that touch pools.
 */

#include <stddef.h>
#include <stdbool.h>
#include <ulmk/config.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_percpu.h>
#include <ulmk_arch.h>

static const ulmk_sched_class_t * UL_KERNEL_BSS sched_class;

static inline __attribute__((always_inline))
uint8_t sched_thread_prs(const ulmk_thread_t *t)
{
	if (t && t->privilege == ULMK_PRIV_KERNEL)
		return 0u;
	return 1u;
}

static inline __attribute__((always_inline))
void sched_mark_preempt_if_higher(ulmk_thread_t *t)
{
	struct ulmk_percpu *pc;
	ulmk_thread_t      *cur;

	pc  = ulmk_percpu_of(t->cpu);
	cur = pc->current;
	if (!cur || (t != cur && t->priority < cur->priority))
		pc->needs_resched = true;

#if ULMK_CONFIG_ENABLE_SMP
	/*
	 * Remote RQ is only examined on that CPU.  Defer the IPI into
	 * ipi_pending so callers can drop IPC/RQ locks first; flushed by
	 * ulmk_sched_kick_pending() (trap exit / explicit wake paths).
	 * Idle has no tick — without a kick the remote never notices.
	 */
	if (t->cpu != (uint8_t)ulmk_arch_cpu_id() &&
	    ulmk_percpu_of(t->cpu)->online) {
		ulmk_percpu()->ipi_pending |= (1u << t->cpu);
	}
#endif
}

static void sched_reap_dead(ulmk_thread_t *prev)
{
	struct ulmk_percpu *pc = ulmk_percpu();

	if (!pc->dead_for_cleanup || pc->dead_for_cleanup == prev)
		return;

	ulmk_arch_ctx_free(&pc->dead_for_cleanup->ctx);
	ulmk_thread_free(pc->dead_for_cleanup);
	pc->dead_for_cleanup = NULL;
}

static void sched_switch_to(ulmk_thread_t *prev, ulmk_thread_t *next)
{
	struct ulmk_percpu *pc = ulmk_percpu();
	ulmk_arch_ctx_t    *from;
	ulmk_arch_ctx_t    *to;

	if (!next)
		next = pc->idle;

	ulmk_thread_ensure_ctx(next);

	from = prev ? &prev->ctx : &pc->startup_ctx;
	to   = &next->ctx;

	if (from == to) {
		pc->current = next;
		next->state = UL_THREAD_STATE_RUNNING;
		if (prev && sys_dnode_is_linked(&prev->sched_node)) {
			sched_class->dequeue(prev);
			prev->state = UL_THREAD_STATE_RUNNING;
		}
		return;
	}

	ulmk_arch_mpu_switch(next->regions, next->region_count,
			     sched_thread_prs(next));

	pc->current = next;
	next->state = UL_THREAD_STATE_RUNNING;
	/*
	 * Always go through arch_sched_switch so arches that share INT with
	 * syscall (C29) can defer the handoff to RETI.INT.  A direct
	 * ctx_switch from inside INT abandons the veneer and leaves the
	 * controller active — further ticks never arrive.
	 */
	ulmk_arch_sched_switch(from, to, ULMK_SCHED_SWITCH_COOP);
}

void ulmk_sched_set_dead_for_cleanup(ulmk_thread_t *th)
{
	struct ulmk_percpu *pc = ulmk_percpu();

	if (pc->dead_for_cleanup) {
		ulmk_arch_ctx_free(&pc->dead_for_cleanup->ctx);
		ulmk_thread_free(pc->dead_for_cleanup);
	}
	pc->dead_for_cleanup = th;
}

void ulmk_sched_init(void)
{
	ulmk_percpu_init();
	sched_class = NULL;
}

void ulmk_sched_set_class(const ulmk_sched_class_t *cls)
{
	sched_class = cls;
	cls->init();
}

void ulmk_sched_start(void)
{
	struct ulmk_percpu *pc = ulmk_percpu();
	ulmk_thread_t      *first = sched_class->pick_next();

	ulmk_thread_ensure_ctx(first);

	/*
	 * Publish current before online/armed.  A remote enqueue IPI that
	 * lands with current==NULL only arms needs_resched; sched_start
	 * must keep that flag on SMP so the first trap/ISR exit can switch.
	 */
	pc->current  = first;
	first->state = UL_THREAD_STATE_RUNNING;
	pc->online   = true;
#if ULMK_CONFIG_ENABLE_SMP
	if (ulmk_arch_cpu_id() != 0u)
		ulmk_arch_secondary_mark_ready();
	/*
	 * Do not clear needs_resched: CPU0 may have enqueued a remote thread
	 * between online and the switch below (soft IPI / SETR already out).
	 */
#else
	pc->needs_resched = false;
#endif
	/*
	 * Do not arm the HW tick here: the first RFE into a virgin userspace
	 * context races with STM0 on QEMU TriCore and can hang before the
	 * first root instruction.  Arm on the first trap/syscall instead.
	 */
	ulmk_arch_mpu_switch(first->regions, first->region_count,
			     sched_thread_prs(first));
	ulmk_arch_ctx_switch(&pc->startup_ctx, &first->ctx);
}

void ulmk_sched_request_resched(void)
{
	ulmk_percpu()->needs_resched = true;
}

#if ULMK_CONFIG_ENABLE_SMP
void ulmk_sched_kick_pending(void)
{
	struct ulmk_percpu *pc = ulmk_percpu();
	uint32_t            pending;
	uint32_t            cpu;

	pending = pc->ipi_pending;
	if (!pending)
		return;
	pc->ipi_pending = 0u;

	for (cpu = 0u; cpu < (uint32_t)ULMK_NR_CPUS; cpu++) {
		if (pending & (1u << cpu))
			ulmk_arch_send_ipi(cpu);
	}
}
#endif

void ulmk_sched_resched(void)
{
	struct ulmk_percpu *pc = ulmk_percpu();
	ulmk_thread_t      *prev = pc->current;
	ulmk_thread_t      *next;

	pc->needs_resched = false;
	sched_reap_dead(prev);
	next = sched_class->pick_next();
	sched_switch_to(prev, next);
}

void ulmk_sched_enqueue(ulmk_thread_t *t)
{
	sched_class->enqueue(t);
	sched_mark_preempt_if_higher(t);
}

void ulmk_sched_dequeue(ulmk_thread_t *t)
{
	sched_class->dequeue(t);
}

void ulmk_sched_enqueue_locked(ulmk_thread_t *t)
{
	ulmk_sched_enqueue(t);
}

void ulmk_sched_dequeue_locked(ulmk_thread_t *t)
{
	ulmk_sched_dequeue(t);
}

ulmk_thread_t *ulmk_sched_current(void)
{
	return ulmk_percpu()->current;
}

ulmk_thread_t *ulmk_sched_peek_next(void)
{
	return sched_class ? sched_class->peek_next() : NULL;
}

void ulmk_sched_trap_dispatch(bool from_isr)
{
	struct ulmk_percpu *pc;
	ulmk_thread_t      *cur;
	ulmk_thread_t      *next;
	bool                want;

	/* Flush before any switch so remotes can run while we continue. */
	ulmk_sched_kick_pending();

	pc = ulmk_percpu();
	cur = pc->current;

	if (!cur || !sched_class)
		return;

	want = pc->needs_resched;
	pc->needs_resched = false;

	if (cur->state != UL_THREAD_STATE_RUNNING) {
		sched_reap_dead(cur);
		next = sched_class->pick_next();
		sched_switch_to(cur, next);
		return;
	}

	if (!want) {
		next = sched_class->peek_next();
		if (!next || next == cur || next->priority >= cur->priority)
			return;
	}

	if (from_isr) {
		cur->state = UL_THREAD_STATE_READY;
		if (sys_dnode_is_linked(&cur->sched_node))
			sched_class->dequeue(cur);
		sched_class->enqueue(cur);

		if (ulmk_arch_sched_isr_preempt_deferred()) {
			/* TriCore: stage the CSA handoff, switch on RFE. */
			next = sched_class->pick_next();
			ulmk_thread_ensure_ctx(next);

			pc->current = next;
			next->state = UL_THREAD_STATE_RUNNING;
			ulmk_arch_mpu_switch(next->regions, next->region_count,
					     sched_thread_prs(next));
			ulmk_arch_sched_switch(&cur->ctx, &next->ctx,
					       ULMK_SCHED_SWITCH_PREEMPT_ISR);
			return;
		}

		/* Coop coroutine switch (ARM Handler mode / RISC-V trap). */
		next = sched_class->pick_next();
		sched_switch_to(cur, next);
		return;
	}

	if (sys_dnode_is_linked(&cur->sched_node))
		sched_class->dequeue(cur);
	cur->state = UL_THREAD_STATE_READY;
	sched_class->enqueue(cur);

	next = sched_class->pick_next();
	sched_switch_to(cur, next);
}
