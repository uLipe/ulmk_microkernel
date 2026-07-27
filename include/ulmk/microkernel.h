/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 */

/**
 * @file microkernel.h
 * @brief ulmk public userspace API.
 *
 * Usage: @code #include <ulmk/microkernel.h> @endcode
 *
 * All public API functions are static inline syscall wrappers: they issue the
 * architecture's SYSCALL/trap instruction and the kernel router
 * (kernel/syscall/syscall_router.c) dispatches to the handler.
 *
 * @see docs/api_spec.md for the full specification.
 */

#ifndef ULMK_MICROKERNEL_H
#define ULMK_MICROKERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/syscall_nr.h>
#include <ulmk/syscall_wcet.h>

/* =========================================================================
 * Privilege levels (TriCore PSW.IO field)
 * ========================================================================= */

typedef enum {
	ULMK_PRIV_USER   = 0,	/* IO=0: no peripheral access */
	ULMK_PRIV_DRIVER = 1,	/* IO=1: peripheral access, restricted syscalls */
	ULMK_PRIV_KERNEL = 2,	/* IO=2: supervisor mode (kernel-internal use only) */
} ulmk_privilege_t;

/* =========================================================================
 * Error codes
 * ========================================================================= */

#define ULMK_OK		  0
#define ULMK_EINVAL	 -1
#define ULMK_ENOMEM	 -2
#define ULMK_EPERM	 -3
#define ULMK_ENOSPC	 -4
#define ULMK_EDEADLK	 -5
#define ULMK_ESRCH	 -6
#define ULMK_ETIMEOUT	 -7
#define ULMK_ECANCELED	 -8
#define ULMK_ENOTSUP	 -9

/* =========================================================================
 * Opaque handles
 * ========================================================================= */

typedef uintptr_t ulmk_tid_t;
typedef uintptr_t ulmk_ep_t;
typedef uintptr_t ulmk_notif_t;

#define ULMK_TID_INVALID		((ulmk_tid_t)0)
#define ULMK_EP_INVALID		((ulmk_ep_t)0)
#define ULMK_NOTIF_INVALID	((ulmk_notif_t)0)

/* =========================================================================
 * IPC message
 * ========================================================================= */

#define ULMK_MSG_WORDS	6

typedef struct {
	uint32_t label;
	uint32_t words[ULMK_MSG_WORDS];
} ulmk_msg_t;

/* =========================================================================
 * Thread attributes
 * ========================================================================= */

typedef struct {
	const char	*name;
	void		(*entry)(void *arg);
	void		*arg;
	uint8_t		 priority;	/* 0 = highest, 255 = lowest */
	size_t		 stack_size;
	ulmk_privilege_t	 privilege;
	size_t		 heap_size;	/* 0 = no per-thread heap; last for compat */
	uint8_t		 cpu;		/* permanent affinity; 0 = CPU0 */
} ulmk_thread_attr_t;

/*
 * Per-thread heap descriptor — returned by ulmk_get_thread_heap().
 * Describes the heap area within the thread's slabAO allocation.
 */
typedef struct {
	uintptr_t base;	/* start of the heap region */
	size_t    size;	/* size of the heap region in bytes */
} ulmk_heap_info_t;

/* =========================================================================
 * Boot information — passed to ulmk_root_thread()
 * ========================================================================= */

#define ULMK_BOOT_MAX_MEM_REGIONS	4

typedef struct {
	struct {
		uintptr_t base;
		size_t    size;
	} mem[ULMK_BOOT_MAX_MEM_REGIONS];
	uint32_t  mem_count;
	uintptr_t csa_pool_base;
	size_t    csa_pool_size;
} ulmk_boot_info_t;

/* =========================================================================
 * Memory domain descriptor — placed in .domain_table by ULMK_DEFINE_DOMAIN
 * ========================================================================= */

