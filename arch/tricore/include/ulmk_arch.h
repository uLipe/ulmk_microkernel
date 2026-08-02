/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Architecture abstraction layer — TriCore TC1.6.x / TC2xx-TC3xx
 * Full specification: docs/arch_api_spec.md
 *
 * This header declares the contract between the platform-independent kernel
 * and the TriCore arch port.  All functions here are implemented in
 * arch/tricore/arch.c (or arch_*.c sub-files).
 */

#ifndef ULMK_ARCH_H
#define ULMK_ARCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <arch_config.h>

/* =========================================================================
 * Arch types (arch_api_spec.md §3)
 * ========================================================================= */

/*
 * Saved CPU context — head of the CSA chain for this thread.
 *
 * On suspension ulmk_arch_ctx_switch() stores the PCXI register here.
 * PCXI encodes a two-frame chain:
 *   pcxi → lower-context CSA (UL=0, saved by SVLCX)
 *        → upper-context CSA (UL=1, saved by CALL into ctx_switch)
 *
 * For a freshly created thread ulmk_arch_ctx_init() fabricates the same
 * two-frame chain so that the first RSLCX+RFE starts the thread at its
 * entry point via the arch trampoline.
 *
 * pcxi must be the first field (assembly context-switch path).
 */
typedef struct {
	uint32_t pcxi;
} ulmk_arch_ctx_t;

/* Saved interrupt state (ICR register value on TriCore). */
typedef uint32_t ulmk_arch_irq_key_t;

typedef struct {
	volatile uint32_t locked;
} ulmk_spinlock_t;

#define ULMK_SPINLOCK_INIT	{ 0u }

/* Single MPU region descriptor. */
typedef struct {
	uintptr_t base;
	size_t    size;
	uint32_t  perms;	/* UL_PERM_* */
	uint8_t   type;		/* UL_REGION_* */
} ulmk_arch_region_t;

/* Region type tags */
#define ULMK_REGION_CODE		0
#define ULMK_REGION_DATA		1
#define ULMK_REGION_STACK		2
#define ULMK_REGION_HEAP		3
#define ULMK_REGION_PERIPH	4
#define ULMK_REGION_SHARED	5
#define ULMK_REGION_GRANT	6

/* =========================================================================
 * CPU control (arch_api_spec.md §5)
 * ========================================================================= */

ulmk_arch_irq_key_t ulmk_arch_cpu_irq_save(void);
void              ulmk_arch_cpu_irq_restore(ulmk_arch_irq_key_t key);
void              ulmk_arch_cpu_irq_enable(void);
void              ulmk_arch_cpu_irq_disable(void);
void              ulmk_arch_cpu_idle(void);
void              ulmk_arch_cpu_halt(void);
uint32_t          ulmk_arch_cpu_clz(uint32_t val);
uint32_t          ulmk_arch_cpu_id(void);

void              ulmk_arch_spin_lock(ulmk_spinlock_t *lock);
void              ulmk_arch_spin_unlock(ulmk_spinlock_t *lock);
ulmk_arch_irq_key_t ulmk_arch_spin_lock_irqsave(ulmk_spinlock_t *lock);
void              ulmk_arch_spin_unlock_irqrestore(ulmk_spinlock_t *lock,
						    ulmk_arch_irq_key_t key);

void              ulmk_arch_send_ipi(uint32_t cpu_id);
void              ulmk_arch_ipi_clear_self(void);
void              ulmk_arch_ipi_note_enter(void);
void              ulmk_arch_ipi_pulse_self(void);
void              ulmk_arch_secondary_mark_ready(void);
void              ulmk_arch_secondary_init(void);
void              ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void));
void              ulmk_arch_smp_mark_ready(void);
void              ulmk_arch_smp_park(void);

void     ulmk_arch_cycle_enable(void);
uint32_t ulmk_arch_cycle_read(void);

/* =========================================================================
 * Context management (arch_api_spec.md §6)
 * ========================================================================= */

void ulmk_arch_csa_pool_init(uintptr_t pool_base, size_t pool_size);

void ulmk_arch_ctx_init(ulmk_arch_ctx_t *ctx,
		      void (*entry)(void *arg), void *arg,
		      uintptr_t stack_top, ulmk_privilege_t priv);

void ulmk_arch_ctx_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to);

void ulmk_arch_ctx_free(ulmk_arch_ctx_t *ctx);

#define ULMK_SCHED_SWITCH_COOP		0u
#define ULMK_SCHED_SWITCH_PREEMPT_ISR	1u

bool ulmk_arch_sched_isr_preempt_deferred(void);
bool ulmk_arch_sched_defer_to_thread(void);
void ulmk_arch_sched_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to,
			    unsigned int flags);

/* =========================================================================
 * MPU (arch_api_spec.md §7)
 * ========================================================================= */

void ulmk_arch_mpu_init(void);

