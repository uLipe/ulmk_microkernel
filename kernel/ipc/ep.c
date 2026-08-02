/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * IPC endpoint handlers — kernel/ipc/ep.c
 * Reference: docs/api_spec.md §7
 *
 * Rendezvous model (seL4-inspired):
 *   - ep_call: caller blocks; if server waiting, wake server immediately.
 *   - ep_recv: server blocks; if caller waiting, deliver immediately.
 *   - ep_reply: wake the blocked caller; server keeps running (cooperative).
 *   - ep_reply_recv: atomic reply + re-block for next message (fast path).
 *
 * Priority inheritance:
 *   On rendezvous, the server's priority is temporarily raised to the
 *   caller's if the caller is higher-priority (lower numeric value).
 *   Applies on both paths: ep_call waking a waiting server, and ep_recv
 *   taking a waiting caller. ep_reply restores saved_prio before wake.
 *
 * Two-layer design:
 *   ep_*_impl() — core logic with native C pointer types; testable on host.
 *   ulmk_kern_ep_*() — syscall ABI wrappers; cast uint32_t args and delegate.
 *
 * Capability enforcement: deferred — any valid endpoint ID may be used.
 */

#include <stdint.h>
#include <stddef.h>
#include <ulmk/microkernel.h>
#include <kernel/include/ulmk_ep_internal.h>
#include <kernel/include/ulmk_notif_internal.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_timeout_internal.h>
#include <kernel/syscall/syscall_router.h>
#include <ulmk_arch.h>

#ifndef UL_UNIT_TEST
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_klock.h>
#else
#include <ulmk/config.h>
#endif

#ifdef UL_UNIT_TEST
ulmk_endpoint_t ep_pool[ULMK_CONFIG_MAX_ENDPOINTS];
#endif

int ulmk_ep_init(ulmk_endpoint_t *ep, ulmk_ep_t id)
{
	ep->id     = id;
	ep->active = true;
	sys_dlist_init(&ep->send_queue);
	sys_dlist_init(&ep->recv_queue);
	return 0;
}

ulmk_endpoint_t *ulmk_ep_by_id(ulmk_ep_t id)
{
#ifdef UL_UNIT_TEST
	/*
	 * Unit test mode: integer IDs index into the static pool.
	 * ep_pool is accessible to tests via extern.
	 */
	if (id == ULMK_EP_INVALID || (uint32_t)id >= ULMK_CONFIG_MAX_ENDPOINTS)
		return NULL;
	if (!ep_pool[(uint32_t)id].active)
		return NULL;
	return &ep_pool[(uint32_t)id];
#else
	ulmk_endpoint_t *ep = (ulmk_endpoint_t *)(uintptr_t)id;

	if (!ep || !ep->active)
		return NULL;
	return ep;
#endif
}