#define ULMK_PERM_READ	(1u << 0)
#define ULMK_PERM_WRITE	(1u << 1)
#define ULMK_PERM_EXEC	(1u << 2)
#define ULMK_PERM_USER	(1u << 3)

typedef struct {
	const char *name;
	uintptr_t   start;
	uintptr_t   end;
	uint32_t    perms;
} ulmk_domain_desc_t;

/* =========================================================================
 * Capability flags — bitmask stored in ulmk_thread_t.cap_flags
 *
 * Checked by the syscall router before privileged operations.
 * Root thread starts with ULMK_CAP_ALL; grants subsets to children.
 * ========================================================================= */

#define ULMK_CAP_SPAWN		(1u << 0)  /* may create threads */
#define ULMK_CAP_KILL		(1u << 1)  /* may kill other threads */
#define ULMK_CAP_IRQ		(1u << 2)  /* may bind/enable hardware IRQs */
#define ULMK_CAP_MAP_PERIPH	(1u << 3)  /* may map peripheral MMIO regions */
#define ULMK_CAP_GRANT_CAP	(1u << 4)  /* may grant capabilities to others */
#define ULMK_CAP_MAP_SHARED	(1u << 5)  /* may map shared phys windows (SDRAM/FB) */
#define ULMK_CAP_ALL		0xFFu	   /* all capabilities; root thread initial */

/* =========================================================================
 * Memory map flags
 * ========================================================================= */

#define ULMK_MMAP_ANON		(1u << 0)
#define ULMK_MMAP_PERIPH	(1u << 1)
#define ULMK_MMAP_SHARED	(1u << 2)  /* fixed phys window, Normal NC */

/* =========================================================================
 * IRQ handler type — userspace attach callback (ISR fast-path)
 * ========================================================================= */

/**
 * @brief ISR attach callback.
 *
 * Called from the kernel IRQ path with the registering thread's memory map
 * active.  Must not invoke syscalls.  Return @c true to have the kernel
 * acknowledge the IRQ and signal the attach-owned notification; return
 * @c false if the callback already acknowledged/rearmed the source (no
 * notification is signalled).
 */
typedef bool (*ulmk_irq_attach_fn_t)(void *data);

/* =========================================================================
 * Include ABI macros and result types after type definitions above.
 * ulmk_syscall_abi.h references ulmk_msg_t, ulmk_tid_t, so it must come last.
 * ========================================================================= */

#include <ulmk/syscall_abi.h>

/* =========================================================================
 * Boot entry point — user must provide exactly one strong definition
 * ========================================================================= */

/**
 * @brief Boot entry point the application must implement.
 *
 * Exactly one strong definition must be linked.  The kernel creates the root
 * thread and calls this at @c ULMK_PRIV_DRIVER with all capabilities.  It must
 * never return — call ulmk_thread_exit() once bootstrapping is complete.
 *
 * @param info Boot state: available memory regions and CSA pool location.
 */
void ulmk_root_thread(const ulmk_boot_info_t *info);

/* =========================================================================
 * Thread API — docs/api_spec.md §6
 * All require ULMK_PRIV_DRIVER except ulmk_thread_self() and ulmk_thread_yield().
 * ========================================================================= */

/**
 * @brief Return the calling thread's TID.
 * @return TID of the current thread.  Callable at any privilege level.
 */
static inline ulmk_tid_t ulmk_thread_self(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_THREAD_SELF, r);
	return (ulmk_tid_t)r;
}

/**
 * @brief Return the CPU id this thread is currently running on.
 */
static inline uint32_t ulmk_cpu_id(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_CPU_ID, r);
	return r;
}

/**
 * @brief Bind a private WCET sample slot for this thread (SYSCALL_WCET builds).
 *
 * When @p slot is non-NULL, each subsequent syscall publishes its sample
 * there in addition to the per-CPU @c g_ulmk_syscall_wcet[] entry.  Required
 * for correct measurement when peer threads share a CPU (IPC client/server).
 *
 * @param slot Userspace slot, or NULL to unbind.
 * @return @c ULMK_OK, or @c ULMK_EINVAL if WCET is not enabled in this build.
 */
