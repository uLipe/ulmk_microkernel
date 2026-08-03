/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Kernel syscall router — internal header
 * Not part of the public API; included only by kernel/syscall/syscall_router.c
 * and by kernel/kernel_main.c.
 */

#ifndef ULMK_SYSCALL_ROUTER_H
#define ULMK_SYSCALL_ROUTER_H

#include <stdint.h>

/*
 * ulmk_syscall_router — dispatch table for all kernel syscall handlers.
 *
 * Called by ulmk_kernel_syscall_trap() after reading D15 (nr) and D4-D7
 * (a0..a3) from the caller's live registers.
 *
 * Returns the value to be placed in D2 (the caller's return register).
 * On privilege violations the router itself returns (uint32_t)ULMK_EPERM.
 */
uint32_t ulmk_syscall_router(uint32_t nr,
			   uint32_t a0, uint32_t a1,
			   uint32_t a2, uint32_t a3);

/* ──────────────────────────────────────────────────────────────────
 * Kernel handler prototypes — implemented in their respective .c files.
 * Convention: ulmk_kern_<subsystem>_<verb>(a0, a1, a2, a3) → uint32_t
 * The router passes raw uint32_t; handlers cast as needed.
 * ────────────────────────────────────────────────────────────────── */

/* Memory */
uint32_t ulmk_kern_mem_map(uint32_t hint, uint32_t size,
			 uint32_t perms, uint32_t flags);
uint32_t ulmk_kern_mem_unmap(uint32_t addr, uint32_t size);
uint32_t ulmk_kern_mem_grant(uint32_t addr, uint32_t size,
			   uint32_t target_tid, uint32_t perms);
uint32_t ulmk_kern_mem_revoke(uint32_t addr, uint32_t target_tid);
/* Per-thread heap (slabAO model) */
uint32_t ulmk_kern_heap_extend(uint32_t size);
uint32_t ulmk_kern_get_thread_heap(uint32_t info_ptr);
/* Kernel-internal heap helpers (not exposed as syscalls) */
uint32_t ulmk_kern_heap_alloc(uint32_t size);
uint32_t ulmk_kern_heap_free(uint32_t ptr);
uint32_t ulmk_kern_heap_aligned_alloc(uint32_t align, uint32_t size);

/* Scheduling */
uint32_t ulmk_kern_yield(void);
uint32_t ulmk_kern_exit(void);         /* does not return; marks thread dead */
uint32_t ulmk_kern_thread_self(void);
uint32_t ulmk_kern_sleep(uint32_t ms);
uint32_t ulmk_kern_sleep_cancel(uint32_t tid);
uint32_t ulmk_kern_tick_start(void);

/* IPC endpoints */
uint32_t ulmk_kern_ep_create(void);
uint32_t ulmk_kern_ep_call(uint32_t ep, uint32_t msg_ptr);
uint32_t ulmk_kern_ep_call_timeout(uint32_t ep, uint32_t msg_ptr,
				   uint32_t ms);
uint32_t ulmk_kern_ep_recv(uint32_t ep, uint32_t msg_ptr, uint32_t sender_ptr);
uint32_t ulmk_kern_ep_reply(uint32_t sender_tid, uint32_t reply_ptr);
uint32_t ulmk_kern_ep_reply_recv(uint32_t ep, uint32_t sender_tid,
			       uint32_t args_ptr);
uint32_t ulmk_kern_ep_grant(uint32_t ep, uint32_t target_tid);
uint32_t ulmk_kern_ep_recv_or_notif(uint32_t ep, uint32_t notif,
				  uint32_t mask, uint32_t result_ptr);
uint32_t ulmk_kern_ep_destroy(uint32_t ep);

/* Notifications */
uint32_t ulmk_kern_notif_create(void);
uint32_t ulmk_kern_notif_signal(uint32_t notif, uint32_t bits);
uint32_t ulmk_kern_notif_wait(uint32_t notif, uint32_t mask, uint32_t bits_ptr);
uint32_t ulmk_kern_notif_wait_timeout(uint32_t notif, uint32_t mask,
				      uint32_t bits_ptr, uint32_t ms);
uint32_t ulmk_kern_notif_poll(uint32_t notif, uint32_t mask);
uint32_t ulmk_kern_notif_destroy(uint32_t notif);

/* IRQ (requires ULMK_PRIV_DRIVER) */
uint32_t ulmk_kern_irq_bind(uint32_t srpn, uint32_t notif, uint32_t bit);
uint32_t ulmk_kern_irq_bind_hw(uint32_t srpn, uint32_t notif_id,
			     uint32_t bit, uint32_t src_reg);
uint32_t ulmk_kern_irq_attach(uint32_t srpn, uint32_t fn, uint32_t data);
uint32_t ulmk_kern_irq_attach_hw(uint32_t srpn, uint32_t fn, uint32_t data,
				 uint32_t src_reg);
uint32_t ulmk_kern_irq_detach(uint32_t srpn);
uint32_t ulmk_kern_irq_enable(uint32_t srpn);
uint32_t ulmk_kern_irq_disable(uint32_t srpn);
uint32_t ulmk_kern_irq_ack(uint32_t srpn);

/* Thread management (requires ULMK_PRIV_DRIVER) */
uint32_t ulmk_kern_thread_spawn(uint32_t attr_ptr);
uint32_t ulmk_kern_thread_kill(uint32_t tid);
uint32_t ulmk_kern_thread_suspend(uint32_t tid);
uint32_t ulmk_kern_thread_resume(uint32_t tid);
uint32_t ulmk_kern_thread_set_prio(uint32_t tid, uint32_t prio);
uint32_t ulmk_kern_thread_get_prio(uint32_t tid);

/* Capability management */
uint32_t ulmk_kern_cap_grant(uint32_t target_tid, uint32_t caps);

#endif /* ULMK_SYSCALL_ROUTER_H */