void ulmk_ep_recv_queue_remove(ulmk_thread_t *th)
{
	if (th->blocked_ep == ULMK_EP_INVALID)
		return;

	if (sys_dnode_is_linked(&th->ipc_node)) {
		sys_dlist_remove(&th->ipc_node);
		sys_dnode_init(&th->ipc_node);
	}
	th->blocked_ep = ULMK_EP_INVALID;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static inline __attribute__((always_inline))
ulmk_thread_t *ipc_pop_head(sys_dlist_t *queue)
{
	sys_dnode_t     *dn;
	ulmk_thread_t   *th;

	dn = sys_dlist_get(queue);
	if (!dn)
		return NULL;

	th = SYS_DLIST_CONTAINER_OF(dn, ulmk_thread_t, ipc_node);
	sys_dnode_init(&th->ipc_node);
	return th;
}

static inline __attribute__((always_inline))
void ipc_enqueue_tail(sys_dlist_t *queue, ulmk_thread_t *th)
{
	sys_dlist_append(queue, &th->ipc_node);
}

static inline __attribute__((always_inline))
void apply_prio_inherit(ulmk_thread_t *server, ulmk_thread_t *caller)
{
	server->saved_prio = server->priority;
	if (caller->priority < server->priority)
		server->priority = caller->priority;
}

static inline __attribute__((always_inline))
void clear_server_block(ulmk_thread_t *server)
{
	server->blocked_reason = UL_BLOCKED_NONE;
	server->blocked_ep     = ULMK_EP_INVALID;

	if (server->blocked_notif != ULMK_NOTIF_INVALID) {
		ulmk_notif_obj_t *n = ulmk_notif_by_id(server->blocked_notif);

		if (n && n->waiter == server)
			n->waiter = NULL;
		server->blocked_notif = ULMK_NOTIF_INVALID;
	}
}

/* Caller's request buffer: staged pointer, else TCB bounce. */
static inline __attribute__((always_inline))
const ulmk_msg_t *caller_request(const ulmk_thread_t *caller)
{
	if (caller->ipc_msg_outptr)
		return caller->ipc_msg_outptr;
	return &caller->ipc_msg;
}

/*
 * Deliver one request copy into the waiting server and apply PI.
 * Prefer the server's userspace outptr (single hop); fall back to TCB
 * bounce for recv_or_notif / tests.  Does not touch the ready queue.
 * Fills rn_result_outptr when the waiter used ep_recv_or_notif.
 */
static inline __attribute__((always_inline))
void prepare_server_delivery(ulmk_thread_t *server,
			     ulmk_thread_t *caller,
			     const ulmk_msg_t *msg)
{
	ulmk_recv_or_notif_result_t *rn;

	server->ipc_sender = caller->tid;
	apply_prio_inherit(server, caller);
	clear_server_block(server);

	/*
	 * Copy through the TCB as well as the userspace outptr so a
	 * cross-CPU writer never depends solely on the remote stack window
	 * remaining mapped in the caller's PMP view.
	 */
	server->ipc_msg = *msg;
	if (server->ipc_msg_outptr) {
		*server->ipc_msg_outptr = *msg;
		server->ipc_msg_outptr  = NULL;
	}

	if (server->ipc_sender_outptr) {
		*server->ipc_sender_outptr = caller->tid;
		server->ipc_sender_outptr  = NULL;
	}

	rn = server->rn_result_outptr;
	if (rn) {
		rn->is_notif    = 0;
		rn->msg         = *msg;
		rn->sender      = caller->tid;
		rn->notif_bits  = 0;
		server->rn_result_outptr = NULL;
	}
}

static void wake_blocked_on_destroy(ulmk_thread_t *th, int status)
{
	ulmk_timeout_disarm(th);

	if (sys_dnode_is_linked(&th->ipc_node)) {
		sys_dlist_remove(&th->ipc_node);
		sys_dnode_init(&th->ipc_node);
	}

	if (th->blocked_notif != ULMK_NOTIF_INVALID) {
		ulmk_notif_obj_t *n = ulmk_notif_by_id(th->blocked_notif);

		if (n && n->waiter == th)
			n->waiter = NULL;
		th->blocked_notif = ULMK_NOTIF_INVALID;
	}

	th->block_status     = status;
	th->blocked_reason   = UL_BLOCKED_NONE;
	th->blocked_ep       = ULMK_EP_INVALID;
	th->state            = UL_THREAD_STATE_READY;
	ulmk_sched_enqueue(th);
}

static void ep_call_timeout_cb(struct ulmk_timeout *to)
{
	ulmk_thread_t *th =
		SYS_DLIST_CONTAINER_OF(to, ulmk_thread_t, timeout);
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	if (!th || th->state != UL_THREAD_STATE_BLOCKED ||
	    th->blocked_reason != UL_BLOCKED_IPC_CALL)
		goto out;

	ulmk_ep_recv_queue_remove(th);
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

/* =========================================================================
 * Core implementation (_impl functions — native pointer types)
 * ========================================================================= */

int ep_call_impl(ulmk_ep_t ep_id, ulmk_msg_t *msg)
{
	ulmk_endpoint_t *ep;
	ulmk_thread_t   *cur;
	ulmk_thread_t   *srv = NULL;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!msg)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	ep = ulmk_ep_by_id(ep_id);
	if (!ep) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur = ulmk_sched_current();
	if (!cur) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur->ipc_sender     = ULMK_TID_INVALID;
	cur->blocked_reason = UL_BLOCKED_IPC_CALL;
	cur->blocked_ep     = ep_id;
	/* Stage request/reply in the caller's userspace buffer (no TCB copy). */
	cur->ipc_msg_outptr = msg;
	cur->block_status   = 0;

	if (!sys_dlist_is_empty(&ep->recv_queue)) {
		/*
		 * Fast path: server already waiting — deliver now; enqueue the
		 * server only after dropping the IPC lock so a remote resume
		 * into ep_reply cannot deadlock (spin with IRQs off on ipc).
		 */
		srv = ipc_pop_head(&ep->recv_queue);
		prepare_server_delivery(srv, cur, msg);
		srv->state = UL_THREAD_STATE_READY;
	} else {
		ipc_enqueue_tail(&ep->send_queue, cur);
	}

	cur->state = UL_THREAD_STATE_BLOCKED;
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	/* RQ ops outside IPC — IRQs masked at the gateway until trap exit. */
	ulmk_sched_dequeue(cur);
	if (srv)
		ulmk_sched_enqueue(srv);
#ifndef UL_UNIT_TEST
	ulmk_sched_kick_pending();
#endif
	/*
	 * Reply (or destroy error) is resolved after trap-exit switch: reply
	 * writes *ipc_msg_outptr; ulmk_kern_syscall_ret_resolve() picks up
	 * block_status on resume.
	 */
	return 0;
}

int ep_call_timeout_impl(ulmk_ep_t ep_id, ulmk_msg_t *msg,
			 uint32_t timeout_ms)
{
	ulmk_endpoint_t *ep;
	ulmk_thread_t *cur;
	ulmk_thread_t *srv = NULL;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!msg || ulmk_ms_to_ticks(timeout_ms) == 0u)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	ep = ulmk_ep_by_id(ep_id);
	if (!ep) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur = ulmk_sched_current();
	if (!cur) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur->ipc_sender     = ULMK_TID_INVALID;
	cur->blocked_reason = UL_BLOCKED_IPC_CALL;
	cur->blocked_ep     = ep_id;
	cur->ipc_msg_outptr = msg;
	cur->block_status   = 0;

	if (!sys_dlist_is_empty(&ep->recv_queue)) {
		srv = ipc_pop_head(&ep->recv_queue);
		prepare_server_delivery(srv, cur, msg);
		srv->state = UL_THREAD_STATE_READY;
	} else {
		ipc_enqueue_tail(&ep->send_queue, cur);
	}

	cur->state = UL_THREAD_STATE_BLOCKED;
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	if (ulmk_timeout_arm(cur, timeout_ms, ep_call_timeout_cb) != ULMK_OK) {
#ifndef UL_UNIT_TEST
		key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
		/*
		 * Abort only if still queued as caller.  If the server already
		 * took the request, leave the rendezvous without a timeout.
		 */
		if (cur->state == UL_THREAD_STATE_BLOCKED &&
		    cur->blocked_reason == UL_BLOCKED_IPC_CALL &&
		    sys_dnode_is_linked(&cur->ipc_node)) {
			ulmk_ep_recv_queue_remove(cur);
			cur->blocked_reason = UL_BLOCKED_NONE;
			cur->ipc_msg_outptr = NULL;
			cur->state          = UL_THREAD_STATE_READY;
			ulmk_sched_enqueue_locked(cur);
		}
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		ulmk_sched_dequeue(cur);
		if (srv)
			ulmk_sched_enqueue(srv);
#ifndef UL_UNIT_TEST
		ulmk_sched_kick_pending();
#endif
		return -ULMK_EINVAL;
	}
	ulmk_sched_dequeue(cur);
	if (srv)
		ulmk_sched_enqueue(srv);
#ifndef UL_UNIT_TEST
	ulmk_sched_kick_pending();
#endif
	return 0;
}