static inline int ulmk_wcet_bind(volatile struct ulmk_syscall_wcet_slot *slot)
{
	uint32_t r;

	ULMK_SYSCALL_1(ULMK_SYS_WCET_BIND, slot, r);
	return (int)r;
}

/**
 * @brief Yield the CPU to the next runnable thread at the same priority.
 *
 * FIFO within a priority level.  Does not time-slice; use blocking or periodic
 * yields for fairness at the same level.
 *
 * @return @c ULMK_OK.
 */
static inline int ulmk_thread_yield(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_YIELD, r);
	return (int)r;
}

/**
 * @brief Block the calling thread for at least @p ms milliseconds.
 *
 * Maximum range is limited by the kernel timing wheel (~4 minutes at
 * 1 kHz).  Longer requests return @c ULMK_EINVAL.
 *
 * @return @c ULMK_OK, @c ULMK_EINVAL, or @c ULMK_ECANCELED if cancelled.
 */
static inline int ulmk_sleep_ms(uint32_t ms)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_SLEEP, ms, r);
	return (int)r;
}

/**
 * @brief Cancel a blocked @c ulmk_sleep_ms on @p tid.
 * @return @c ULMK_OK, @c ULMK_ESRCH, or @c ULMK_EINVAL if not sleeping.
 * @pre Caller privilege >= @c ULMK_PRIV_DRIVER.
 */
static inline int ulmk_sleep_cancel(ulmk_tid_t tid)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_SLEEP_CANCEL, tid, r);
	return (int)r;
}

/**
 * @brief Arm the kernel periodic tick (board bring-up).
 *
 * Called once from board_timer_start() after console IPC is ready so the
 * first userspace RFE is not racing the timer IRQ on sensitive emulators.
 */
static inline void ulmk_tick_start(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_TICK_START, r);
	(void)r;
}

/**
 * @brief Create a runnable thread.
 * @param attr Thread attributes: entry, arg, stack_size, priority, privilege
 *             and optional per-thread heap size.
 * @return New TID, or @c ULMK_TID_INVALID on failure.
 * @pre Caller holds @c ULMK_CAP_SPAWN.
 */
static inline ulmk_tid_t ulmk_thread_create(const ulmk_thread_attr_t *attr)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_THREAD_SPAWN, attr, r);
	return (ulmk_tid_t)r;
}

/**
 * @brief Terminate a thread, freeing its stack and context.
 * @param tid Target thread; removed from any wait list first.
 * @return @c ULMK_OK, @c ULMK_ESRCH (unknown tid) or @c ULMK_EPERM.
 * @pre Caller holds @c ULMK_CAP_KILL.
 */
static inline int ulmk_thread_kill(ulmk_tid_t tid)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_THREAD_KILL, tid, r);
	return (int)r;
}

/**
 * @brief Remove a thread from the ready queue without terminating it.
 * @param tid Target thread; consumes no CPU while suspended.
 * @return @c ULMK_OK or an error code.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER.
 */
static inline int ulmk_thread_suspend(ulmk_tid_t tid)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_THREAD_SUSPEND, tid, r);
	return (int)r;
}

/**
 * @brief Make a previously suspended thread runnable again.
 * @param tid Target thread.
 * @return @c ULMK_OK or an error code.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER.
 */
static inline int ulmk_thread_resume(ulmk_tid_t tid)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_THREAD_RESUME, tid, r);
	return (int)r;
}

/**
 * @brief Set a thread's priority.
 * @param tid  Target thread.
 * @param prio New priority (0 = highest, 255 = lowest).
 * @return @c ULMK_OK or an error code.
 * @note Takes effect at the next scheduling decision.
 */
static inline int ulmk_thread_priority_set(ulmk_tid_t tid, uint8_t prio)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_THREAD_SET_PRIO, tid, prio, r);
	return (int)r;
}

