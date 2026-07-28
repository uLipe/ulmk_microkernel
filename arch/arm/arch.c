/* SPDX-License-Identifier: MIT */
/*
 * ARM Cortex-M arch port — arch/arm/arch.c
 *
 * Covers ARMv7-M (Cortex-M4/M7) and ARMv8-M mainline (Cortex-M33).  CPU
 * control, context management, the SVC/SysTick/IRQ/fault C dispatchers, atomics
 * and boot bring-up live here; the MPU register programming lives in
 * mpu_v7m.c / mpu_v8m.c (selected by ULMK_ARCH_ARMV8M).
 *
 * Context switch model: exceptions run on MSP, so each thread owns a private
 * kernel stack and ulmk_arch_ctx_switch (ctx_switch.S) swaps MSP for a
 * synchronous coroutine switch.  PendSV is never used.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <ulmk_arch.h>

/* SysTick reload: board may override; default derives from CPU clock / tick. */
#ifndef BOARD_CPU_HZ
#define BOARD_CPU_HZ		25000000u	/* QEMU mps2 default core clock */
#endif
#ifndef ULMK_CONFIG_TICK_HZ
#define ULMK_CONFIG_TICK_HZ	1000u
#endif

#define REG32(a)	(*(volatile uint32_t *)(uintptr_t)(a))
#define REG8(a)		(*(volatile uint8_t  *)(uintptr_t)(a))

static void dump_puts(const char *s);
static void dump_hex8(uint32_t v);

/*
 * Set by ctx_switch.S before it raises SVC #ULMK_ARCH_SVC_LAUNCH to start the
 * very first thread from Handler mode.  Points at the thread's arch context.
 */
void *g_arm_first_launch;

/* =========================================================================
 * CPU control
 * ========================================================================= */

#define ULMK_IRQ_KEY_SKIP	(1u << 31)

ulmk_arch_irq_key_t ulmk_arch_cpu_irq_save(void)
{
	uint32_t primask;

	__asm__ volatile("mrs %0, primask" : "=r"(primask));
	/* PRIMASK==1 ⇒ IRQs already masked (syscall / nested). */
	if (primask & 1u)
		return (ulmk_arch_irq_key_t)(primask | ULMK_IRQ_KEY_SKIP);
	__asm__ volatile("cpsid i" ::: "memory");
	return primask;
}

void ulmk_arch_cpu_irq_restore(ulmk_arch_irq_key_t key)
{
	uint32_t primask = (uint32_t)key;

	if (primask & ULMK_IRQ_KEY_SKIP)
		return;
	__asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}

void ulmk_arch_cpu_irq_enable(void)
{
	__asm__ volatile("cpsie i" ::: "memory");
}

void ulmk_arch_cpu_irq_disable(void)
{
	__asm__ volatile("cpsid i" ::: "memory");
}

void ulmk_arch_cpu_idle(void)
{
#if ULMK_ARCH_IDLE_IS_WFI
	__asm__ volatile("wfi" ::: "memory");
#else
	__asm__ volatile("nop");
#endif
}

void ulmk_arch_cpu_halt(void)
{
	for (;;)
		;
}

uint32_t ulmk_arch_cpu_clz(uint32_t val)
{
	if (val == 0u)
		return 32u;
	return (uint32_t)__builtin_clz(val);
}

#if ULMK_CONFIG_SYSCALL_WCET
#define DEMCR		0xE000EDFCu
#define DWT_CTRL	0xE0001000u
#define DWT_CYCCNT	0xE0001004u
#define DEMCR_TRCENA	(1u << 24)
#define DWT_CYCCNTENA	(1u << 0)

void ulmk_arch_cycle_enable(void)
{
	REG32(DEMCR) |= DEMCR_TRCENA;
	REG32(DWT_CYCCNT) = 0u;
	REG32(DWT_CTRL) |= DWT_CYCCNTENA;
}

uint32_t ulmk_arch_cycle_read(void)
{
	return REG32(DWT_CYCCNT);
}
#else
void ulmk_arch_cycle_enable(void)
{
}

uint32_t ulmk_arch_cycle_read(void)
{
	return 0u;
}
#endif