int ep_recv_impl(ulmk_ep_t ep_id, ulmk_msg_t *msg, ulmk_tid_t *sender)
{
	ulmk_endpoint_t *ep;
	ulmk_thread_t   *cur;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!msg)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	ep = ulmk_ep_by_id(ep_id);
	if (!ep) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	cur = ulmk_sched_current();
	if (!cur) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	if (!sys_dlist_is_empty(&ep->send_queue)) {
		ulmk_thread_t *caller = ipc_pop_head(&ep->send_queue);

		*msg = *caller_request(caller);
		cur->ipc_sender = caller->tid;
		apply_prio_inherit(cur, caller);

		if (sender)
			*sender = caller->tid;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

	cur->ipc_msg_outptr    = msg;
	cur->ipc_sender_outptr = sender;
	cur->blocked_reason    = UL_BLOCKED_IPC_RECV;
	cur->blocked_ep        = ep_id;
	cur->block_status      = 0;
	ipc_enqueue_tail(&ep->recv_queue, cur);

	cur->state = UL_THREAD_STATE_BLOCKED;
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	ulmk_sched_dequeue(cur);
#ifndef UL_UNIT_TEST
	ulmk_sched_kick_pending();
#endif
	/* Delivery / errors completed at wake + trap-exit resume. */
	return 0;
}