/**
 * @brief Query a thread's current priority.
 * @param tid Target thread.
 * @return Priority in the range 0-255, or a negative error code if @p tid is
 *         unknown.
 */
static inline int ulmk_thread_priority_get(ulmk_tid_t tid)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_THREAD_GET_PRIO, tid, r);
	return (int)r;
}

/**
 * @brief Terminate the calling thread.
 *
 * Never returns.  ulmk_root_thread() must call this once bootstrapping is done.
 */
static inline __attribute__((noreturn)) void ulmk_thread_exit(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_EXIT, r);
	(void)r;
	__builtin_unreachable();
}

/* =========================================================================
 * IPC Endpoint API — docs/api_spec.md §7
 * ========================================================================= */

/**
 * @brief Create an endpoint owned by the calling thread.
 *
 * Create the endpoint before spawning the server so clients can call it
 * immediately.
 *
 * @return New endpoint, or @c ULMK_EP_INVALID if the pool is exhausted.
 */
static inline ulmk_ep_t ulmk_ep_create(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_EP_CREATE, r);
	return (ulmk_ep_t)r;
}

/**
 * @brief Synchronous send: call an endpoint and block for the reply.
 * @param ep  Target endpoint.
 * @param msg Message to send; overwritten in place with the reply.
 * @return @c ULMK_OK, or @c ULMK_EINVAL (bad endpoint).
 */
static inline int ulmk_ep_call(ulmk_ep_t ep, ulmk_msg_t *msg)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_EP_CALL, ep, msg, r);
	return (int)r;
}

/**
 * @brief Synchronous endpoint call with a bounded wait.
 * @param timeout_ms Maximum wait; 0 is invalid (use @c ulmk_ep_call).
 * @return @c ULMK_OK, @c ULMK_ETIMEOUT, or an error code.
 */
static inline int ulmk_ep_call_timeout(ulmk_ep_t ep, ulmk_msg_t *msg,
				     uint32_t timeout_ms)
{
	uint32_t r;
	ULMK_SYSCALL_3(ULMK_SYS_EP_CALL_TIMEOUT, ep, msg, timeout_ms, r);
	return (int)r;
}

/**
 * @brief Block until a message arrives on an endpoint.
 * @param ep     Endpoint to receive from.
 * @param[out] msg    Filled with the received message.
 * @param[out] sender Filled with the caller's TID.
 * @return @c ULMK_OK or an error code.
 * @note The server must ulmk_ep_reply(@p sender, …) to unblock the caller.
 */
static inline int ulmk_ep_recv(ulmk_ep_t ep, ulmk_msg_t *msg, ulmk_tid_t *sender)
{
	uint32_t r;
	ULMK_SYSCALL_3(ULMK_SYS_EP_RECV, ep, msg, sender, r);
	return (int)r;
}

/**
 * @brief Reply to a blocked caller and make it runnable.
 * @param sender TID returned by ulmk_ep_recv().
 * @param reply  Reply message.
 * @return @c ULMK_OK or an error code.
 */
static inline int ulmk_ep_reply(ulmk_tid_t sender, const ulmk_msg_t *reply)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_EP_REPLY, sender, reply, r);
	return (int)r;
}

/**
 * @brief Atomic reply-and-receive: reply, then block for the next call.
 *
 * Cheaper than a separate ulmk_ep_reply() + ulmk_ep_recv().  The output
 * pointers are packed into a stack-allocated args struct to stay within the
 * 4-register argument limit.
 *
 * @param ep     Endpoint to receive the next call from.
 * @param sender Caller to reply to, or @c ULMK_TID_INVALID to skip the reply
 *               and only receive.
 * @param reply  Reply message for @p sender.
 * @param[out] next        Filled with the next received message.
 * @param[out] next_sender Filled with the next caller's TID.
 * @return @c ULMK_OK or an error code.
 */
