/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Notification handlers — kernel/notif/notif.c
 * Reference: docs/api_spec.md §8
 *
 * Two-layer design:
 *   notif_*_impl() — core logic with native C pointer types; testable on host.
 *   ulmk_kern_notif_*() — syscall ABI wrappers.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <kernel/include/ulmk_notif_internal.h>
#include <kernel/include/ulmk_ep_internal.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_timeout_internal.h>
#ifndef UL_UNIT_TEST
#include <kernel/include/ulmk_klock.h>
#endif
#include <kernel/syscall/syscall_router.h>

#ifndef UL_UNIT_TEST
#include <kernel/include/ulmk_mem_internal.h>
#else
#include <ulmk/config.h>
#endif

#ifdef UL_UNIT_TEST
ulmk_notif_obj_t notif_pool[ULMK_CONFIG_MAX_NOTIFS];
#endif

static void notif_wait_timeout_cb(struct ulmk_timeout *to)
{
	ulmk_thread_t *th =
		SYS_DLIST_CONTAINER_OF(to, ulmk_thread_t, timeout);
	ulmk_notif_obj_t *n;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	if (!th || th->state != UL_THREAD_STATE_BLOCKED ||
	    th->blocked_reason != UL_BLOCKED_NOTIF)
		goto out;

	n = ulmk_notif_by_id(th->blocked_notif);
	if (n && n->waiter == th)
		n->waiter = NULL;

	th->blocked_notif  = ULMK_NOTIF_INVALID;
	th->block_status   = ULMK_ETIMEOUT;
	th->blocked_reason = UL_BLOCKED_NONE;
	th->state          = UL_THREAD_STATE_READY;
	ulmk_sched_enqueue_locked(th);

out:
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
	ulmk_sched_kick_pending();
#endif
}

int ulmk_notif_obj_init(ulmk_notif_obj_t *n, ulmk_notif_t id)
{
	n->id        = id;
	n->active    = true;
	n->bits      = 0;
	n->waiter    = NULL;
	n->wait_mask = 0;
	return 0;
}

ulmk_notif_obj_t *ulmk_notif_by_id(ulmk_notif_t id)
{
#ifdef UL_UNIT_TEST
	if (id == ULMK_NOTIF_INVALID || (uint32_t)id >= ULMK_CONFIG_MAX_NOTIFS)
		return NULL;
	if (!notif_pool[(uint32_t)id].active)
		return NULL;
	return &notif_pool[(uint32_t)id];
#else
	ulmk_notif_obj_t *n = (ulmk_notif_obj_t *)(uintptr_t)id;

	if (!n || !n->active)
		return NULL;
	return n;
#endif
}

/* =========================================================================
 * Core implementation (_impl — native pointer types)
 * ========================================================================= */

int notif_signal_impl(ulmk_notif_t notif_id, uint32_t bits)
{
	ulmk_notif_obj_t *n;
	ulmk_thread_t    *w;
	uint32_t          delivered;
	int               need_disarm = 0;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif

	n = ulmk_notif_by_id(notif_id);
	if (!n) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	n->bits |= bits;

	w = n->waiter;
	if (!w || w->state != UL_THREAD_STATE_BLOCKED) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

	delivered = n->bits & w->notif_wait_mask;
	if (!delivered) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

	n->bits          &= ~delivered;
	n->waiter         = NULL;
	w->notif_received = delivered;
	if (w->timeout.cb)
		need_disarm = 1;

	if (w->blocked_reason == UL_BLOCKED_IPC_OR_NOTIF) {
		ulmk_recv_or_notif_result_t *rn = w->rn_result_outptr;

		ulmk_ep_recv_queue_remove(w);
		w->blocked_notif = ULMK_NOTIF_INVALID;
		if (rn) {
			rn->is_notif   = 1;
			rn->notif_bits = delivered;
			rn->sender     = ULMK_TID_INVALID;
			w->rn_result_outptr = NULL;
		}
		w->syscall_wake_ret       = 1;
		w->syscall_wake_ret_valid = 1;
	} else if (w->notif_bits_outptr) {
		*w->notif_bits_outptr = delivered;
		w->notif_bits_outptr  = NULL;
	}

	w->blocked_reason = UL_BLOCKED_NONE;
	w->state          = UL_THREAD_STATE_READY;
	/*
	 * Enqueue under IPC so an ISR signal cannot race a wait that still
	 * holds the lock between unlock and RQ insert (lost wake / stuck
	 * flood in silicon_irq_stress).  ep_reply uses unlock-first because
	 * it can deadlock with call's IPC→RQ order across CPUs.
	 */
	ulmk_sched_enqueue_locked(w);
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	/* Timer lock must not nest under ipc (see ulmk_klock.h). */
	if (need_disarm)
		ulmk_timeout_disarm(w);
#ifndef UL_UNIT_TEST
	ulmk_sched_kick_pending();
#endif
	return 0;
}

