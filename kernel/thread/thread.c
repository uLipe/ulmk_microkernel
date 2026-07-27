/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Thread lifecycle handlers — kernel/thread/thread.c
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_percpu.h>
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_ep_internal.h>
#include <kernel/include/ulmk_notif_internal.h>
#include <kernel/include/ulmk_timeout_internal.h>
#include <kernel/syscall/syscall_router.h>
#include <ulmk_arch.h>
#include <kernel/include/list.h>

/*
 * Ports that run exceptions on a private per-thread kernel stack carved from
 * the top of the thread stack (ARM Cortex-M) must reserve that carve on top of
 * the caller's requested user stack, or a small stack_size underflows into
 * adjacent memory on the first exception.  Zero on ports with a shared kernel
 * stack (TriCore CSA, RISC-V).  Static callers (idle/root) already fold this
 * reserve into their statically-sized stacks; only the dynamic allocator adds it.
 */
#ifndef ULMK_ARCH_KSTACK_SIZE
#define ULMK_ARCH_KSTACK_SIZE	0u
#endif

/*
 * When 1, remote-affinity threads defer ulmk_arch_ctx_init() until first
 * schedule on that CPU (TriCore FCX).  Default 0: fabricate at create time.
 */
#ifndef ULMK_ARCH_CTX_FABRICATE_ON_AFFINITY_CPU
#define ULMK_ARCH_CTX_FABRICATE_ON_AFFINITY_CPU	0
#endif

/*
 * Thread registry: all allocated TCBs (static + dynamic).
 * ulmk_tid_t is an opaque handle (TCB pointer); lookup is O(1) via cast.
 */
static sys_dlist_t UL_KERNEL_BSS tcb_registry;
static bool        UL_KERNEL_BSS tcb_registry_inited;
#ifdef THREAD_UNIT_TEST
static ulmk_tid_t  UL_KERNEL_BSS tid_counter;
#endif

static void tcb_registry_ensure_init(void)
{
	if (!tcb_registry_inited) {
		sys_dlist_init(&tcb_registry);
		tcb_registry_inited = true;
	}
}

int ulmk_thread_init(ulmk_thread_t *th, const ulmk_thread_attr_t *attr, void *stack)
{
	if (!th || !attr || !stack || !attr->entry)
		return ULMK_EINVAL;
	if (attr->stack_size == 0)
		return ULMK_EINVAL;

	sys_dnode_init(&th->sched_node);
	sys_dnode_init(&th->ipc_node);
	sys_dnode_init(&th->reg_node);
	sys_dnode_init(&th->timeout.node);
	th->timeout.cb = NULL;
	th->timeout.expires = 0u;

	th->stack_base  = (uint8_t *)stack;
	th->stack_size  = attr->stack_size;
	th->slab_base   = NULL;
	th->slab_size   = 0u;
	th->heap_base   = 0u;
	th->heap_size   = 0u;
	th->priority    = attr->priority;
	th->cpu         = attr->cpu;
	th->saved_prio  = attr->priority;
	th->state       = UL_THREAD_STATE_READY;
	th->blocked_reason = UL_BLOCKED_NONE;
	th->privilege   = attr->privilege;
#ifdef THREAD_UNIT_TEST
	th->tid         = ++tid_counter;
#else
	th->tid         = (ulmk_tid_t)(uintptr_t)th;
#endif
	th->blocked_ep  = ULMK_EP_INVALID;
	th->blocked_notif = ULMK_NOTIF_INVALID;
	th->ipc_sender  = ULMK_TID_INVALID;
	th->notif_received = 0u;
	th->notif_wait_mask = 0u;
	th->block_status    = 0;
	th->syscall_wake_ret = 0;
	th->syscall_wake_ret_valid = 0;
	th->ipc_msg_outptr    = NULL;
	th->ipc_sender_outptr = NULL;
	th->notif_bits_outptr  = NULL;
	th->rn_result_outptr   = NULL;
	th->wcet_out          = NULL;
	th->region_count      = 0u;

	th->cap_flags = (attr->privilege == ULMK_PRIV_KERNEL) ? ULMK_CAP_ALL : 0u;
	/*
	 * Drivers need MMIO + IRQ to bring up board servers.  Granting only
	 * after create races with the first schedule of a higher-priority
	 * driver thread; seed the minimum set at create time.
	 */
	if (attr->privilege == ULMK_PRIV_DRIVER)
		th->cap_flags |= (uint8_t)(ULMK_CAP_MAP_PERIPH | ULMK_CAP_IRQ);

	/*
	 * Every non-kernel thread gets its stack as a default R+W MPU region.
	 */
	if (attr->privilege != ULMK_PRIV_KERNEL) {
		th->regions[0].base  = (uintptr_t)stack;
		th->regions[0].size  = attr->stack_size;
		th->regions[0].perms = ULMK_PERM_READ | ULMK_PERM_WRITE;
		th->regions[0].type  = ULMK_REGION_STACK;
		th->region_count     = 1u;
	}

#if ULMK_CONFIG_ENABLE_SMP
	th->start_entry = attr->entry;
	th->start_arg   = attr->arg;
	/*
	 * Some arches (TriCore CSA/FCX) must fabricate on the affinity CPU.
	 * Others only write the thread stack — safe from the creator, and
	 * required: lazy remote ctx + concurrent remote spawns races the
	 * first schedule (instruction fault in trap epilogue on RV32).
	 */
	if (attr->cpu != (uint8_t)ulmk_arch_cpu_id() &&
	    ULMK_ARCH_CTX_FABRICATE_ON_AFFINITY_CPU) {
		th->ctx       = (ulmk_arch_ctx_t){ 0 };
		th->ctx_ready = 0u;
	} else {
		ulmk_arch_ctx_init(&th->ctx,
				   attr->entry,
				   attr->arg,
#if defined(ULMK_ARCH_STACK_GROWS_UP) && ULMK_ARCH_STACK_GROWS_UP
				   (uintptr_t)stack,
#else
				   (uintptr_t)stack + attr->stack_size,
#endif
				   attr->privilege);
		th->ctx_ready = 1u;
	}
#else
	ulmk_arch_ctx_init(&th->ctx,
			 attr->entry,
			 attr->arg,
#if defined(ULMK_ARCH_STACK_GROWS_UP) && ULMK_ARCH_STACK_GROWS_UP
			 (uintptr_t)stack,
#else
			 (uintptr_t)stack + attr->stack_size,
#endif
			 attr->privilege);
#endif

	/* Register in the TCB registry (append at tail). */
	tcb_registry_ensure_init();
	sys_dlist_append(&tcb_registry, &th->reg_node);

	return ULMK_OK;
}