static inline int ulmk_ep_reply_recv(ulmk_ep_t ep, ulmk_tid_t sender,
				   const ulmk_msg_t *reply, ulmk_msg_t *next,
				   ulmk_tid_t *next_sender)
{
	ulmk_reply_recv_args_t args = { reply, next, next_sender };
	uint32_t r;
	ULMK_SYSCALL_3(ULMK_SYS_EP_REPLY_RECV, ep, sender, &args, r);
	return (int)r;
}

/**
 * @brief Allow another thread to call an endpoint.
 * @param ep     Endpoint owned by the caller.
 * @param target Thread granted the right to ulmk_ep_call() @p ep.
 * @return @c ULMK_OK or an error code.
 * @note The owner retains full access.
 */
static inline int ulmk_ep_grant(ulmk_ep_t ep, ulmk_tid_t target)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_EP_GRANT, ep, target, r);
	return (int)r;
}

/**
 * @brief Block on an endpoint or a notification, whichever arrives first.
 *
 * Lets a server also react to hardware events.  On return exactly one path is
 * taken: @p sender != @c ULMK_TID_INVALID (IPC) or @p notif_bits != 0
 * (notification).  The result is marshalled through a stack-allocated struct
 * and copied to the output pointers after the syscall.
 *
 * @param ep    Endpoint to receive from.
 * @param notif Notification to wait on.
 * @param mask  Notification bits of interest.
 * @param[out] msg        Received message (IPC path); may be NULL.
 * @param[out] sender     Caller's TID (IPC path); may be NULL.
 * @param[out] notif_bits Matched bits (notification path); may be NULL.
 * @return @c ULMK_OK or an error code.
 */
static inline int ulmk_ep_recv_or_notif(ulmk_ep_t ep, ulmk_notif_t notif,
				      uint32_t mask, ulmk_msg_t *msg,
				      ulmk_tid_t *sender, uint32_t *notif_bits)
{
	ulmk_recv_or_notif_result_t res = { {0, {0}}, ULMK_TID_INVALID, 0, 0 };
	uint32_t r;
	ULMK_SYSCALL_4(ULMK_SYS_EP_RECV_OR_NOTIF, ep, notif, mask, &res, r);
	if (msg)        *msg        = res.msg;
	if (sender)     *sender     = res.sender;
	if (notif_bits) *notif_bits = res.notif_bits;
	return (int)r;
}

/**
 * @brief Free an endpoint.
 * @param ep Endpoint to destroy.
 * @return @c ULMK_OK or an error code.
 * @note Any threads blocked on @p ep are unblocked with @c ULMK_EINVAL.
 */
static inline int ulmk_ep_destroy(ulmk_ep_t ep)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_EP_DESTROY, ep, r);
	return (int)r;
}

/* =========================================================================
 * Notification API — docs/api_spec.md §8
 * ========================================================================= */

/**
 * @brief Create a 32-bit notification object (each bit an independent flag).
 * @return New notification, or @c ULMK_NOTIF_INVALID if the pool is exhausted.
 */
static inline ulmk_notif_t ulmk_notif_create(void)
{
	uint32_t r;
	ULMK_SYSCALL_0(ULMK_SYS_NOTIF_CREATE, r);
	return (ulmk_notif_t)r;
}

/**
 * @brief Atomically OR bits into a notification, waking any waiter.
 * @param notif Target notification.
 * @param bits  Bits to set.
 * @return @c ULMK_OK or an error code.
 * @note Safe to reach from IRQ-delivery context (the kernel signals from the ISR).
 */
static inline int ulmk_notif_signal(ulmk_notif_t notif, uint32_t bits)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_NOTIF_SIGNAL, notif, bits, r);
	return (int)r;
}

/**
 * @brief Non-blocking check of notification bits.
 * @param notif Target notification.
 * @param mask  Bits of interest.
 * @return The set bits matching @p mask (atomically cleared), or 0 if none.
 */
static inline uint32_t ulmk_notif_poll(ulmk_notif_t notif, uint32_t mask)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_NOTIF_POLL, notif, mask, r);
	return r;
}

