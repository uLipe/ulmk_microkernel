/* SPDX-License-Identifier: MIT */
/*
 * C29 architecture implementation — arch/c29/arch.c
 *
 * Covers: CPU control, spinlocks, atomics, context fabrication, MPU stubs,
 * and the arch-init entry point.  IRQ/tick lives in irq.c; SSU in mpu.c;
 * SMP stubs in smp.c; context switch in ctx_switch.S.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <ulmk/config.h>
#include <ulmk_arch.h>
#include <ulmk/microkernel.h>
#include <ulmk/platform.h>

/* Forward declarations from ctx_switch.S and irq.c */
extern void ulmk_c29_tick_isr(void);
extern void ulmk_c29_syscall_isr(void);
extern void ulmk_c29_yield_isr(void);
extern void ulmk_c29_thread_trampoline(void);

/* Staged by ulmk_arch_sched_switch; consumed by INT veneer on RETI.INT. */
extern ulmk_arch_ctx_t *g_c29_preempt_old_ctx;
extern ulmk_arch_ctx_t *g_c29_preempt_new_ctx;
extern uint32_t g_c29_int_depth;

void ulmk_c29_run_thread(void *arg, void (*entry)(void *arg))
{
	entry(arg);
	ulmk_thread_exit();
}

/*
 * NMI path: INT-nested faults panic; otherwise restore APR shadow and
 * let the kernel kill the current user thread if any.
 */
void ulmk_c29_nmi_dispatch(void)
{
	if (g_c29_int_depth != 0u) {
		ulmk_kern_trap_panic();
		return;
	}
	ulmk_kern_trap_mpu_restore();
	ulmk_kern_trap_recoverable();
}

/*
 * PIPE INT_CTL_H register for a given channel: force flag (FLAG_FRC) is bit 0.
 * PIPE base is overrideable from board_config.h.
 */
#define PIPE_INT_CTL_H(n) \
	(*((volatile uint32_t *)(ULMK_ARCH_PIPE_INT_CTL_H(n))))

#define PIPE_INT_VECT(n) \
	(*((volatile uint32_t *)(ULMK_ARCH_PIPE_INT_VECT(n))))

/* DSTS register (read via __builtin or memory-mapped alias). */
#define C29_DSTS_INTE		(1u << 0)

/* ---------------------------------------------------------------------------
 * CPU control
 * ---------------------------------------------------------------------------
 */

ulmk_arch_irq_key_t ulmk_arch_cpu_irq_save(void)
{
	ulmk_arch_irq_key_t key;

	__asm__ volatile(
		"ST.32	%0, DSTS\n\t"
		"DISINT"
		: "=m"(key) : : "memory");
	return key;
}

void ulmk_arch_cpu_irq_restore(ulmk_arch_irq_key_t key)
{
	if (key & C29_DSTS_INTE)
		__asm__ volatile("ENINT" ::: "memory");
}

void ulmk_arch_cpu_irq_enable(void)
{
	__asm__ volatile("ENINT" ::: "memory");
}

void ulmk_arch_cpu_irq_disable(void)
{
	__asm__ volatile("DISINT" ::: "memory");
}

void ulmk_arch_cpu_idle(void)
{
	__asm__ volatile("IDLE" ::: "memory");
}

void ulmk_arch_cpu_halt(void)
{
	for (;;)
		__asm__ volatile("IDLE" ::: "memory");
}

uint32_t ulmk_arch_cpu_clz(uint32_t val)
{
	return (uint32_t)__builtin_c29_i32_clzeros_d((int32_t)val);
}

#if ULMK_CONFIG_ENABLE_SMP
/*
 * CORE_ID CSFR is Link-2 only and awkward to read from C with this
 * toolchain.  Boot stubs stamp g_c29_my_cpu before park; CPU0 sets 0
 * in ulmk_arch_init().
 */
volatile uint32_t g_c29_my_cpu;
#endif

uint32_t ulmk_arch_cpu_id(void)
{
#if ULMK_CONFIG_ENABLE_SMP
	return g_c29_my_cpu;
#else
	return 0u;
#endif
}

/* ---------------------------------------------------------------------------
 * Spinlocks
 * ---------------------------------------------------------------------------
 */

