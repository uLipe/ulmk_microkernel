/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Syscall numbers — ulmk
 * Single source of truth for both userspace wrappers and the kernel router.
 * Full specification: docs/api_spec.md §12
 *
 * Numbering rationale (sparse on purpose — room for additions per group):
 *   1–9   Memory
 *   10–19 Scheduling / exit
 *   20–29 Thread query (unprivileged)
 *   30–39 IPC endpoints
 *   40–49 Notifications
 *   60–69 IRQ (IO >= 1)
 *   70–79 Thread management (IO >= 1)
 *   80–89 Process management (IO >= 1)
 */

#ifndef ULMK_SYSCALL_NR_H
#define ULMK_SYSCALL_NR_H

/* ── Memory ──────────────────────────────────────────────────────── */
#define ULMK_SYS_MMAP                 1  /* void *ulmk_mem_map(hint, sz, perms, flags) */
#define ULMK_SYS_MUNMAP               2  /* int   ulmk_mem_unmap(addr, sz)            */
#define ULMK_SYS_MEM_GRANT            3  /* int   ulmk_mem_grant(addr, sz, tid, perms)*/
/* D-cache maintenance (DRIVER; ULMK_ENOTSUP if !ULMK_ARCH_HAS_CACHE) */
#define ULMK_SYS_DCACHE_CLEAN         4  /* int ulmk_dcache_clean(addr, size)         */
#define ULMK_SYS_DCACHE_INVALIDATE    5  /* int ulmk_dcache_invalidate(addr, size)    */
#define ULMK_SYS_DCACHE_CLEAN_INV     6  /* int ulmk_dcache_clean_invalidate(...)     */
#define ULMK_SYS_HEAP_EXTEND          7  /* int ulmk_heap_extend(size)                */
#define ULMK_SYS_GET_THREAD_HEAP      8  /* int ulmk_get_thread_heap(info*)           */

/* ── Scheduling / exit ───────────────────────────────────────────── */
#define ULMK_SYS_YIELD               10  /* void  ulmk_thread_yield(void)             */
#define ULMK_SYS_EXIT                11  /* void  ulmk_thread_exit(void) [noreturn]   */
#define ULMK_SYS_SLEEP               12  /* int   ulmk_sleep_ms(ms)                   */
#define ULMK_SYS_SLEEP_CANCEL        13  /* int   ulmk_sleep_cancel(tid)              */
#define ULMK_SYS_EP_CALL_TIMEOUT     14  /* int   ulmk_ep_call_timeout(ep,msg*,ms)    */
#define ULMK_SYS_TICK_START          15  /* void  ulmk_tick_start(void) — board init  */

/* ── Thread query (any privilege) ───────────────────────────────── */
#define ULMK_SYS_THREAD_SELF         20  /* ulmk_tid_t ulmk_thread_self(void)           */
#define ULMK_SYS_CPU_ID              21  /* uint32_t ulmk_cpu_id(void)                */
#define ULMK_SYS_WCET_BIND           22  /* int ulmk_wcet_bind(slot*)                 */

/* ── IPC endpoints ───────────────────────────────────────────────── */
#define ULMK_SYS_EP_CREATE           30  /* ulmk_ep_t ulmk_ep_create(void)              */
#define ULMK_SYS_EP_CALL             31  /* int     ulmk_ep_call(ep, msg*)            */
#define ULMK_SYS_EP_RECV             32  /* int     ulmk_ep_recv(ep, msg*, sender*)   */
#define ULMK_SYS_EP_REPLY            33  /* int     ulmk_ep_reply(sender, reply*)     */
#define ULMK_SYS_EP_REPLY_RECV       34  /* int     ulmk_ep_reply_recv(...)           */
#define ULMK_SYS_EP_GRANT            35  /* int     ulmk_ep_grant(ep, target)         */
#define ULMK_SYS_EP_RECV_OR_NOTIF    36  /* int     ulmk_ep_recv_or_notif(...)        */
#define ULMK_SYS_EP_DESTROY          37  /* int     ulmk_ep_destroy(ep)               */

/* ── Notifications ───────────────────────────────────────────────── */
#define ULMK_SYS_NOTIF_CREATE        40  /* ulmk_notif_t ulmk_notif_create(void)        */
#define ULMK_SYS_NOTIF_SIGNAL        41  /* int        ulmk_notif_signal(notif, bits) */
#define ULMK_SYS_NOTIF_WAIT          42  /* int        ulmk_notif_wait(notif, mask, bits*) */
#define ULMK_SYS_NOTIF_POLL          43  /* uint32_t   ulmk_notif_poll(notif, mask)   */
#define ULMK_SYS_NOTIF_DESTROY       44  /* int        ulmk_notif_destroy(notif)      */
#define ULMK_SYS_NOTIF_WAIT_TIMEOUT  45  /* int ulmk_notif_wait_timeout(n,mask,bits*,ms) */

/* ── IRQ (requires IO >= 1 / ULMK_PRIV_DRIVER) ────────────────────── */
#define ULMK_SYS_IRQ_BIND            60  /* int ulmk_irq_bind(srpn, notif, bit)       */
#define ULMK_SYS_IRQ_ENABLE          61  /* int ulmk_irq_enable(srpn)                 */
#define ULMK_SYS_IRQ_DISABLE         62  /* int ulmk_irq_disable(srpn)                */
#define ULMK_SYS_IRQ_ACK             63  /* int ulmk_irq_ack(srpn)                    */
#define ULMK_SYS_IRQ_BIND_HW         64  /* int ulmk_irq_bind_hw(srpn,notif,bit,src)  */
#define ULMK_SYS_IRQ_ATTACH          65  /* ulmk_notif_t ulmk_irq_attach(srpn,fn,data) */
#define ULMK_SYS_IRQ_ATTACH_HW       66  /* ulmk_notif_t ulmk_irq_attach_hw(...)      */
#define ULMK_SYS_IRQ_DETACH          67  /* int ulmk_irq_detach(srpn)                 */

/* ── Thread management (requires IO >= 1 / ULMK_PRIV_DRIVER) ─────── */
#define ULMK_SYS_THREAD_SPAWN        70  /* ulmk_tid_t ulmk_thread_create(attr*)        */
#define ULMK_SYS_THREAD_KILL         71  /* int      ulmk_thread_kill(tid)            */
#define ULMK_SYS_THREAD_SUSPEND      72  /* int      ulmk_thread_suspend(tid)         */
#define ULMK_SYS_THREAD_RESUME       73  /* int      ulmk_thread_resume(tid)          */
#define ULMK_SYS_THREAD_SET_PRIO     74  /* int      ulmk_thread_priority_set(tid,p)  */
#define ULMK_SYS_THREAD_GET_PRIO     75  /* int      ulmk_thread_priority_get(tid)    */

/* ── Process management (requires IO >= 1 / ULMK_PRIV_DRIVER) ─────── */
#define ULMK_SYS_PROC_CREATE         80
#define ULMK_SYS_PROC_DESTROY        81
#define ULMK_SYS_PROC_ADD_REGION     82
#define ULMK_SYS_PROC_GRANT_CAP      83
#define ULMK_SYS_PROC_GRANT_IRQ      84

/* Upper bound used by the router for range validation. */
#define ULMK_SYS_MAX                128

#endif /* ULMK_SYSCALL_NR_H */