/**
 * @brief Block until at least one bit matching @p mask is set.
 * @param notif Target notification.
 * @param mask  Bits to wait for.
 * @param[out] bits Filled with the matched bits (which are then cleared).
 * @return @c ULMK_OK or an error code.
 */
static inline int ulmk_notif_wait(ulmk_notif_t notif, uint32_t mask, uint32_t *bits)
{
	uint32_t r;
	ULMK_SYSCALL_3(ULMK_SYS_NOTIF_WAIT, notif, mask, bits, r);
	return (int)r;
}

/**
 * @brief Wait for notification bits with a bounded wait.
 * @param timeout_ms Maximum wait; overflow of the wheel returns @c ULMK_EINVAL.
 * @return @c ULMK_OK, @c ULMK_ETIMEOUT, or an error code.
 */
static inline int ulmk_notif_wait_timeout(ulmk_notif_t notif, uint32_t mask,
					uint32_t *bits, uint32_t timeout_ms)
{
	uint32_t r;
	ULMK_SYSCALL_4(ULMK_SYS_NOTIF_WAIT_TIMEOUT, notif, mask, bits,
		       timeout_ms, r);
	return (int)r;
}

/**
 * @brief Free a notification object.
 * @param notif Notification to destroy.
 * @return @c ULMK_OK or an error code.
 * @note Threads blocked on @p notif are woken with @c ULMK_EINVAL.
 */
static inline int ulmk_notif_destroy(ulmk_notif_t notif)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_NOTIF_DESTROY, notif, r);
	return (int)r;
}

/* =========================================================================
 * Memory API — docs/api_spec.md §9
 *
 * Per-thread heap model (slabAO):
 *   Each thread may carry a private heap allocated at creation time by
 *   setting attr.heap_size > 0.  The kernel allocates a contiguous slabAO
 *   (stack_size + heap_size bytes) from user_pool and covers it with a
 *   single MPU DPR.  The TCB lives in a separate allocation.
 *
 *   ulmk_get_thread_heap() — query heap base and size for the calling thread.
 *   ulmk_heap_extend()     — grow heap by allocating an additional slab from
 *                            the kernel pool; requires ULMK_PRIV_DRIVER.
 * ========================================================================= */

/**
 * @brief Query the calling thread's private heap.
 * @param[out] info Filled with the heap base and size.
 * @return @c ULMK_OK, or @c ULMK_EPERM if the thread was created without a heap
 *         (@c attr.heap_size == 0).
 */
static inline int ulmk_get_thread_heap(ulmk_heap_info_t *info)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_GET_THREAD_HEAP, info, r);
	return (int)r;
}

/**
 * @brief Grow the calling thread's heap.
 * @param size Additional bytes to allocate from the global user pool, covered
 *             by an extra MPU region.
 * @return @c ULMK_OK, @c ULMK_ENOMEM, @c ULMK_EPERM or @c ULMK_ENOSPC (MPU
 *         region limit reached).
 * @pre Caller runs at @c ULMK_PRIV_DRIVER.
 */
static inline int ulmk_heap_extend(size_t size)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_HEAP_EXTEND, size, r);
	return (int)r;
}

/**
 * @brief Map a memory region.
 * @param hint  Advisory placement address (may be NULL).
 * @param size  Region size in bytes.
 * @param perms Permission mask of @c ULMK_PERM_* flags.
 * @param flags @c ULMK_MMAP_ANON (user pool), @c ULMK_MMAP_PERIPH (MMIO,
 *              requires @c ULMK_CAP_MAP_PERIPH), or @c ULMK_MMAP_SHARED
 *              (fixed physical window / SDRAM / FB; requires
 *              @c ULMK_CAP_MAP_SHARED).
 * @return Base address of the mapping, or NULL on failure.
 */