int notif_wait_impl(ulmk_notif_t notif_id, uint32_t mask, uint32_t *out)
{
	ulmk_notif_obj_t *n;
	ulmk_thread_t    *cur;
	uint32_t          matched;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif

	n = ulmk_notif_by_id(notif_id);
	if (!n || !out) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	matched = n->bits & mask;
	if (matched) {
		n->bits &= ~matched;
		*out = matched;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

	cur = ulmk_sched_current();
	if (!cur) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur->blocked_reason    = UL_BLOCKED_NOTIF;
	cur->notif_wait_mask   = mask;
	cur->blocked_notif     = notif_id;
	cur->notif_bits_outptr = out;
	cur->block_status      = 0;

	cur->state = UL_THREAD_STATE_BLOCKED;
	ulmk_sched_dequeue_locked(cur);

	n->waiter    = cur;
	n->wait_mask = mask;

	matched = n->bits & mask;
	if (matched) {
		n->bits   &= ~matched;
		n->waiter  = NULL;
		cur->state = UL_THREAD_STATE_READY;
		cur->blocked_reason = UL_BLOCKED_NONE;
		cur->blocked_notif  = ULMK_NOTIF_INVALID;
		ulmk_sched_enqueue_locked(cur);
		*out = matched;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	/* Wake writes *notif_bits_outptr; switch deferred to trap exit. */
	return 0;
}

int notif_wait_timeout_impl(ulmk_notif_t notif_id, uint32_t mask,
			    uint32_t *out, uint32_t timeout_ms)
{
	ulmk_notif_obj_t *n;
	ulmk_thread_t *cur;
	uint32_t matched;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (ulmk_ms_to_ticks(timeout_ms) == 0u)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	n = ulmk_notif_by_id(notif_id);
	if (!n || !out) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	matched = n->bits & mask;
	if (matched) {
		n->bits &= ~matched;
		*out = matched;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

	cur = ulmk_sched_current();
	if (!cur) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur->blocked_reason    = UL_BLOCKED_NOTIF;
	cur->notif_wait_mask   = mask;
	cur->blocked_notif     = notif_id;
	cur->notif_bits_outptr = out;
	cur->block_status      = 0;

	cur->state = UL_THREAD_STATE_BLOCKED;
	ulmk_sched_dequeue_locked(cur);

	n->waiter    = cur;
	n->wait_mask = mask;

	matched = n->bits & mask;
	if (matched) {
		n->bits   &= ~matched;
		n->waiter  = NULL;
		cur->state = UL_THREAD_STATE_READY;
		cur->blocked_reason = UL_BLOCKED_NONE;
		cur->blocked_notif  = ULMK_NOTIF_INVALID;
		ulmk_sched_enqueue_locked(cur);
		*out = matched;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	/*
	 * Arm after dropping ipc — never nest timer under ipc (ulmk_klock.h).
	 * If signal already woke us, the timeout cb sees non-blocked and no-ops.
	 */
	if (ulmk_timeout_arm(cur, timeout_ms, notif_wait_timeout_cb) != ULMK_OK) {
#ifndef UL_UNIT_TEST
		key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
		if (cur->state == UL_THREAD_STATE_BLOCKED &&
		    cur->blocked_reason == UL_BLOCKED_NOTIF &&
		    n->waiter == cur) {
			n->waiter = NULL;
			cur->blocked_reason    = UL_BLOCKED_NONE;
			cur->blocked_notif     = ULMK_NOTIF_INVALID;
			cur->notif_bits_outptr = NULL;
			cur->state             = UL_THREAD_STATE_READY;
			ulmk_sched_enqueue_locked(cur);
		}
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
		ulmk_sched_kick_pending();
#endif
		return -ULMK_EINVAL;
	}
	return 0;
}

uint32_t notif_poll_impl(ulmk_notif_t notif_id, uint32_t mask)
{
	ulmk_notif_obj_t *n = ulmk_notif_by_id(notif_id);
	uint32_t          matched;

	if (!n)
		return 0;

	matched  = n->bits & mask;
	n->bits &= ~matched;

	return matched;
}

int notif_destroy_impl(ulmk_notif_t notif_id)
{
	ulmk_notif_obj_t *n = ulmk_notif_by_id(notif_id);
	ulmk_thread_t    *w;

	if (!n)
		return -ULMK_EINVAL;

	w = n->waiter;
	n->waiter = NULL;
	if (w) {
		ulmk_timeout_disarm(w);
		if (w->blocked_reason == UL_BLOCKED_IPC_OR_NOTIF)
			ulmk_ep_recv_queue_remove(w);

		w->blocked_notif  = ULMK_NOTIF_INVALID;
		w->blocked_ep     = ULMK_EP_INVALID;
		w->block_status   = ULMK_EINVAL;
		w->blocked_reason = UL_BLOCKED_NONE;
		w->state          = UL_THREAD_STATE_READY;
		ulmk_sched_enqueue(w);
	}

	n->active = false;
#ifndef UL_UNIT_TEST
	ulmk_heap_free(n);
#endif
	return 0;
}

/* =========================================================================
 * Syscall ABI wrappers
 * ========================================================================= */

uint32_t ulmk_kern_notif_create(void)
{
#ifdef UL_UNIT_TEST
	uint32_t i;

	for (i = 0u; i < ULMK_CONFIG_MAX_NOTIFS; i++) {
		if (!notif_pool[i].active) {
			ulmk_notif_obj_init(&notif_pool[i], (ulmk_notif_t)i);
			return (uint32_t)i;
		}
	}
	return (uint32_t)(int32_t)(-ULMK_ENOSPC);
#else
	ulmk_notif_obj_t *n = (ulmk_notif_obj_t *)ulmk_heap_alloc(sizeof(ulmk_notif_obj_t));

	if (!n)
		return (uint32_t)ULMK_NOTIF_INVALID;
	ulmk_notif_obj_init(n, (ulmk_notif_t)(uintptr_t)n);
	return (uint32_t)(uintptr_t)n;
#endif
}

uint32_t ulmk_kern_notif_destroy(uint32_t notif_id)
{
	return (uint32_t)(int32_t)notif_destroy_impl((ulmk_notif_t)notif_id);
}

uint32_t ulmk_kern_notif_signal(uint32_t notif_id, uint32_t bits)
{
	return (uint32_t)(int32_t)notif_signal_impl((ulmk_notif_t)notif_id, bits);
}

uint32_t ulmk_kern_notif_wait(uint32_t notif_id, uint32_t mask,
			    uint32_t bits_ptr)
{
	return (uint32_t)(int32_t)notif_wait_impl(
		(ulmk_notif_t)notif_id, mask,
		(uint32_t *)(uintptr_t)bits_ptr);
}

uint32_t ulmk_kern_notif_wait_timeout(uint32_t notif_id, uint32_t mask,
				      uint32_t bits_ptr, uint32_t timeout_ms)
{
	return (uint32_t)(int32_t)notif_wait_timeout_impl(
		(ulmk_notif_t)notif_id, mask,
		(uint32_t *)(uintptr_t)bits_ptr, timeout_ms);
}

uint32_t ulmk_kern_notif_poll(uint32_t notif_id, uint32_t mask)
{
	return notif_poll_impl((ulmk_notif_t)notif_id, mask);
}