#if ULMK_CONFIG_ENABLE_SMP
void ulmk_thread_ensure_ctx(ulmk_thread_t *th)
{
	if (!th || th->ctx_ready)
		return;
	if (th->cpu != (uint8_t)ulmk_arch_cpu_id())
		return;

	ulmk_arch_ctx_init(&th->ctx,
			   th->start_entry,
			   th->start_arg,
#if defined(ULMK_ARCH_STACK_GROWS_UP) && ULMK_ARCH_STACK_GROWS_UP
			   (uintptr_t)th->stack_base,
#else
			   (uintptr_t)th->stack_base + th->stack_size,
#endif
			   th->privilege);
	th->ctx_ready = 1u;
}
#endif

ulmk_thread_t *ulmk_thread_by_tid(ulmk_tid_t tid)
{
#ifdef THREAD_UNIT_TEST
	ulmk_thread_t *th;

	SYS_DLIST_FOR_EACH_CONTAINER(&tcb_registry, th, reg_node) {
		if (th->tid == tid)
			return th;
	}
	return NULL;
#else
	ulmk_thread_t *th = (ulmk_thread_t *)tid;

	if (tid == ULMK_TID_INVALID || !th)
		return NULL;
	if (th->state == UL_THREAD_STATE_DEAD)
		return NULL;
	if (!sys_dnode_is_linked(&th->reg_node))
		return NULL;
	return th;
#endif
}

void ulmk_thread_set_state(ulmk_thread_t *th, uint8_t state)
{
	th->state = state;
}

/*
 * Unlink a TCB from the global registry list and free its heap memory.
 * Stack is freed first (it was allocated from the heap), then the TCB.
 * Called only for heap-allocated threads (dynamic TCBs).
 * Static TCBs (idle, root) must never be passed to this function.
 */
void ulmk_thread_free(ulmk_thread_t *th)
{
	ulmk_timeout_disarm(th);

	if (sys_dnode_is_linked(&th->reg_node)) {
		sys_dlist_remove(&th->reg_node);
		sys_dnode_init(&th->reg_node);
	}

	/*
	 * Static threads (idle, root) have slab_base == NULL; their TCB and
	 * stack live in linker-reserved sections and must not be passed to
	 * the heap allocator.
	 */
	if (!th->slab_base)
		return;

	ulmk_heap_free(th->slab_base);
	ulmk_heap_free(th);
}

/* =========================================================================
 * Syscall handlers
 * ========================================================================= */

uint32_t ulmk_kern_thread_self(void)
{
	ulmk_thread_t *t = ulmk_sched_current();

#ifdef THREAD_UNIT_TEST
	return t ? (uint32_t)t->tid : (uint32_t)ULMK_TID_INVALID;
#else
	return t ? (uint32_t)(uintptr_t)t : (uint32_t)ULMK_TID_INVALID;
#endif
}