static inline void *ulmk_mem_map(void *hint, size_t size,
			       uint32_t perms, uint32_t flags)
{
	uint32_t r;
	ULMK_SYSCALL_4(ULMK_SYS_MMAP, hint, size, perms, flags, r);
	/*
	 * Kernel errors are small negative ints (ULMK_E*).  Do not treat any
	 * high-bit address as failure — MMIO maps often live above 0x80000000.
	 */
	if ((int32_t)r < 0 && (int32_t)r >= -16)
		return NULL;
	return (void *)(uintptr_t)r;
}

/**
 * @brief Unmap a region previously returned by ulmk_mem_map().
 * @param addr Base address of the mapping.
 * @param size Region size in bytes.
 * @return @c ULMK_OK or an error code.
 */
static inline int ulmk_mem_unmap(void *addr, size_t size)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_MUNMAP, addr, size, r);
	return (int)r;
}

/**
 * @brief Share a memory region with another thread (no copy).
 * @param addr   Base address of the region.
 * @param size   Region size in bytes.
 * @param target Thread to grant access to.
 * @param perms  Permission mask of @c ULMK_PERM_* flags.
 * @return @c ULMK_OK or an error code.
 */
static inline int ulmk_mem_grant(void *addr, size_t size,
			       ulmk_tid_t target, uint32_t perms)
{
	uint32_t r;
	ULMK_SYSCALL_4(ULMK_SYS_MEM_GRANT, addr, size, target, perms, r);
	return (int)r;
}

/* =========================================================================
 * IRQ API — docs/api_spec.md §10  (requires ULMK_PRIV_DRIVER)
 * ========================================================================= */

/**
 * @brief Bind a hardware interrupt to a notification bit.
 * @param srpn  Interrupt priority number / source.
 * @param notif Notification signalled when the IRQ fires (from the ISR).
 * @param bit   Bit index to set in @p notif.
 * @return @c ULMK_OK or an error code.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER and holds @c ULMK_CAP_IRQ.
 * @note At most @c ULMK_CONFIG_MAX_IRQ_BINDINGS bindings may be active at once.
 */
static inline int ulmk_irq_bind(uint8_t srpn, ulmk_notif_t notif, uint32_t bit)
{
	uint32_t r;
	ULMK_SYSCALL_3(ULMK_SYS_IRQ_BIND, srpn, notif, bit, r);
	return (int)r;
}

/**
 * @brief Enable delivery of an interrupt.
 * @param srpn Interrupt to enable.
 * @return @c ULMK_OK or an error code.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER and holds @c ULMK_CAP_IRQ.
 */
static inline int ulmk_irq_enable(uint8_t srpn)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_IRQ_ENABLE, srpn, r);
	return (int)r;
}

/**
 * @brief Disable delivery of an interrupt.
 * @param srpn Interrupt to disable.
 * @return @c ULMK_OK or an error code.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER and holds @c ULMK_CAP_IRQ.
 */
static inline int ulmk_irq_disable(uint8_t srpn)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_IRQ_DISABLE, srpn, r);
	return (int)r;
}

/**
 * @brief Clear the pending flag for an interrupt.
 * @param srpn Interrupt to acknowledge.
 * @return @c ULMK_OK or an error code.
 * @note Must be called after servicing a level-triggered IRQ to avoid immediate
 *       re-entry.
 */
static inline int ulmk_irq_ack(uint8_t srpn)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_IRQ_ACK, srpn, r);
	return (int)r;
}

/**
 * @brief Bind a fixed hardware source register to a notification bit.
 *
 * Like ulmk_irq_bind() but for on-chip peripherals whose interrupt-source
 * register address is fixed by the SoC; used by board services.
 *
 * @param srpn    Interrupt priority number / source.
 * @param notif   Notification signalled when the IRQ fires.
 * @param bit     Bit index to set in @p notif.
 * @param src_reg Absolute address of the source register; 0 is rejected.
 * @return @c ULMK_OK, or @c ULMK_EINVAL if @p src_reg is 0.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER and holds @c ULMK_CAP_IRQ.
 */