void ulmk_arch_spin_lock(ulmk_spinlock_t *lock)
{
	uint32_t old;

	do {
		__builtin_c29_atomic_mem_enter();
		old = lock->locked;
		if (!old) {
			lock->locked = 1u;
		}
		__builtin_c29_atomic_leave();
	} while (old);
}

void ulmk_arch_spin_unlock(ulmk_spinlock_t *lock)
{
	__asm__ volatile("" ::: "memory");
	lock->locked = 0u;
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

/* ---------------------------------------------------------------------------
 * Atomics
 * ---------------------------------------------------------------------------
 */

uint32_t ulmk_arch_atomic_cas(volatile uint32_t *ptr,
			      uint32_t expected, uint32_t desired)
{
	uint32_t old;

	__builtin_c29_atomic_mem_enter();
	old = *ptr;
	if (old == expected) {
		*ptr = desired;
	}
	__builtin_c29_atomic_leave();
	return old;
}

uint32_t ulmk_arch_atomic_add(volatile uint32_t *ptr, uint32_t val)
{
	uint32_t old;

	__builtin_c29_atomic_mem_enter();
	old = *ptr;
	*ptr = old + val;
	__builtin_c29_atomic_leave();
	return old;
}

/* ---------------------------------------------------------------------------
 * Cycle counter (CPUTIMER2 free-run)
 * ---------------------------------------------------------------------------
 */

static volatile uint32_t *_timer2_tim(void)
{
	/* TIM register is at offset 0 from CPUTIMER2_BASE. */
	return (volatile uint32_t *)ULMK_BOARD_CPUTIMER2_BASE;
}

void ulmk_arch_cycle_enable(void)
{
	/*
	 * CPUTIMER2 is started in ulmk_arch_tick_init(); nothing extra needed
	 * for the cycle-read path — TIM counts down continuously.
	 */
}

uint32_t ulmk_arch_cycle_read(void)
{
	return *_timer2_tim();
}

/* ---------------------------------------------------------------------------
 * CSA pool init — no-op for C29 (no CSA mechanism)
 * ---------------------------------------------------------------------------
 */

void ulmk_arch_csa_pool_init(uintptr_t pool_base, size_t pool_size)
{
	(void)pool_base;
	(void)pool_size;
}

/* ---------------------------------------------------------------------------
 * Context fabrication
 *
 * Software INT frame (ULMK_SAVE_CONTEXT), stack grows upward:
 *   +0   A14
 *   +4   RPC
 *   +8   DSTS
 *   +12  ESTS
 *   +16  XA0..XA12  (56 bytes)
 *   +72  XD0..XD14  (64 bytes)
 *   +136 XM0..XM30 (128 bytes)
 * Total software frame = 264 bytes.
 *
 * RET / RETI.INT both consume an extra 8-byte slot below A14 (previous RPC
 * pushed by CALL or by INT entry).  Fabricated threads never took a real INT,
 * so we reserve that slot explicitly — same idea as FreeRTOS C29
 * pxPortInitialiseStack.
 * ---------------------------------------------------------------------------
 */

#define CTX_SOFT_SIZE	264u
#define CTX_RETI_PAD	8u
#define CTX_TOTAL_SIZE	(CTX_SOFT_SIZE + CTX_RETI_PAD)

/* Offsets within the soft frame (after the RETI pad). */
#define TF_RPC		4u
#define TF_DSTS		8u
#define TF_ESTS		12u
/* XA0 at +16; A4 is word index 4 of A0..A13 → offset 16+16 = 32 */
#define TF_A4		32u
#define TF_A5		36u

/* Match FreeRTOS C29 fabricated DSTS/ESTS (INT-enabled leaf context). */
#define C29_FAB_DSTS	0x07F90001u
#define C29_FAB_ESTS	0x00020101u

void ulmk_arch_ctx_init(ulmk_arch_ctx_t *ctx,
			void (*entry)(void *arg), void *arg,
			uintptr_t stack_top, ulmk_privilege_t priv)
{
	uint8_t *base;
	uint8_t *frame;
	uint32_t *w;
	uint32_t i;

	(void)priv;	/* privilege enforced by SSU Links/APRs */

	/*
	 * Stack grows upward.  Caller passes the low address of the stack
	 * region (see ULMK_ARCH_STACK_GROWS_UP).  Place RETI pad + soft
	 * frame at the base; SP starts just past the soft frame.
	 */
	base = (uint8_t *)((stack_top + 7u) & ~(uintptr_t)7u);

	for (i = 0u; i < CTX_TOTAL_SIZE; i++)
		base[i] = 0;

	w = (uint32_t *)base;
	/* Slot popped by RET/RETI.INT after restoring the soft frame. */
	w[0] = (uint32_t)(uintptr_t)ulmk_c29_thread_trampoline & ~1u;
	w[1] = C29_FAB_DSTS;

	frame = base + CTX_RETI_PAD;
	w = (uint32_t *)frame;

	/*
	 * RPC = trampoline.  Trampoline CALLs ulmk_c29_run_thread with
	 * A4=arg and A5=entry already in the fabricated frame.
	 */
	w[TF_RPC / 4u] = (uint32_t)(uintptr_t)ulmk_c29_thread_trampoline &
			 ~1u;
	w[TF_DSTS / 4u] = C29_FAB_DSTS;
	w[TF_ESTS / 4u] = C29_FAB_ESTS;
	w[TF_A4 / 4u] = (uint32_t)(uintptr_t)arg;
	w[TF_A5 / 4u] = (uint32_t)(uintptr_t)entry;

	ctx->sp = (uint32_t)((uintptr_t)frame + CTX_SOFT_SIZE);
}

void ulmk_arch_ctx_free(ulmk_arch_ctx_t *ctx)
{
	/* No dynamic allocation — nothing to release. */
	(void)ctx;
}

/* ---------------------------------------------------------------------------
 * Scheduler switch helpers
 * ---------------------------------------------------------------------------
 */

bool ulmk_arch_sched_isr_preempt_deferred(void)
{
	/* INT veneer already saved the frame; switch applies on RETI.INT. */
	return true;
}

void ulmk_arch_sched_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to,
			    unsigned int flags)
{
	/*
	 * Syscall and IRQ share the INT veneer.  Any switch requested while
	 * an INT is active must be deferred to RETI.INT — a direct RET coop
	 * switch would leave the PIPE INT active and block further ticks.
	 */
	if (g_c29_int_depth != 0u ||
	    flags == ULMK_SCHED_SWITCH_PREEMPT_ISR) {
		g_c29_preempt_old_ctx = from;
		g_c29_preempt_new_ctx = (ulmk_arch_ctx_t *)to;
		return;
	}
	ulmk_arch_ctx_switch(from, to);
}