/* =========================================================================
 * Context management
 * ========================================================================= */

void ulmk_arch_csa_pool_init(uintptr_t pool_base, size_t pool_size)
{
	(void)pool_base;
	(void)pool_size;
}

/*
 * Carve a private kernel stack from the top of the thread stack and record the
 * user entry parameters; the fabricated thread is launched lazily by
 * ulmk_arch_ctx_switch (see ctx_switch.S _arm_launch_fresh) on first schedule.
 */
void ulmk_arch_ctx_init(ulmk_arch_ctx_t *ctx,
			void (*entry)(void *arg), void *arg,
			uintptr_t stack_top, ulmk_privilege_t priv)
{
	uint32_t control;

	stack_top &= ~0x7u;

	control = ULMK_ARCH_CONTROL_SPSEL;		/* threads run on PSP */
	if (priv != ULMK_PRIV_KERNEL)
		control |= ULMK_ARCH_CONTROL_NPRIV;	/* driver/user = unprivileged */

	ctx->kstack_top = (uint32_t)stack_top;
	ctx->user_sp    = (uint32_t)(stack_top - ULMK_ARCH_KSTACK_SIZE);
	ctx->entry      = (uint32_t)(uintptr_t)entry;
	ctx->arg        = (uint32_t)(uintptr_t)arg;
	ctx->control    = control;
	ctx->ksp        = 0u;
	ctx->fresh      = 1u;
}

void ulmk_arch_ctx_free(ulmk_arch_ctx_t *ctx)
{
	if (ctx) {
		ctx->ksp   = 0u;
		ctx->fresh = 0u;
	}
}

bool ulmk_arch_sched_isr_preempt_deferred(void)
{
	return false;
}

/*
 * Cortex-M exceptions run in Handler mode on MSP; the synchronous coroutine
 * switch is safe there.
 */
void ulmk_arch_sched_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to,
			    unsigned int flags)
{
	(void)flags;
	ulmk_arch_ctx_switch(from, to);
}

/* =========================================================================
 * Exception C dispatchers (entry stubs in trap.S)
 * ========================================================================= */

/*
 * SVCall.  @frame points at the caller's stacked exception frame on PSP:
 *   [0]=r0 [1]=r1 [2]=r2 [3]=r3 [4]=r12 [5]=lr [6]=pc [7]=xpsr
 * The syscall number is the immediate of the "svc #imm" instruction that
 * raised the exception (the halfword just below the stacked return PC).
 */
void _arm_svc_dispatch(uint32_t *frame)
{
	const uint16_t *pc;
	uint32_t        args[4];
	uint32_t        ret;
	uint8_t         nr;

	pc = (const uint16_t *)(uintptr_t)frame[6];
	nr = (uint8_t)(pc[-1] & 0xFFu);

	args[0] = frame[0];
	args[1] = frame[1];
	args[2] = frame[2];
	args[3] = frame[3];

	ret = ulmk_kern_trap_syscall(nr, args);
	ulmk_kern_sched_dispatch(false);
	ret = ulmk_kern_syscall_ret_resolve(ret);
	frame[0] = ret;
	ulmk_kern_trap_mpu_restore();
}

void _arm_systick_dispatch(void)
{
	ulmk_kern_timer_tick();
	ulmk_kern_trap_mpu_restore();
}

extern uint8_t _arm_irq_line_to_srpn(uint32_t line);

void _arm_irq_dispatch(uint32_t line)
{
	uint8_t srpn = _arm_irq_line_to_srpn(line);

	/*
	 * Mask the NVIC line before signalling.  A peripheral IRQ stays asserted
	 * (level) until its driver clears the source, so leaving the line enabled
	 * would re-enter this handler on every exception exit — a storm that
	 * starves the (lower-priority) driver thread that would clear it.  The
	 * driver re-enables the line via ulmk_irq_enable() after acking.  This
	 * mirrors the RISC-V backend, which quiesces the source inside dispatch.
	 */
	ulmk_arch_irq_src_disable(srpn);
	ulmk_kern_irq_dispatch(srpn);
	ulmk_kern_sched_dispatch(true);
	ulmk_kern_trap_mpu_restore();
}