uint32_t ulmk_kern_yield(void)
{
	ulmk_thread_t *cur = ulmk_sched_current();

	if (cur) {
		cur->state = UL_THREAD_STATE_READY;
		ulmk_sched_dequeue(cur);
		ulmk_sched_enqueue(cur);
		/* Equal-prio FIFO rotation — enqueue alone may not preempt. */
		ulmk_sched_request_resched();
	}
	return 0;
}

/*
 * exit — mark thread dead; switch at trap exit.
 *
 * Context chain and stack cannot be freed here: the thread is still
 * executing and its context chain is live.  Register for deferred cleanup.
 */
uint32_t ulmk_kern_exit(void)
{
	ulmk_thread_t *cur = ulmk_sched_current();

	if (cur) {
		cur->state = UL_THREAD_STATE_DEAD;
		ulmk_sched_dequeue(cur);
		ulmk_sched_set_dead_for_cleanup(cur);
	}
	return 0;
}

/*
 * spawn — allocate TCB and slabAO (stack + optional heap) from user_pool,
 * initialise, and enqueue.  Returns new TID or a negative error code.
 *
 * The slabAO is a single contiguous block: [stack | heap].
 * A single DPR covers the entire slab so the thread's heap is accessible
 * via the same MPU region as its stack, with no extra DPR entries.
 * The TCB is a separate allocation to prevent userspace from reaching it.
 */
uint32_t ulmk_kern_thread_spawn(uint32_t attr_ptr)
{
	const ulmk_thread_attr_t *uattr =
		(const ulmk_thread_attr_t *)(uintptr_t)attr_ptr;
	ulmk_thread_attr_t attr;
	ulmk_thread_t *th;
	void          *slab;
	size_t         slab_size;
	size_t         heap_size;
	int            ret;

	if (!uattr || !uattr->entry || uattr->stack_size == 0)
		return (uint32_t)ULMK_TID_INVALID;

	/*
	 * Grow the requested stack by the arch kernel-stack reserve so the full
	 * user stack survives the per-thread carve (see ULMK_ARCH_KSTACK_SIZE).
	 */
	attr = *uattr;
#if !ULMK_CONFIG_ENABLE_SMP
	/*
	 * UP builds have a single RQ.  Absorb affinity quietly so apps that
	 * hard-code attr.cpu=1 (SMP demos) keep working without a rebuild.
	 */
	attr.cpu = 0u;
#else
	if (attr.cpu >= (uint8_t)ULMK_NR_CPUS)
		return (uint32_t)ULMK_TID_INVALID;
#endif
	attr.stack_size += ULMK_ARCH_KSTACK_SIZE;

	heap_size = attr.heap_size;
	slab_size = attr.stack_size + heap_size;

	th = (ulmk_thread_t *)ulmk_heap_alloc(sizeof(ulmk_thread_t));
	if (!th)
		return (uint32_t)ULMK_TID_INVALID;

	slab = ulmk_heap_alloc(slab_size);
	if (!slab) {
		ulmk_heap_free(th);
		return (uint32_t)ULMK_TID_INVALID;
	}

	ret = ulmk_thread_init(th, &attr, slab);
	if (ret != ULMK_OK) {
		ulmk_heap_free(slab);
		ulmk_heap_free(th);
		return (uint32_t)ULMK_TID_INVALID;
	}

	th->slab_base = slab;
	th->slab_size = slab_size;
	th->heap_base = (uintptr_t)slab + attr.stack_size;
	th->heap_size = heap_size;

	/*
	 * Extend the single DPR to cover the full slabAO (stack + heap)
	 * when the thread has a heap.  ulmk_thread_init already set
	 * regions[0] to cover just the stack.
	 */
	if (heap_size > 0u && attr.privilege != ULMK_PRIV_KERNEL) {
		th->regions[0].base = (uintptr_t)slab;
		th->regions[0].size = slab_size;
	}

	ulmk_sched_enqueue(th);
#ifdef THREAD_UNIT_TEST
	return (uint32_t)th->tid;
#else
	return (uint32_t)(uintptr_t)th;
#endif
}

uint32_t ulmk_kern_get_thread_heap(uint32_t info_ptr)
{
	ulmk_thread_t    *cur  = ulmk_sched_current();
	ulmk_heap_info_t *info = (ulmk_heap_info_t *)(uintptr_t)info_ptr;

	if (!cur || !info)
		return (uint32_t)(int32_t)ULMK_EINVAL;
	if (cur->heap_size == 0u)
		return (uint32_t)(int32_t)ULMK_EPERM;

	info->base = cur->heap_base;
	info->size = cur->heap_size;
	return (uint32_t)ULMK_OK;
}