int ep_reply_impl(ulmk_tid_t sender_tid, const ulmk_msg_t *reply)
{
	ulmk_thread_t *cur    = ulmk_sched_current();
	ulmk_thread_t *caller;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!reply)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	caller = ulmk_thread_by_tid(sender_tid);
	if (!caller || caller->blocked_reason != UL_BLOCKED_IPC_CALL) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	if (cur)
		cur->priority = cur->saved_prio;

	/* One hop into the caller's staged buffer (or TCB bounce fallback). */
	if (caller->ipc_msg_outptr) {
		*caller->ipc_msg_outptr = *reply;
		caller->ipc_msg_outptr  = NULL;
	} else {
		caller->ipc_msg = *reply;
	}

	caller->state          = UL_THREAD_STATE_READY;
	caller->blocked_reason = UL_BLOCKED_NONE;
	caller->blocked_ep     = ULMK_EP_INVALID;
#ifndef UL_UNIT_TEST
	/*
	 * Enqueue after dropping IPC — same rule as ep_call waking a server.
	 * Holding IPC across rq_lock + remote IPI deadlocks with a peer in
	 * ep_call (IPC then RQ) and inflates cross-CPU reply WCET.
	 * Disarm after unlock — never nest timer under ipc.
	 */
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
	ulmk_timeout_disarm(caller);
	ulmk_sched_enqueue(caller);
#ifndef UL_UNIT_TEST
	ulmk_sched_kick_pending();
#endif

	return 0;
}

int ep_reply_recv_impl(ulmk_ep_t ep_id, ulmk_tid_t sender_tid,
		       const ulmk_msg_t *reply, ulmk_msg_t *next,
		       ulmk_tid_t *next_sender)
{
	ulmk_endpoint_t *ep;
	ulmk_thread_t   *cur;
	ulmk_thread_t   *caller;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!next)
		return -ULMK_EINVAL;

	if (sender_tid != ULMK_TID_INVALID)
		ep_reply_impl(sender_tid, reply);

	/*
	 * Hot path: next client already waiting — deliver without blocking.
	 * Must hold g_ulmk_lock_ipc: ep_call enqueues under the same lock;
	 * an unlocked pop races multi-CPU clients into a corrupted send_queue.
	 */
	ep = ulmk_ep_by_id(ep_id);
	if (!ep)
		return -ULMK_EINVAL;

	cur = ulmk_sched_current();
	if (!cur)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	if (!sys_dlist_is_empty(&ep->send_queue)) {
		caller = ipc_pop_head(&ep->send_queue);
		*next = *caller_request(caller);
		cur->ipc_sender = caller->tid;
		apply_prio_inherit(cur, caller);
		if (next_sender)
			*next_sender = caller->tid;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif

	return ep_recv_impl(ep_id, next, next_sender);
}

