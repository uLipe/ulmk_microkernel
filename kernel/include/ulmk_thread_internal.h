/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Internal thread management types and interface.
 * Not part of the public API — do not include from userspace.
 */

#ifndef UL_THREAD_INTERNAL_H
#define UL_THREAD_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <ulmk_arch.h>
#include <kernel/include/list.h>
#include <kernel/include/ulmk_timer.h>
#include <kernel/include/ulmk_area_internal.h>

struct ulmk_syscall_wcet_slot;

#define UL_THREAD_STATE_DEAD      0
#define UL_THREAD_STATE_READY     1
#define UL_THREAD_STATE_RUNNING   2
#define UL_THREAD_STATE_BLOCKED   3
#define UL_THREAD_STATE_SUSPENDED 4

/* Reason a thread is in UL_THREAD_STATE_BLOCKED. */
#define UL_BLOCKED_NONE          0
#define UL_BLOCKED_IPC_CALL      2  /* waiting for server reply */
#define UL_BLOCKED_IPC_RECV      3  /* waiting for a caller */
#define UL_BLOCKED_NOTIF         4  /* waiting for notification bits */
#define UL_BLOCKED_IPC_OR_NOTIF  5  /* ulmk_ep_recv_or_notif — either */
#define UL_BLOCKED_SLEEP         6

typedef struct ulmk_thread {
	ulmk_arch_ctx_t    ctx;
	uint8_t         *stack_base;
	size_t           stack_size;
	/*
	 * Heap block holding the stack; NULL for static threads (idle, root),
	 * whose TCB and stack live in linker-reserved sections.
	 */
	void            *stack_alloc;
	uint8_t          priority;
	uint8_t          cpu;             /* permanent affinity (0..NR_CPUS-1) */
	uint8_t          saved_prio;      /* priority before inheritance boost */
	uint8_t          state;
	uint8_t          blocked_reason;
	ulmk_privilege_t   privilege;
#if ULMK_CONFIG_ENABLE_SMP
	/*
	 * Lazy arch ctx: TriCore CSAs must be carved from the affinity CPU's
	 * FCX.  Remote create stores entry/arg and fabricates on first
	 * schedule on that CPU (see ulmk_thread_ensure_ctx).
	 */
	void           (*start_entry)(void *arg);
	void            *start_arg;
	uint8_t          ctx_ready;
#endif
	ulmk_tid_t         tid;	/* opaque handle: (uintptr_t)this */
	ulmk_ep_t          blocked_ep;      /* ep blocked on; for cleanup on kill */
	ulmk_notif_t       blocked_notif;   /* notif blocked on; recv_or_notif */
	sys_dnode_t        sched_node;      /* run-queue linkage */
	sys_dnode_t        ipc_node;        /* IPC send or recv queue linkage */
	sys_dnode_t        reg_node;        /* global TCB registry linkage */
	struct ulmk_timeout timeout;
	/*
	 * Bounce buffer — fallback when the peer has no userspace staging
	 * pointer (recv_or_notif result, destroy paths, unit tests).  Hot
	 * paths stage via ipc_msg_outptr to avoid TCB→TCB→user double copies.
	 */
	ulmk_msg_t          ipc_msg;
	ulmk_tid_t          ipc_sender;     /* sender TID stored by recv for reply */
	/*
	 * Userspace staging pointer:
	 *   ep_call  — in/out buffer (request on entry, reply on return)
	 *   ep_recv  — filled before wake when a caller rendezvous arrives
	 */
	ulmk_msg_t         *ipc_msg_outptr;
	ulmk_tid_t         *ipc_sender_outptr;
	uint32_t         *notif_bits_outptr;
	ulmk_recv_or_notif_result_t *rn_result_outptr;
	uint32_t          notif_wait_mask;
	uint32_t          notif_received; /* bits consumed on notif wakeup */
	/*
	 * Status returned by a blocking syscall after wakeup.
	 * 0 = normal completion; ULMK_EINVAL = object destroyed under us.
	 * Applied by ulmk_kern_syscall_ret_resolve() after trap-exit switch.
	 */
	int32_t           block_status;
	/*
	 * Optional success return override (e.g. recv_or_notif notif path → 1).
	 * Valid when syscall_wake_ret_valid != 0; cleared on resolve.
	 */
	int32_t           syscall_wake_ret;
	uint8_t           syscall_wake_ret_valid;
	/*
	 * The stack is pinned: programmed on every switch because hardware
	 * exception stacking on some ports cannot take a fault.  Everything
	 * else the thread may touch is an area, loaded by the fault handler.
	 */
	ulmk_arch_region_t  stack_region;
	struct ulmk_area_set areas;
	/*
	 * Capability bitmask — which privileged operations this thread may invoke.
	 */
	uint8_t           cap_flags;
	/*
	 * Per-thread WCET pause state (ULMK_CONFIG_SYSCALL_WCET).  Lives on the
	 * TCB so a context switch to another thread's syscall cannot clobber it.
	 */
	uint32_t          wcet_blocked;
	uint32_t          wcet_block_mark;
	uint8_t           wcet_block_open;
	/*
	 * Optional userspace slot (ulmk_wcet_bind).  When set, each syscall
	 * also publishes there so same-CPU peers cannot overwrite samples.
	 */
	volatile struct ulmk_syscall_wcet_slot *wcet_out;
} ulmk_thread_t;

int          ulmk_thread_init(ulmk_thread_t *th, const ulmk_thread_attr_t *attr,
			    void *stack);
ulmk_thread_t *ulmk_thread_by_tid(ulmk_tid_t tid);
void         ulmk_thread_set_state(ulmk_thread_t *th, uint8_t state);
void         ulmk_thread_free(ulmk_thread_t *th);
#if ULMK_CONFIG_ENABLE_SMP
void         ulmk_thread_ensure_ctx(ulmk_thread_t *th);
#else
static inline void ulmk_thread_ensure_ctx(ulmk_thread_t *th)
{
	(void)th;
}
#endif

#endif /* UL_THREAD_INTERNAL_H */