/* Cache maintenance — no-op stubs (API parity with ARM). */
void ulmk_arch_cache_enable(void);
void ulmk_arch_dcache_clean_all(void);
void ulmk_arch_dcache_invalidate_all(void);
void ulmk_arch_dcache_clean_invalidate_all(void);
void ulmk_arch_icache_invalidate_all(void);
void ulmk_arch_dcache_clean(void *addr, size_t len);
void ulmk_arch_dcache_invalidate(void *addr, size_t len);
void ulmk_arch_dcache_clean_invalidate(void *addr, size_t len);
void ulmk_arch_mpu_enable(void);
void ulmk_arch_mpu_disable(void);

void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			   uint8_t count);

void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			uint8_t prs);

bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size,
				uint32_t perms);

/* =========================================================================
 * IRQ and SRC control (arch_api_spec.md §8)
 * ========================================================================= */

void ulmk_arch_irq_vectors_init(uintptr_t btv, uintptr_t biv,
			      uintptr_t isp_top);

void ulmk_arch_irq_src_configure(uint8_t srpn, uint8_t priority,
			       uint8_t cpu_id);

/*
 * ulmk_arch_irq_src_register — associate a fixed hardware SRC register with
 * an SRPN.  Used for on-chip peripherals whose SRC address is SoC-defined.
 */
void ulmk_arch_irq_src_register(uint8_t srpn, uint32_t src_reg_addr);

void ulmk_arch_irq_src_enable(uint8_t srpn);
void ulmk_arch_irq_src_disable(uint8_t srpn);
void ulmk_arch_irq_src_ack(uint8_t srpn);
bool ulmk_arch_irq_src_is_pending(uint8_t srpn);

/*
 * ulmk_arch_irq_src_trigger — software-raise an IRQ (QEMU / test use only).
 * Sets the SETR bit in the SRC register for @srpn.
 */
void ulmk_arch_irq_src_trigger(uint8_t srpn);

/*
 * Run a userspace IRQ attach callback under @regions (owner map).
 * Caller must set per-CPU irq_attach_owner / irq_attach_srpn first.
 * Sets in_irq_attach for the duration.  Returns the callback bool, or
 * false if a fault aborted the call.
 */
bool ulmk_arch_irq_attach_call(ulmk_irq_attach_fn_t fn, void *data,
			       const ulmk_arch_region_t *regions,
			       uint8_t count);

/* =========================================================================
 * Atomic operations (arch_api_spec.md §10)
 * ========================================================================= */

uint32_t ulmk_arch_atomic_cas(volatile uint32_t *ptr,
			    uint32_t expected, uint32_t desired);

uint32_t ulmk_arch_atomic_add(volatile uint32_t *ptr, uint32_t val);

/* =========================================================================
 * Physical memory allocator (arch_api_spec.md §11 — support)
 * ========================================================================= */


/* =========================================================================
 * Boot entry (arch_api_spec.md §11)
 * ========================================================================= */

/*
 * ulmk_kern_start - common C runtime bring-up; does not return
 * Entered from startup.S once the CPU-mandatory prologue (stack, and on
 * TriCore the CSA free list + small-data anchors) is done, with interrupts
 * disabled.  Relocates .data, clears BSS, then calls ulmk_board_init(),
 * ulmk_arch_init() and ulmk_kern_main().  Lives in kernel/init/init.c.
 */
void ulmk_kern_start(void);

/*
 * ulmk_board_init - board-level hardware setup, called by ulmk_kern_start()
 * before the .data copy (PLL, flash wait states, external RAM).
 *
 * A definition must always be linked: the board provides one in its
 * board_services.c, or a no-op comes from stub/board_init_stub.c.  The
 * reference is deliberately NOT weak — under static-library packaging (the
 * SDK) a weak reference would fail to pull the board's definition out of the
 * archive and silently resolve to address 0, crashing at boot.
 */
void ulmk_board_init(void);

/*
 * CPU EndInit unlock/relock for EndInit-protected CSFRs (BTV, BIV, ISP,
 * SYSCON.PROTEN, …).  Board provides a strong implementation (WDT password
 * sequence).  Weak no-ops in arch cover QEMU / boards without EndInit.
 *
 * OpenOCD's CBS_OCNTRL unlock makes these writes succeed without EndInit on
 * the flash path; button/PORST do not — without these hooks BTV/ISP stay at
 * reset defaults and the first SYSCALL class-4 traps (red ESR0).
 */
void ulmk_board_cpu_endinit_clear(void);
void ulmk_board_cpu_endinit_set(void);

/*
 * ulmk_arch_init - one-time CPU and peripheral initialisation
 * Called by ulmk_kern_start() after ulmk_board_init() and after .data/.bss
 * init.  Fills @info and returns; control passes to ulmk_kern_main().
 */
void ulmk_arch_init(ulmk_boot_info_t *info);