/* =========================================================================
 * Atomics (single-core: mask interrupts)
 * ========================================================================= */

uint32_t ulmk_arch_atomic_cas(volatile uint32_t *ptr,
			      uint32_t expected, uint32_t desired)
{
	ulmk_arch_irq_key_t key;
	uint32_t            old;

	key = ulmk_arch_cpu_irq_save();
	old = *ptr;
	if (old == expected)
		*ptr = desired;
	ulmk_arch_cpu_irq_restore(key);
	return old;
}

uint32_t ulmk_arch_atomic_add(volatile uint32_t *ptr, uint32_t val)
{
	ulmk_arch_irq_key_t key;
	uint32_t            old;

	key = ulmk_arch_cpu_irq_save();
	old  = *ptr;
	*ptr = old + val;
	ulmk_arch_cpu_irq_restore(key);
	return old;
}

/* =========================================================================
 * Trap diagnostics
 * ========================================================================= */

static void dump_puts(const char *s)
{
	while (*s)
		ulmk_printk_char_out(*s++);
}

static void dump_hex8(uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	char              buf[11];
	int               i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++)
		buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xFu];
	buf[10] = '\0';
	dump_puts(buf);
}

void ulmk_arch_trap_dump(uint8_t trap_class, uint8_t tin)
{
	(void)trap_class;
	(void)tin;
	dump_puts(" cfsr=");
	dump_hex8(REG32(ULMK_ARCH_SCB_CFSR));
	dump_puts(" hfsr=");
	dump_hex8(REG32(ULMK_ARCH_SCB_HFSR));
	dump_puts(" mmfar=");
	dump_hex8(REG32(ULMK_ARCH_SCB_MMFAR));
	dump_puts(" bfar=");
	dump_hex8(REG32(ULMK_ARCH_SCB_BFAR));
	dump_puts("\n");
}

void ulmk_arch_trap_entry(uint8_t trap_class, uint8_t tin)
{
	/*
	 * HardFault / unexpected paths: policy is panic.  MemManage / Bus /
	 * Usage from Thread+PSP are recovered in _arm_fault_dispatch before
	 * reaching here (kill via ulmk_user_thread_entry / thread_exit).
	 */
	dump_puts("TRAP class=");
	dump_hex8((uint32_t)trap_class);
	dump_puts(" exc=");
	dump_hex8((uint32_t)tin);
	ulmk_arch_trap_dump(trap_class, tin);
	ulmk_kern_trap_panic();
}

/*
 * Fault / NMI C dispatcher (entry stub _arm_fault_entry).  @exc is the IPSR
 * exception number, @frame the offending PSP frame.  Faults are fatal.
 */
#ifdef ULMK_TEST_BUILD
/*
 * QEMU only honours M-profile semihosting (BKPT 0xAB) from privileged mode
 * (translate.c: semihosting_enabled(current_el == 0)).  Integration tests run
 * their root/worker threads unprivileged, so tests/arch/arm_test_io.c issues a
 * BKPT 0xAB that escalates here to a HardFault.  Re-issue it from this handler
 * (Handler mode, privileged) so the finisher actually stops the machine.
 */
static void arm_test_semihost_reissue(uint32_t r0, uint32_t r1)
{
	register uint32_t a0 __asm__("r0") = r0;
	register uint32_t a1 __asm__("r1") = r1;

	__asm__ volatile("bkpt 0xAB" : : "r"(a0), "r"(a1) : "memory");
}
#endif

/*
 * @exc         IPSR exception number (3=HardFault, 4=MemManage, 5=BusFault,
 *              6=UsageFault, 2=NMI).
 * @frame       stacked exception frame (r0-r3,r12,lr,pc,xpsr).
 * @exc_return  EXC_RETURN token: bit2 set => the fault came from a Thread-mode
 *              context running on PSP.
 *
 * A memory/usage fault taken from an unprivileged thread is recoverable: rewrite
 * the stacked PC to ulmk_thread_exit so the offending thread terminates itself
 * through the normal syscall path once we return.  Everything else is fatal.
 */