uint32_t ulmk_kern_thread_kill(uint32_t tid)
{
	ulmk_thread_t *th = ulmk_thread_by_tid((ulmk_tid_t)tid);
	ulmk_thread_t *peer;

	if (!th || th->state == UL_THREAD_STATE_DEAD)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	ulmk_timeout_disarm(th);

	/*
	 * Drop from IPC queues (send or recv share ipc_node) and clear any
	 * notif waiter slot before the TCB disappears.
	 */
	if (th->blocked_reason == UL_BLOCKED_IPC_CALL ||
	    th->blocked_reason == UL_BLOCKED_IPC_RECV ||
	    th->blocked_reason == UL_BLOCKED_IPC_OR_NOTIF)
		ulmk_ep_recv_queue_remove(th);

	if (th->blocked_notif != ULMK_NOTIF_INVALID) {
		ulmk_notif_obj_t *n = ulmk_notif_by_id(th->blocked_notif);

		if (n && n->waiter == th)
			n->waiter = NULL;
		th->blocked_notif = ULMK_NOTIF_INVALID;
	}

	/*
	 * Mid-rendezvous: this thread already received a call and has not
	 * replied — unblock the caller so it does not wait forever.
	 */
	if (th->ipc_sender != ULMK_TID_INVALID) {
		peer = ulmk_thread_by_tid(th->ipc_sender);
		if (peer && peer->state == UL_THREAD_STATE_BLOCKED &&
		    peer->blocked_reason == UL_BLOCKED_IPC_CALL) {
			if (sys_dnode_is_linked(&peer->ipc_node)) {
				sys_dlist_remove(&peer->ipc_node);
				sys_dnode_init(&peer->ipc_node);
			}
			ulmk_timeout_disarm(peer);
			peer->block_status   = ULMK_ESRCH;
			peer->blocked_reason = UL_BLOCKED_NONE;
			peer->blocked_ep     = ULMK_EP_INVALID;
			peer->state          = UL_THREAD_STATE_READY;
			ulmk_sched_enqueue(peer);
		}
		th->ipc_sender = ULMK_TID_INVALID;
	}

	th->state = UL_THREAD_STATE_DEAD;
	ulmk_sched_dequeue(th);

	if (th != ulmk_sched_current()) {
		ulmk_arch_ctx_free(&th->ctx);
		ulmk_thread_free(th);
	} else {
		/* Trap exit switches away (state == DEAD). */
		ulmk_sched_set_dead_for_cleanup(th);
	}

	return 0;
}

uint32_t ulmk_kern_thread_suspend(uint32_t tid)
{
	ulmk_thread_t *th = ulmk_thread_by_tid((ulmk_tid_t)tid);

	if (!th)
		return (uint32_t)(int32_t)ULMK_ESRCH;
	if (th->state == UL_THREAD_STATE_DEAD)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	th->state = UL_THREAD_STATE_SUSPENDED;
	ulmk_sched_dequeue(th);
	/* Self-suspend: trap exit sees !RUNNING and switches. */

	return 0;
}

uint32_t ulmk_kern_thread_resume(uint32_t tid)
{
	ulmk_thread_t *th = ulmk_thread_by_tid((ulmk_tid_t)tid);

	if (!th)
		return (uint32_t)(int32_t)ULMK_ESRCH;
	if (th->state != UL_THREAD_STATE_SUSPENDED)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	ulmk_sched_enqueue(th);
	return 0;
}

uint32_t ulmk_kern_thread_set_prio(uint32_t tid, uint32_t prio)
{
	ulmk_thread_t *th = ulmk_thread_by_tid((ulmk_tid_t)tid);

	if (!th)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	if (th->state == UL_THREAD_STATE_READY) {
		ulmk_sched_dequeue(th);
		th->priority = (uint8_t)prio;
		ulmk_sched_enqueue(th);
	} else {
		th->priority = (uint8_t)prio;
	}
	return 0;
}

uint32_t ulmk_kern_thread_get_prio(uint32_t tid)
{
	ulmk_thread_t *th = ulmk_thread_by_tid((ulmk_tid_t)tid);

	if (!th || th->state == UL_THREAD_STATE_DEAD)
		return (uint32_t)(int32_t)ULMK_ESRCH;
	return (uint32_t)th->priority;
}

uint32_t ulmk_kern_cap_grant(uint32_t target_tid, uint32_t caps)
{
	ulmk_thread_t *cur    = ulmk_sched_current();
	ulmk_thread_t *target = ulmk_thread_by_tid((ulmk_tid_t)target_tid);

	if (!cur || !target)
		return (uint32_t)(int32_t)ULMK_ESRCH;

	if ((cur->cap_flags & (uint8_t)caps) != (uint8_t)caps)
		return (uint32_t)(int32_t)ULMK_EPERM;

	target->cap_flags |= (uint8_t)caps;
	return (uint32_t)ULMK_OK;
}