/* Periodic kernel tick (STM0 CMP).  Called by kernel only. */
void ulmk_arch_tick_init(uint32_t tick_hz);
void ulmk_arch_tick_ack(void);

/*
 * Index of the timing wheel used by timer_add/tick on this arch.
 *
 * TriCore / RISC-V / ARM: per-hart local tick → return ulmk_arch_cpu_id().
 * (Older TriCore SMP briefly pinned all sleeps to wheel 0 after STM0
 * CMP1 races; per-core STM0/1/2 restores the normal model.)
 */
uint32_t ulmk_arch_timer_wheel_cpu(void);

/* =========================================================================
 * Arch-internal syscall entry (arch/tricore/arch.c)
 * vectors.S parks TIN + D4–D7 on the stack and passes that frame pointer;
 * this entry switches to kernel PRS and dispatches ulmk_kern_trap_syscall().
 * ========================================================================= */

void ulmk_arch_syscall_entry(uint32_t frame_ptr);

/*
 * ulmk_arch_trap_entry — arch-level fault dispatcher; called from vectors.S.
 * Reads PSW to determine context, performs the arch-specific dump, then
 * calls ulmk_kern_trap_recoverable() or ulmk_kern_trap_panic().
 * Does not return.
 */
void ulmk_arch_trap_entry(uint8_t trap_class, uint8_t tin);

/*
 * ulmk_arch_trap_dump — dump arch-specific CPU state after a hardware fault.
 * Called by ulmk_arch_trap_entry(); output goes via ulmk_printk_char_out so it
 * remains safe even before the kernel is fully initialised.
 */
void ulmk_arch_trap_dump(uint8_t trap_class, uint8_t tin);

/*
 * ulmk_printk_char_out — single-character output primitive.
 * Provided by the board (e.g. boards/qemu_tc3xx/qemu_console.c) as a weak
 * symbol.  Both the kernel print subsystem and arch fault handlers may call
 * this directly; it is declared here so the arch layer can use it without
 * depending on kernel-internal headers.
 */
void ulmk_printk_char_out(char c);

/* =========================================================================
 * Kernel callbacks — implemented by kernel/, called by arch port
 * (arch_api_spec.md §12)
 *
 * These functions contain NO arch-specific code.  The arch port is
 * responsible for extracting all hardware-dependent state before calling.
 * ========================================================================= */

/*
 * ulmk_kern_irq_dispatch — route a hardware IRQ to its notification object.
 * @srpn: SRPN (priority number) of the interrupt that fired.
 * Called from the generic ISR stub before RSLCX/RFE.
 */
void ulmk_kern_irq_dispatch(uint8_t srpn);
void ulmk_kern_ipi_resched(void);
void ulmk_kern_timer_tick(void);
#if ULMK_CONFIG_ENABLE_SMP
void ulmk_kern_ipi_from_isr(void);
#endif
void ulmk_kern_secondary_main(void);

/*
 * ulmk_kern_sched_dispatch — run trap-exit preemption after IRQ or syscall.
 * @from_isr: true when called from an ISR epilogue, false from syscall return.
 */
void ulmk_kern_sched_dispatch(bool from_isr);

/*
 * ulmk_kern_syscall_ret_resolve — after trap-exit switch, apply staged
 * block_status / wake return overrides for the resumed thread.
 */
uint32_t ulmk_kern_syscall_ret_resolve(uint32_t ret);

/*
 * ulmk_kern_trap_syscall — dispatch a SYSCALL trap (class 6).
 * @tin:  trap identification number (= syscall number, 0–127)
 * @args: arguments read from D4–D7 by ulmk_arch_syscall_entry()
 * Returns the value to be written to D2 (done by ulmk_arch_syscall_entry).
 * Called from ulmk_arch_syscall_entry() in arch/tricore/arch.c.
 */
uint32_t ulmk_kern_trap_syscall(uint8_t tin, uint32_t args[4]);

/*
 * ulmk_kern_trap_recoverable — kill the current thread and reschedule.
 * Called by ulmk_arch_trap_entry() when a class-1 fault is detected in
 * a user/driver thread.  Contains no arch-specific code.
 */
void ulmk_kern_trap_recoverable(void);

/*
 * ulmk_irq_in_attach — true while a userspace IRQ attach callback runs.
 * Used by arch trap paths to prefer owner-kill over system panic.
 */
bool ulmk_irq_in_attach(void);

/*
 * ulmk_kern_trap_panic — halt the system after an unrecoverable fault.
 * Called by ulmk_arch_trap_entry() for all faults that cannot be isolated
 * to a single user thread (kernel, ISR, or non-class-1 faults).
 * Contains no arch-specific code.
 */
void ulmk_kern_trap_panic(void);

/*
 * ulmk_kern_main - platform-independent kernel entry; does not return
 * Called by ulmk_kern_start() after ulmk_arch_init().
 */
void ulmk_kern_main(const ulmk_boot_info_t *info);

#endif /* ULMK_ARCH_H */
