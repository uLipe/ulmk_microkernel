/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Syscall router — kernel/syscall/syscall_router.c
 * Reference: docs/api_spec.md §12
 *
 * Called from ulmk_arch_syscall_entry() (arch layer) which extracts the
 * syscall number and up to four arguments from arch registers before
 * reaching this function.
 *
 * Privilege enforcement: syscalls marked "IO >= 1" are gated here by
 * checking the current thread's privilege.  ULMK_PRIV_DRIVER = 1.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <ulmk/syscall_nr.h>
#include <kernel/syscall/syscall_router.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_irq_internal.h>
#include <kernel/include/ulmk_syscall_wcet_internal.h>
#include <ulmk_arch.h>

/* Shorthand: fetch caller's privilege from the scheduler. */
static inline ulmk_privilege_t _caller_priv(void)
{
	ulmk_thread_t *t = ulmk_sched_current();
	return t ? t->privilege : ULMK_PRIV_USER;
}

#define REQUIRE_DRIVER(ret) \
	do { \
		if (_caller_priv() < ULMK_PRIV_DRIVER) \
			return (uint32_t)(int32_t)ULMK_EPERM; \
	} while (0)

/* Capability check — caller must hold @cap in cap_flags */
#define REQUIRE_CAP(cap) \
	do { \
		ulmk_thread_t *_t = ulmk_sched_current(); \
		if (!_t || !(_t->cap_flags & (cap))) \
			return (uint32_t)(int32_t)ULMK_EPERM; \
	} while (0)

