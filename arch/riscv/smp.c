/* SPDX-License-Identifier: MIT */
/*
 * RISC-V SMP: hart id, spinlocks, CLINT MSIP IPI, secondary park/release.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/config.h>
#include <ulmk_arch.h>
#include <arch_config.h>

#if ULMK_CONFIG_ENABLE_SMP
#define SMP_MAX_HARTS	ULMK_ARCH_NUM_CPU
#else
#define SMP_MAX_HARTS	1
#endif

/*
 * .data gate — secondaries must not touch BSS-backed release/entry until
 * CPU0 finishes ulmk_kern_start() relocation.  Park spins until this magic.
 */
#define SMP_GATE_WAIT	0x11111111u
#define SMP_GATE_READY	0xC0DEC0DEu

static volatile uint32_t g_smp_gate = SMP_GATE_WAIT;
/*
 * Park stacks live in .data (not BSS) so CPU0's BSS clear cannot trash a
 * secondary hart spinning before ulmk_arch_smp_mark_ready().
 */
/*
 * Must absorb printk + mpu_init + sched_start before the switch to idle.
 * 2048 overflowed into the next hart's park stack / trailing .data (idle
 * TCBs sit immediately after) when three secondaries raced on -smp 4 —
 * corrupted ctx → U-mode INST_FAULT at the trap epilogue (mtval=0x…f2).
 */
#define ULMK_SECONDARY_STACK_SIZE	4096u

uint8_t g_ulmk_secondary_stack[SMP_MAX_HARTS][ULMK_SECONDARY_STACK_SIZE]
	__attribute__((aligned(16))) = { { 1 } };
static volatile uint32_t g_secondary_release[SMP_MAX_HARTS];
static void (*g_secondary_entry[SMP_MAX_HARTS])(void);

void ulmk_arch_smp_mark_ready(void)
{
	__asm__ volatile("fence rw, rw" ::: "memory");
	g_smp_gate = SMP_GATE_READY;
}

uint32_t ulmk_arch_smp_ready_mask(void)
{
	/* RISC-V secondary mark uses online bits in percpu; expose gate only. */
	return (g_smp_gate == SMP_GATE_READY) ? 0x1u : 0u;
}

void ulmk_arch_smp_wait_ready(uint32_t mask)
{
	(void)mask;
}

uint32_t ulmk_arch_cpu_id(void)
{
	uint32_t id;

	__asm__ volatile("csrr %0, mhartid" : "=r"(id));
	return id;
}

void ulmk_arch_spin_lock(ulmk_spinlock_t *lock)
{
#if ULMK_CONFIG_ENABLE_SMP
	uint32_t tmp;
	uint32_t status;

	for (;;) {
		__asm__ volatile(
			"1: lr.w %0, (%2)\n"
			"   bnez %0, 1b\n"
			"   li %1, 1\n"
			"   sc.w %1, %1, (%2)\n"
			"   bnez %1, 1b\n"
			: "=&r"(tmp), "=&r"(status)
			: "r"(&lock->locked)
			: "memory");
		__asm__ volatile("fence rw, rw" ::: "memory");
		return;
	}
#else
	/* UP: irq_save at irqsave wrapper is sufficient — no LR/SC. */
	(void)lock;
#endif
}

void ulmk_arch_spin_unlock(ulmk_spinlock_t *lock)
{
#if ULMK_CONFIG_ENABLE_SMP
	__asm__ volatile("fence rw, rw" ::: "memory");
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

static volatile uint32_t *clint_msip(uint32_t hart)
{
	return (volatile uint32_t *)(uintptr_t)
		(ULMK_ARCH_CLINT_MSIP0 + hart * 4u);
}

volatile uint32_t g_ulmk_ipi_sent;
volatile uint32_t g_ulmk_ipi_taken;

void ulmk_arch_send_ipi(uint32_t cpu_id)
{
#if ULMK_CONFIG_ENABLE_SMP
	if (cpu_id >= (uint32_t)ULMK_ARCH_NUM_CPU)
		return;
	if (cpu_id == ulmk_arch_cpu_id())
		return;
	g_ulmk_ipi_sent++;
	__asm__ volatile("fence rw, rw" ::: "memory");
	*clint_msip(cpu_id) = 1u;
#else
	(void)cpu_id;
#endif
}

void ulmk_arch_ipi_note_enter(void)
{
}

void ulmk_arch_ipi_clear_self(void)
{
#if ULMK_CONFIG_ENABLE_SMP
	uint32_t cpu = ulmk_arch_cpu_id();

	if (cpu < (uint32_t)ULMK_ARCH_NUM_CPU) {
		*clint_msip(cpu) = 0u;
		g_ulmk_ipi_taken++;
	}
#endif
}

void ulmk_arch_ipi_pulse_self(void)
{
#if ULMK_CONFIG_ENABLE_SMP
	uint32_t cpu = ulmk_arch_cpu_id();

	if (cpu < (uint32_t)SMP_MAX_HARTS)
		*clint_msip(cpu) = 1u;
#endif
}

void ulmk_arch_secondary_init(void)
{
	extern void _trap_handler(void);

	__asm__ volatile("csrw mtvec, %0" :: "r"((uint32_t)_trap_handler));
#if ULMK_CONFIG_ENABLE_SMP
	__asm__ volatile("csrs mie, %0" :: "r"(1u << 3));
#endif
	ulmk_arch_mpu_init();
}

void ulmk_arch_secondary_mark_ready(void)
{
}

void ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void))
{
#if ULMK_CONFIG_ENABLE_SMP
	if (cpu_id == 0u || cpu_id >= (uint32_t)SMP_MAX_HARTS || !entry)
		return;

	g_secondary_entry[cpu_id] = entry;
	__asm__ volatile("fence rw, rw" ::: "memory");
	g_secondary_release[cpu_id] = 1u;
	*clint_msip(cpu_id) = 1u;
#else
	(void)cpu_id;
	(void)entry;
#endif
}

/*
 * Park path for harts != 0 — entered from _start.  Waits until
 * ulmk_arch_start_secondary releases the hart, then runs entry on a
 * private stack.
 */
void ulmk_arch_smp_park(void)
{
#if ULMK_CONFIG_ENABLE_SMP
	uint32_t hart = ulmk_arch_cpu_id();
	void (*entry)(void);
	uintptr_t sp;

	if (hart == 0u || hart >= (uint32_t)SMP_MAX_HARTS) {
		for (;;)
			__asm__ volatile("wfi");
	}

	/* Wait for CPU0 .data/.bss bring-up, then for secondary release. */
	while (g_smp_gate != SMP_GATE_READY)
		;
	while (g_secondary_release[hart] == 0u)
		;

	ulmk_arch_ipi_clear_self();
	entry = g_secondary_entry[hart];
	sp = (uintptr_t)&g_ulmk_secondary_stack[hart][0] +
	     sizeof(g_ulmk_secondary_stack[hart]);
	sp &= ~(uintptr_t)15u;

	if (!entry) {
		for (;;)
			;
	}

	__asm__ volatile("mv sp, %0" : : "r"(sp) : "memory");
	entry();

	for (;;)
		;
#else
	for (;;)
		__asm__ volatile("wfi");
#endif
}