int ep_grant_impl(ulmk_ep_t ep_id, ulmk_tid_t target_tid)
{
	if (!ulmk_ep_by_id(ep_id))
		return -ULMK_EINVAL;
	if (!ulmk_thread_by_tid(target_tid))
		return -ULMK_EINVAL;
	return 0;
}

int ep_recv_or_notif_impl(ulmk_ep_t ep_id, ulmk_notif_t notif_id,
			  uint32_t mask, ulmk_recv_or_notif_result_t *res)
{
	ulmk_endpoint_t  *ep;
	ulmk_notif_obj_t *n;
	ulmk_thread_t    *cur;
	uint32_t        matched;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!res)
		return -ULMK_EINVAL;

	ep = ulmk_ep_by_id(ep_id);
	if (!ep)
		return -ULMK_EINVAL;

	n = ulmk_notif_by_id(notif_id);
	if (!n)
		return -ULMK_EINVAL;

	cur = ulmk_sched_current();
	if (!cur)
		return -ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	matched = n->bits & mask;
	if (matched) {
		n->bits         &= ~matched;
		res->is_notif    = 1;
		res->notif_bits  = matched;
		res->sender      = ULMK_TID_INVALID;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 1;
	}

	if (!sys_dlist_is_empty(&ep->send_queue)) {
		ulmk_thread_t *caller = ipc_pop_head(&ep->send_queue);

		apply_prio_inherit(cur, caller);
		cur->ipc_sender = caller->tid;
		cur->ipc_msg    = *caller_request(caller);

		res->is_notif = 0;
		res->msg      = cur->ipc_msg;
		res->sender   = cur->ipc_sender;
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return 0;
	}

	cur->ipc_sender        = ULMK_TID_INVALID;
	cur->blocked_reason    = UL_BLOCKED_IPC_OR_NOTIF;
	cur->blocked_ep        = ep_id;
	cur->blocked_notif     = notif_id;
	cur->notif_wait_mask   = mask;
	cur->ipc_msg_outptr    = NULL;
	cur->ipc_sender_outptr = NULL;
	cur->rn_result_outptr  = res;
	cur->block_status      = 0;

	ipc_enqueue_tail(&ep->recv_queue, cur);
	n->waiter    = cur;
	n->wait_mask = mask;

	cur->state = UL_THREAD_STATE_BLOCKED;
	ulmk_sched_dequeue_locked(cur);
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
	ulmk_sched_kick_pending();
#endif
	/*
	 * Wake paths fill *rn_result_outptr (IPC via prepare_server_delivery,
	 * notif via notif_signal).  Return 0 here; userspace sees the staged
	 * result after trap-exit resume.  Destroy errors use block_status.
	 */
	return 0;
}

int ep_destroy_impl(ulmk_ep_t ep_id)
{
	ulmk_endpoint_t *ep;
	ulmk_thread_t   *th;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	ep = ulmk_ep_by_id(ep_id);
	if (!ep) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		return -ULMK_EINVAL;
	}

	ep->active = false;

	while ((th = ipc_pop_head(&ep->send_queue)) != NULL) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		wake_blocked_on_destroy(th, ULMK_EINVAL);
#ifndef UL_UNIT_TEST
		key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	}

	while ((th = ipc_pop_head(&ep->recv_queue)) != NULL) {
#ifndef UL_UNIT_TEST
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
#endif
		wake_blocked_on_destroy(th, ULMK_EINVAL);
#ifndef UL_UNIT_TEST
		key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_ipc);
#endif
	}

#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_ipc, key);
	ulmk_heap_free(ep);
	ulmk_sched_kick_pending();