static inline int ulmk_irq_bind_hw(uint8_t srpn, ulmk_notif_t notif,
				   uint32_t bit, uintptr_t src_reg)
{
	uint32_t r;
	ULMK_SYSCALL_4(ULMK_SYS_IRQ_BIND_HW, srpn, notif, bit, src_reg, r);
	return (int)r;
}

/**
 * @brief Attach a userspace ISR callback and obtain a waitable notification.
 *
 * Creates a notification object owned by the binding (bit 0).  When the IRQ
 * fires the kernel invokes @p fn(@p data) under the registering thread's
 * memory map (see docs — opt-in via @c ULMK_CONFIG_IRQ_ATTACH).
 * If @p fn returns true the kernel acks the source and signals the notif
 * (even with no waiter); if false the callback owns ack/rearm and no signal
 * is raised.
 *
 * @return Notification handle on success; on error a small negative
 *         @c ULMK_E* code cast to @c ulmk_notif_t (e.g. @c ULMK_ENOTSUP when
 *         @c ULMK_CONFIG_IRQ_ATTACH=0).  Distinguish errors with
 *         @c ((int32_t)n < 0 && (int32_t)n >= -16) — handles may be high
 *         pointer values.
 * @pre Caller runs at @c ULMK_PRIV_DRIVER and holds @c ULMK_CAP_IRQ.
 * @note Conflicts with an existing bind/attach on the same @p srpn.
 * @warning Enabling @c ULMK_CONFIG_IRQ_ATTACH opens a trusted ISR path into
 *          userspace — the caller accepts that risk.
 */
static inline ulmk_notif_t ulmk_irq_attach(uint8_t srpn,
					  ulmk_irq_attach_fn_t fn,
					  void *data)
{
	uint32_t r;
	ULMK_SYSCALL_3(ULMK_SYS_IRQ_ATTACH, srpn, fn, data, r);
	return (ulmk_notif_t)r;
}

/**
 * @brief Like @c ulmk_irq_attach but registers a fixed hardware SRC address.
 */
static inline ulmk_notif_t ulmk_irq_attach_hw(uint8_t srpn,
					     ulmk_irq_attach_fn_t fn,
					     void *data,
					     uintptr_t src_reg)
{
	uint32_t r;
	ULMK_SYSCALL_4(ULMK_SYS_IRQ_ATTACH_HW, srpn, fn, data, src_reg, r);
	return (ulmk_notif_t)r;
}

/**
 * @brief Tear down an attach (or leave bind-only bindings untouched).
 *
 * Disables the source, destroys an attach-owned notification, and frees the
 * binding slot when it was created by attach/attach_hw.
 *
 * @return @c ULMK_OK, @c ULMK_EINVAL, or @c ULMK_ENOTSUP when
 *         @c ULMK_CONFIG_IRQ_ATTACH=0.
 */
static inline int ulmk_irq_detach(uint8_t srpn)
{
	uint32_t r;
	ULMK_SYSCALL_1(ULMK_SYS_IRQ_DETACH, srpn, r);
	return (int)r;
}

/* =========================================================================
 * Capability API — docs/api_spec.md §13
 * Requires ULMK_CAP_GRANT_CAP.
 * ========================================================================= */

/**
 * @brief Grant capabilities to another thread.
 * @param target Thread to receive the capabilities.
 * @param caps   Mask of @c ULMK_CAP_* flags to grant.
 * @return @c ULMK_OK or an error code.
 * @pre Caller holds @c ULMK_CAP_GRANT_CAP and every capability in @p caps.
 */
static inline int ulmk_cap_grant(ulmk_tid_t target, uint32_t caps)
{
	uint32_t r;
	ULMK_SYSCALL_2(ULMK_SYS_PROC_GRANT_CAP, target, caps, r);
	return (int)r;
}

#endif /* ULMK_MICROKERNEL_H */