void _arm_fault_dispatch(uint32_t exc, uint32_t *frame, uint32_t exc_return)
{
#ifdef ULMK_TEST_BUILD
	if (frame) {
		const uint16_t *fpc = (const uint16_t *)(uintptr_t)frame[6];

		if (fpc && *fpc == 0xbeabu)
			arm_test_semihost_reissue(frame[0], frame[1]);
	}
#endif
	/*
	 * Recoverable when the fault came from a Thread-mode context on PSP
	 * (exc_return bit2 set, stacked xPSR exception field zero).  Mirrors the
	 * TriCore port, which keys recovery on the interrupted context rather
	 * than on kernel scheduler state — arch/ must not reach into kernel
	 * internals.  Handler-mode (kernel/ISR) faults are always fatal.
	 */
	if (frame && (exc_return & 0x4u) && (frame[7] & 0x1FFu) == 0u &&
	    (exc == 4u || exc == 5u || exc == 6u)) {
		extern void ulmk_user_thread_entry(void (*)(void *), void *);

		/*
		 * Redirect the offending thread into the shared user-text entry
		 * with a NULL entry pointer: it skips straight to ulmk_thread_exit
		 * so the thread is reaped through the normal syscall path.  The
		 * target must live in user text — the microkernel syscall wrappers
		 * are static-inline, so &ulmk_thread_exit in kernel code would
		 * resolve to a kernel-text copy an unprivileged thread cannot run.
		 */
		REG32(ULMK_ARCH_SCB_CFSR) = REG32(ULMK_ARCH_SCB_CFSR);
		frame[0] = 0u;	/* r0 = entry = NULL */
		/*
		 * Stacked ReturnAddress must be halfword-aligned; Thumb mode is
		 * carried by EPSR.T (already set in the stacked xPSR).  ELF
		 * function symbols have bit0 set — clear it before EXC_RETURN.
		 */
		frame[6] = (uint32_t)(uintptr_t)&ulmk_user_thread_entry & ~1u;
		return;
	}

	/* ISR attach callback fault — kill owner, keep the system up. */
	if (ulmk_irq_in_attach() && (exc == 4u || exc == 5u || exc == 6u)) {
		REG32(ULMK_ARCH_SCB_CFSR) = REG32(ULMK_ARCH_SCB_CFSR);
		ulmk_kern_trap_recoverable();
		for (;;)
			;
	}

	dump_puts("FAULT exc=");
	dump_hex8(exc);
	if (frame) {
		dump_puts(" pc=");
		dump_hex8(frame[6]);
		dump_puts(" lr=");
		dump_hex8(frame[5]);
	}
	dump_puts("\n");
	ulmk_arch_trap_entry(1u, (uint8_t)exc);
}

void ulmk_arch_syscall_entry(void)
{
}

/* =========================================================================
 * SysTick — periodic kernel timing-wheel tick
 * ========================================================================= */

void ulmk_arch_tick_init(uint32_t tick_hz)
{
	uint32_t reload;

	if (tick_hz == 0u)
		tick_hz = 1000u;

	reload = BOARD_CPU_HZ / tick_hz;
	if (reload == 0u)
		reload = 1u;
	reload -= 1u;
	if (reload > 0x00FFFFFFu)
		reload = 0x00FFFFFFu;

	REG32(ULMK_ARCH_SYSTICK_VAL)  = 0u;
	REG32(ULMK_ARCH_SYSTICK_LOAD) = reload;
	REG32(ULMK_ARCH_SYSTICK_CTRL) = ULMK_ARCH_SYSTICK_ENABLE |
					ULMK_ARCH_SYSTICK_TICKINT |
					ULMK_ARCH_SYSTICK_CLKSOURCE;
}

void ulmk_arch_tick_ack(void)
{
	/* SysTick auto-reloads; COUNTFLAG clear is implicit on handler entry. */
}

uint32_t ulmk_arch_timer_wheel_cpu(void)
{
	return ulmk_arch_cpu_id();
}

/*
 * All configurable exceptions share one priority level so SVC, SysTick and
 * external IRQs never preempt each other (they tail-chain instead) — this keeps
 * the coroutine kernel-stack invariant simple.  Faults keep their higher fixed
 * priority.  Priority fields are the top bits; 0xA0 leaves headroom above.
 */
#define ULMK_ARCH_EXC_PRIO	0xA0u