#endif
	return 0;
}

/* =========================================================================
 * Syscall ABI wrappers — cast uint32_t args, delegate to _impl.
 * ========================================================================= */

uint32_t ulmk_kern_ep_create(void)
{
#ifdef UL_UNIT_TEST
	uint32_t i;

	for (i = 0u; i < ULMK_CONFIG_MAX_ENDPOINTS; i++) {
		if (!ep_pool[i].active) {
			ulmk_ep_init(&ep_pool[i], (ulmk_ep_t)i);
			return (uint32_t)i;
		}
	}
	return (uint32_t)(int32_t)(-ULMK_ENOSPC);
#else
	ulmk_endpoint_t *ep = (ulmk_endpoint_t *)ulmk_heap_alloc(sizeof(ulmk_endpoint_t));

	if (!ep)
		return (uint32_t)ULMK_EP_INVALID;
	ulmk_ep_init(ep, (ulmk_ep_t)(uintptr_t)ep);
	return (uint32_t)(uintptr_t)ep;
#endif
}

uint32_t ulmk_kern_ep_call(uint32_t ep_id, uint32_t msg_ptr)
{
	return (uint32_t)(int32_t)ep_call_impl(
		(ulmk_ep_t)ep_id,
		(ulmk_msg_t *)(uintptr_t)msg_ptr);
}

uint32_t ulmk_kern_ep_call_timeout(uint32_t ep_id, uint32_t msg_ptr,
				   uint32_t timeout_ms)
{
	return (uint32_t)(int32_t)ep_call_timeout_impl(
		(ulmk_ep_t)ep_id,
		(ulmk_msg_t *)(uintptr_t)msg_ptr,
		timeout_ms);
}

uint32_t ulmk_kern_ep_recv(uint32_t ep_id, uint32_t msg_ptr,
			 uint32_t sender_ptr)
{
	return (uint32_t)(int32_t)ep_recv_impl(
		(ulmk_ep_t)ep_id,
		(ulmk_msg_t  *)(uintptr_t)msg_ptr,
		(ulmk_tid_t  *)(uintptr_t)sender_ptr);
}

uint32_t ulmk_kern_ep_reply(uint32_t sender_tid_u32, uint32_t reply_ptr)
{
	return (uint32_t)(int32_t)ep_reply_impl(
		(ulmk_tid_t)sender_tid_u32,
		(const ulmk_msg_t *)(uintptr_t)reply_ptr);
}

uint32_t ulmk_kern_ep_reply_recv(uint32_t ep_id, uint32_t sender_tid_u32,
			       uint32_t args_ptr)
{
	const ulmk_reply_recv_args_t *args =
		(const ulmk_reply_recv_args_t *)(uintptr_t)args_ptr;

	if (!args)
		return (uint32_t)(int32_t)(-ULMK_EINVAL);

	return (uint32_t)(int32_t)ep_reply_recv_impl(
		(ulmk_ep_t)ep_id,
		(ulmk_tid_t)sender_tid_u32,
		args->reply, args->next, args->next_sender);
}

uint32_t ulmk_kern_ep_grant(uint32_t ep_id, uint32_t target_tid)
{
	return (uint32_t)(int32_t)ep_grant_impl(
		(ulmk_ep_t)ep_id, (ulmk_tid_t)target_tid);
}

uint32_t ulmk_kern_ep_recv_or_notif(uint32_t ep_id, uint32_t notif_id,
				  uint32_t mask, uint32_t result_ptr)
{
	return (uint32_t)(int32_t)ep_recv_or_notif_impl(
		(ulmk_ep_t)ep_id,
		(ulmk_notif_t)notif_id,
		mask,
		(ulmk_recv_or_notif_result_t *)(uintptr_t)result_ptr);
}

uint32_t ulmk_kern_ep_destroy(uint32_t ep_id)
{
	return (uint32_t)(int32_t)ep_destroy_impl((ulmk_ep_t)ep_id);
}