/* ---------------------------------------------------------------------------
 * Arch init — called by ulmk_kern_start() after .data/.bss are ready
 * ---------------------------------------------------------------------------
 */

extern void ulmk_kern_main(const ulmk_boot_info_t *info);

void ulmk_arch_init(ulmk_boot_info_t *info)
{
	/*
	 * Clear the Data Line Buffer enable bits before any shared RAM use.
	 * See errata SPRZ569E; arch_config.h defines the register address.
	 */
	volatile uint32_t *dlb = (volatile uint32_t *)ULMK_ARCH_MEM_DLB_CONFIG;

	*dlb &= ~(ULMK_ARCH_MEM_DLB_CPU1_EN |
		  ULMK_ARCH_MEM_DLB_CPU2_EN |
		  ULMK_ARCH_MEM_DLB_CPU3_EN |
		  (1u << 6)); /* SYNCBRIDGE_DLB_EN */

	/*
	 * Fill boot_info with the CPU1 RAM region and tick clock.
	 * Linker symbols for the user pool bound come from the generated .cmd.
	 */
	extern char _ulmk_user_pool_start[];
	extern char _ulmk_user_pool_end[];
	extern char _ulmk_isr_stack_top[];

	info->mem[0].base = (uintptr_t)_ulmk_user_pool_start;
	info->mem[0].size = (size_t)(_ulmk_user_pool_end - _ulmk_user_pool_start);
	info->mem_count   = 1;

	/* No CSA pool on C29. */
	info->csa_pool_base = 0;
	info->csa_pool_size = 0;

#if ULMK_CONFIG_ENABLE_SMP
	g_c29_my_cpu = 0u;
#endif

	ulmk_arch_mpu_init();
	ulmk_arch_irq_vectors_init(0u, 0u, (uintptr_t)_ulmk_isr_stack_top);
}

/* ---------------------------------------------------------------------------
 * Printk character output — board provides the UART write
 * ---------------------------------------------------------------------------
 */

__attribute__((weak)) void ulmk_printk_char_out(char c)
{
	(void)c;
}
