/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Sleep / sleep_cancel — kernel/time/sleep.c
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <kernel/include/ulmk_timer.h>
#include <kernel/include/ulmk_timeout_internal.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_klock.h>
#include <kernel/syscall/syscall_router.h>

uint32_t ulmk_ms_to_ticks(uint32_t ms)
{
	uint64_t ticks;

	ticks = ((uint64_t)ms * (uint64_t)ULMK_CONFIG_TICK_HZ + 999u) / 1000u;
	if (ticks == 0u)
		ticks = 1u;
	if (ticks > ULMK_TIMER_TIMEOUT_MAX)
		return 0u;
	return (uint32_t)ticks;
}

int ulmk_timeout_arm(ulmk_thread_t *th, uint32_t ms,
		     void (*cb)(struct ulmk_timeout *to))
{
	uint32_t ticks = ulmk_ms_to_ticks(ms);

	if (!th || !cb || ticks == 0u)
		return ULMK_EINVAL;

	/*
	 * Cancel first — never sys_dnode_init() a still-linked node.
	 * Init-without-remove orphans the wheel bucket and the next tick
	 * walks a self-loop forever (IRQs masked).
	 */
	(void)ulmk_timer_cancel(&th->timeout);
	th->timeout.cb = cb;
	return ulmk_timer_add(&th->timeout, ticks);
}

void ulmk_timeout_disarm(ulmk_thread_t *th)
{
	if (!th)
		return;
	(void)ulmk_timer_cancel(&th->timeout);
	th->timeout.cb = NULL;
}

static void sleep_timeout_cb(struct ulmk_timeout *to)
{
	ulmk_thread_t *th =
		SYS_DLIST_CONTAINER_OF(to, ulmk_thread_t, timeout);
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_thread);
#endif
	if (!th || th->state != UL_THREAD_STATE_BLOCKED ||
	    th->blocked_reason != UL_BLOCKED_SLEEP) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_thread, key);
#endif
		return;
	}

	th->block_status   = ULMK_OK;
	th->blocked_reason = UL_BLOCKED_NONE;
	th->state          = UL_THREAD_STATE_READY;
	ulmk_sched_enqueue(th);
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_thread, key);
	ulmk_sched_request_resched();
	ulmk_sched_kick_pending();
#endif
}

uint32_t ulmk_kern_sleep(uint32_t ms)
{
	ulmk_thread_t      *cur;
	ulmk_arch_irq_key_t key;

	if (ulmk_ms_to_ticks(ms) == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	cur = ulmk_sched_current();
	if (!cur)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_thread);

	cur->blocked_reason = UL_BLOCKED_SLEEP;
	cur->block_status   = 0;
	cur->state          = UL_THREAD_STATE_BLOCKED;

	if (ulmk_timeout_arm(cur, ms, sleep_timeout_cb) != ULMK_OK) {
		cur->state          = UL_THREAD_STATE_READY;
		cur->blocked_reason = UL_BLOCKED_NONE;
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_thread, key);
		return (uint32_t)(int32_t)ULMK_EINVAL;
	}

	ulmk_sched_dequeue(cur);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_thread, key);
	return 0u;
}

uint32_t ulmk_kern_sleep_cancel(uint32_t tid)
{
	ulmk_thread_t      *th;
	ulmk_arch_irq_key_t key;

	th = ulmk_thread_by_tid((ulmk_tid_t)tid);
	if (!th)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_thread);

	if (th->state != UL_THREAD_STATE_BLOCKED ||
	    th->blocked_reason != UL_BLOCKED_SLEEP) {
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_thread, key);
		return (uint32_t)(int32_t)ULMK_EINVAL;
	}

	ulmk_timeout_disarm(th);
	th->block_status   = ULMK_ECANCELED;
	th->blocked_reason = UL_BLOCKED_NONE;
	th->state          = UL_THREAD_STATE_READY;
	ulmk_sched_enqueue(th);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_thread, key);
#ifndef UL_UNIT_TEST
	ulmk_sched_request_resched();
	ulmk_sched_kick_pending();
#endif
	return (uint32_t)(int32_t)ULMK_OK;
}