static void exc_priorities_init(void)
{
	/* SHPR2[31:24] = SVCall; SHPR3[23:16]=PendSV, [31:24]=SysTick */
	REG32(ULMK_ARCH_SCB_SHPR2) = (uint32_t)ULMK_ARCH_EXC_PRIO << 24;
	REG32(ULMK_ARCH_SCB_SHPR3) = ((uint32_t)ULMK_ARCH_EXC_PRIO << 24) |
				     ((uint32_t)ULMK_ARCH_EXC_PRIO << 16);

	/* Enable dedicated fault handlers (else everything escalates to HardFault). */
	REG32(ULMK_ARCH_SCB_SHCSR) |= ULMK_ARCH_SHCSR_MEMFAULTENA |
				      ULMK_ARCH_SHCSR_BUSFAULTENA |
				      ULMK_ARCH_SHCSR_USGFAULTENA;

	/*
	 * CCR.USERSETMPEND: let unprivileged (driver) threads pend an NVIC line
	 * via STIR.  Drivers own their interrupt sources in this model, so the
	 * software-trigger path (used by driver self-signalling and the IRQ
	 * integration test) must work without a syscall round-trip.
	 */
	REG32(ULMK_ARCH_SCB_CCR) |= (1u << 1);
}

/* =========================================================================
 * Boot
 * ========================================================================= */

void ulmk_arch_init(ulmk_boot_info_t *info)
{
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_end[];

	if (info) {
		info->mem_count     = 1u;
		info->mem[0].base   = (uintptr_t)_ulmk_user_ram_start;
		info->mem[0].size   = (uintptr_t)_ulmk_user_pool_end -
				      (uintptr_t)_ulmk_user_ram_start;
		info->csa_pool_base = 0u;
		info->csa_pool_size = 0u;
	}

	exc_priorities_init();
	ulmk_arch_mpu_init();
	/*
	 * MPU attrs must be live before enabling I/D-cache so SDRAM/FB and
	 * Normal RAM pick up WB vs Device correctly.
	 */
	ulmk_arch_cache_enable();
	ulmk_arch_irq_vectors_init(0u, 0u, 0u);
	/* SysTick is armed lazily on the first context switch (see sched_switch). */
}

/* =========================================================================
 * SMP — not supported on Cortex-M; stubs keep the arch API complete
 * ========================================================================= */

uint32_t ulmk_arch_cpu_id(void)
{
	return 0u;
}

void ulmk_arch_spin_lock(ulmk_spinlock_t *lock)
{
#if ULMK_CONFIG_ENABLE_SMP
	while (ulmk_arch_atomic_cas(&lock->locked, 0u, 1u) != 0u)
		;
#else
	(void)lock;
#endif
}

void ulmk_arch_spin_unlock(ulmk_spinlock_t *lock)
{
#if ULMK_CONFIG_ENABLE_SMP
	lock->locked = 0u;
#else
	(void)lock;
#endif
}

ulmk_arch_irq_key_t ulmk_arch_spin_lock_irqsave(ulmk_spinlock_t *lock)
{
	ulmk_arch_irq_key_t key = ulmk_arch_cpu_irq_save();

	ulmk_arch_spin_lock(lock);
	return key;
}

void ulmk_arch_spin_unlock_irqrestore(ulmk_spinlock_t *lock,
				      ulmk_arch_irq_key_t key)
{
	ulmk_arch_spin_unlock(lock);
	ulmk_arch_cpu_irq_restore(key);
}

void ulmk_arch_send_ipi(uint32_t cpu_id)
{
	(void)cpu_id;
}

void ulmk_arch_ipi_note_enter(void)
{
}

void ulmk_arch_ipi_clear_self(void)
{
}

void ulmk_arch_ipi_pulse_self(void)
{
}

void ulmk_arch_secondary_init(void)
{
}

void ulmk_arch_secondary_mark_ready(void)
{
}

void ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void))
{
	(void)cpu_id;
	(void)entry;
}

void ulmk_arch_smp_park(void)
{
	for (;;)
		ulmk_arch_cpu_halt();
}

void ulmk_arch_smp_mark_ready(void)
{
}