uint32_t ulmk_syscall_router(uint32_t nr,
			   uint32_t a0, uint32_t a1,
			   uint32_t a2, uint32_t a3)
{
	if (nr == 0 || nr >= ULMK_SYS_MAX)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	/* Attach callbacks must not nest into the syscall router. */
	if (ulmk_irq_in_attach())
		return (uint32_t)(int32_t)ULMK_EPERM;

	switch (nr) {

	/* ── Memory (any privilege) ──────────────────────────────────── */
	case ULMK_SYS_MMAP:
		if (a3 & ULMK_MMAP_PERIPH)
			REQUIRE_CAP(ULMK_CAP_MAP_PERIPH);
		if (a3 & ULMK_MMAP_SHARED)
			REQUIRE_CAP(ULMK_CAP_MAP_SHARED);
		return ulmk_kern_mem_map(a0, a1, a2, a3);

	case ULMK_SYS_MUNMAP:
		return ulmk_kern_mem_unmap(a0, a1);

	case ULMK_SYS_MEM_GRANT:
		return ulmk_kern_mem_grant(a0, a1, a2, a3);

	case ULMK_SYS_MEM_REVOKE:
		return ulmk_kern_mem_revoke(a0, a1);

	case ULMK_SYS_DCACHE_CLEAN:
		REQUIRE_DRIVER(a0);
#if ULMK_ARCH_HAS_CACHE
		ulmk_arch_dcache_clean((void *)(uintptr_t)a0, (size_t)a1);
		return (uint32_t)(int32_t)ULMK_OK;
#else
		return (uint32_t)(int32_t)ULMK_ENOTSUP;
#endif

	case ULMK_SYS_DCACHE_INVALIDATE:
		REQUIRE_DRIVER(a0);
#if ULMK_ARCH_HAS_CACHE
		ulmk_arch_dcache_invalidate((void *)(uintptr_t)a0, (size_t)a1);
		return (uint32_t)(int32_t)ULMK_OK;
#else
		return (uint32_t)(int32_t)ULMK_ENOTSUP;
#endif

	case ULMK_SYS_DCACHE_CLEAN_INV:
		REQUIRE_DRIVER(a0);
#if ULMK_ARCH_HAS_CACHE
		ulmk_arch_dcache_clean_invalidate((void *)(uintptr_t)a0,
						  (size_t)a1);
		return (uint32_t)(int32_t)ULMK_OK;
#else
		return (uint32_t)(int32_t)ULMK_ENOTSUP;
#endif

	/* Per-thread heap (slabAO model) */
	case ULMK_SYS_GET_THREAD_HEAP:
		return ulmk_kern_get_thread_heap(a0);

	case ULMK_SYS_HEAP_EXTEND:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_heap_extend(a0);

	/* ── Scheduling (any privilege) ──────────────────────────────── */
	case ULMK_SYS_YIELD:
		return ulmk_kern_yield();

	case ULMK_SYS_EXIT:
		return ulmk_kern_exit();

	case ULMK_SYS_SLEEP:
		return ulmk_kern_sleep(a0);

	case ULMK_SYS_SLEEP_CANCEL:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_sleep_cancel(a0);

	case ULMK_SYS_EP_CALL_TIMEOUT:
		return ulmk_kern_ep_call_timeout(a0, a1, a2);

	case ULMK_SYS_TICK_START:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_tick_start();

	/* ── Thread query (any privilege) ────────────────────────────── */
	case ULMK_SYS_THREAD_SELF:
		return ulmk_kern_thread_self();

	case ULMK_SYS_CPU_ID:
		return ulmk_arch_cpu_id();

	case ULMK_SYS_WCET_BIND:
		return ulmk_kern_wcet_bind(a0);

	/* ── IPC endpoints (any privilege) ──────────────────────────── */
	case ULMK_SYS_EP_CREATE:
		return ulmk_kern_ep_create();

	case ULMK_SYS_EP_CALL:
		return ulmk_kern_ep_call(a0, a1);

	case ULMK_SYS_EP_RECV:
		return ulmk_kern_ep_recv(a0, a1, a2);

	case ULMK_SYS_EP_REPLY:
		return ulmk_kern_ep_reply(a0, a1);

	case ULMK_SYS_EP_REPLY_RECV:
		return ulmk_kern_ep_reply_recv(a0, a1, a2);

	case ULMK_SYS_EP_GRANT:
		return ulmk_kern_ep_grant(a0, a1);

	case ULMK_SYS_EP_RECV_OR_NOTIF:
		return ulmk_kern_ep_recv_or_notif(a0, a1, a2, a3);

	case ULMK_SYS_EP_DESTROY:
		return ulmk_kern_ep_destroy(a0);

	/* ── Notifications (any privilege) ──────────────────────────── */
	case ULMK_SYS_NOTIF_CREATE:
		return ulmk_kern_notif_create();

	case ULMK_SYS_NOTIF_SIGNAL:
		return ulmk_kern_notif_signal(a0, a1);

	case ULMK_SYS_NOTIF_WAIT:
		return ulmk_kern_notif_wait(a0, a1, a2);

	case ULMK_SYS_NOTIF_WAIT_TIMEOUT:
		return ulmk_kern_notif_wait_timeout(a0, a1, a2, a3);

	case ULMK_SYS_NOTIF_POLL:
		return ulmk_kern_notif_poll(a0, a1);

	case ULMK_SYS_NOTIF_DESTROY:
		return ulmk_kern_notif_destroy(a0);

	/* ── IRQ (requires ULMK_PRIV_DRIVER) ──────────────────────────── */
	case ULMK_SYS_IRQ_BIND:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_IRQ);
		return ulmk_kern_irq_bind(a0, a1, a2);

	case ULMK_SYS_IRQ_ENABLE:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_irq_enable(a0);

	case ULMK_SYS_IRQ_DISABLE:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_irq_disable(a0);

	case ULMK_SYS_IRQ_ACK:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_irq_ack(a0);

	case ULMK_SYS_IRQ_BIND_HW:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_IRQ);
		return ulmk_kern_irq_bind_hw(a0, a1, a2, a3);

	case ULMK_SYS_IRQ_ATTACH:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_IRQ);
		return ulmk_kern_irq_attach(a0, a1, a2);

	case ULMK_SYS_IRQ_ATTACH_HW:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_IRQ);
		return ulmk_kern_irq_attach_hw(a0, a1, a2, a3);

	case ULMK_SYS_IRQ_DETACH:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_IRQ);
		return ulmk_kern_irq_detach(a0);

	/* ── Thread management (requires IO >= 1 / ULMK_PRIV_DRIVER) ─── */
	case ULMK_SYS_THREAD_SPAWN:
		/*
		 * Return ULMK_EPERM (negative) in the TID word on gate
		 * failure — sdk_suite/silicon_cap_neg check tid_is_eperm().
		 */
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_SPAWN);
		return ulmk_kern_thread_spawn(a0);

	case ULMK_SYS_THREAD_KILL:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_KILL);
		return ulmk_kern_thread_kill(a0);

	case ULMK_SYS_THREAD_SUSPEND:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_thread_suspend(a0);

	case ULMK_SYS_THREAD_RESUME:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_thread_resume(a0);

	case ULMK_SYS_THREAD_SET_PRIO:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_thread_set_prio(a0, a1);

	case ULMK_SYS_THREAD_GET_PRIO:
		REQUIRE_DRIVER(a0);
		return ulmk_kern_thread_get_prio(a0);

	/* ── Capability management (requires ULMK_CAP_GRANT_CAP) ───────── */
	case ULMK_SYS_PROC_GRANT_CAP:
		REQUIRE_DRIVER(a0);
		REQUIRE_CAP(ULMK_CAP_GRANT_CAP);
		return ulmk_kern_cap_grant(a0, a1);

	default:
		return (uint32_t)(int32_t)ULMK_EINVAL;
	}
}
