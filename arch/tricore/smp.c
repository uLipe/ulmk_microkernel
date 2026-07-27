/* SPDX-License-Identifier: MIT */
/*
 * TriCore SMP — hart id helpers live in arch.c; IPI + secondary bring-up here.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/config.h>
#include <ulmk_arch.h>
#include <arch_config.h>
#include <board_config.h>

#if defined(ULMK_BOARD_CPU1_PC) || defined(ULMK_BOARD_CPU2_PC)
#include <board_smp.h>
#endif

#if ULMK_CONFIG_ENABLE_SMP

#define SMP_GATE_WAIT	0x11111111u
#define SMP_GATE_READY	0xC0DEC0DEu

static volatile uint32_t g_smp_gate = SMP_GATE_WAIT;
static volatile uint32_t g_secondary_release[ULMK_ARCH_NUM_CPU];
static volatile uint32_t g_secondary_armed[ULMK_ARCH_NUM_CPU];
static void (*g_secondary_entry[ULMK_ARCH_NUM_CPU])(void);

extern char _trap_class0[];
extern char _ulmk_int_table[];
extern char _ulmk_isr_stack_cpu1_top[];
extern void _ulmk_cpu1_start(void);
#if defined(ULMK_BOARD_CPU2_PC)
extern char _ulmk_isr_stack_cpu2_top[];
extern void _ulmk_cpu2_start(void);
#endif

/*
 * TC27x SRC.TOS (iLLD IfxSrc_Tos): 0=CPU0, 1=CPU1, 2=CPU2, 3=DMA.
 * Mapping CPU2→3 sends GPSR/STM IRQs to DMA — CPU2 never vectors.
 */
static uint32_t cpu_to_tos(uint32_t cpu)
{
	if (cpu > 2u)
		return 0u;
	return cpu;
}

static volatile uint32_t *ipi_src(uint32_t cpu)
{
	if (cpu == 0u)
		return (volatile uint32_t *)(uintptr_t)ULMK_BOARD_SRC_GPSR00;
#if defined(ULMK_BOARD_SRC_GPSR10)
	if (cpu == 1u)
		return (volatile uint32_t *)(uintptr_t)ULMK_BOARD_SRC_GPSR10;
#endif
#if defined(ULMK_BOARD_SRC_GPSR20)
	if (cpu == 2u)
		return (volatile uint32_t *)(uintptr_t)ULMK_BOARD_SRC_GPSR20;
#endif
	return NULL;
}

#if defined(ULMK_BOARD_INT_SRB0)
static volatile uint32_t *ipi_srb(uint32_t cpu)
{
	if (cpu == 0u)
		return (volatile uint32_t *)(uintptr_t)ULMK_BOARD_INT_SRB0;
#if defined(ULMK_BOARD_INT_SRB1)
	if (cpu == 1u)
		return (volatile uint32_t *)(uintptr_t)ULMK_BOARD_INT_SRB1;
#endif
#if defined(ULMK_BOARD_INT_SRB2)
	if (cpu == 2u)
		return (volatile uint32_t *)(uintptr_t)ULMK_BOARD_INT_SRB2;
#endif
	return NULL;
}
#endif

/*
 * RMW helpers — match iLLD IfxSrc_*: never absolute-write the SRC word.
 * Absolute writes zero ECC[21:16]; ICU ECC check then faults the request
 * (TC27x UM §16.7 — ECC over SRPN/TOS/SRE/SRN index).
 */
static void ipi_src_set_bits(volatile uint32_t *src, uint32_t bits)
{
	*src = *src | bits;
}

static void ipi_trigger(uint32_t cpu, volatile uint32_t *src)
{
	/*
	 * TC27x UM §16.5: GPSRxy triggers ONLY via SRC.SETR or SRBx[y].
	 * Pulse both — SETR is the iLLD path; SRB is the documented
	 * software-broadcast path for the same GPSR group SR0 (TRIG0).
	 */
	ipi_src_set_bits(src, ULMK_ARCH_SRC_SETR_BIT);
#if defined(ULMK_BOARD_INT_SRB0)
	{
		volatile uint32_t *srb = ipi_srb(cpu);

		if (srb)
			*srb = 1u;	/* TRIG0 → GPSRx SR0 */
	}
#else
	(void)cpu;
#endif
}

void ulmk_arch_smp_mark_ready(void)
{
	__asm__ volatile("dsync" ::: "memory");
	g_smp_gate = SMP_GATE_READY;
}

uint32_t ulmk_arch_smp_ready_mask(void)
{
	return (g_smp_gate == SMP_GATE_READY) ? 0x1u : 0u;
}

void ulmk_arch_smp_wait_ready(uint32_t mask)
{
	(void)mask;
}

void ulmk_arch_send_ipi(uint32_t cpu_id)
{
	volatile uint32_t *src;

	if (cpu_id >= (uint32_t)ULMK_ARCH_NUM_CPU)
		return;
	if (cpu_id == ulmk_arch_cpu_id())
		return;
	src = ipi_src(cpu_id);
	if (!src)
		return;
	__asm__ volatile("dsync" ::: "memory");
	ipi_trigger(cpu_id, src);
}

void ulmk_arch_ipi_pulse_self(void)
{
	uint32_t           cpu = ulmk_arch_cpu_id();
	volatile uint32_t *src = ipi_src(cpu);

	if (!src)
		return;
	ipi_trigger(cpu, src);
}

void ulmk_arch_ipi_clear_self(void)
{
	uint32_t           cpu = ulmk_arch_cpu_id();
	volatile uint32_t *src = ipi_src(cpu);

	if (src)
		ipi_src_set_bits(src, ULMK_ARCH_SRC_CLRR_BIT);
}

void ulmk_arch_ipi_note_enter(void)
{
}

/*
 * Two-step bring-up matching iLLD IfxSrc_init + IfxSrc_enable:
 *   1) SRPN + TOS + CLRR (SRE still 0), preserve ECC
 *   2) SRE = 1
 * Avoids SETR+SRE race on first arm (sticky PIPN).
 */
static void ipi_src_arm(uint32_t cpu)
{
	volatile uint32_t *src = ipi_src(cpu);
	uint32_t           v;
	uint32_t           tos;

	if (!src)
		return;
	tos = cpu_to_tos(cpu);
	v = *src;
	v &= ~0x1FFFu;	/* SRPN[7:0] + SRE + TOS[12:11] */
	v |= (uint32_t)ULMK_BOARD_IRQ_IPI;
	v |= tos << ULMK_ARCH_SRC_TOS_SHIFT;
	*src = v | ULMK_ARCH_SRC_CLRR_BIT;
	ipi_src_set_bits(src, 1u << ULMK_BOARD_SRC_SRE_BIT);
}

void ulmk_arch_secondary_init(void)
{
	uint32_t  cpu = ulmk_arch_cpu_id();
	uintptr_t isp;

	if (cpu == 1u) {
		isp = (uintptr_t)_ulmk_isr_stack_cpu1_top;
#if defined(ULMK_BOARD_CPU2_PC)
	} else if (cpu == 2u) {
		isp = (uintptr_t)_ulmk_isr_stack_cpu2_top;
#endif
	} else {
		for (;;)
			;
	}

	ulmk_arch_irq_vectors_init(
		(uintptr_t)_trap_class0,
		(uintptr_t)_ulmk_int_table,
		isp);
	ulmk_arch_mpu_init();
	ulmk_arch_mpu_enable();
	/*
	 * Arm receive GPSR before marking ready — CPU0 waits on
	 * g_secondary_armed so the first remote enqueue IPI is not lost.
	 * Two-step arm (CLRR then SRE) per Infineon SRC programming rules.
	 */
	ipi_src_arm(cpu);
}

void ulmk_arch_secondary_mark_ready(void)
{
	uint32_t cpu = ulmk_arch_cpu_id();

	__asm__ volatile("dsync" ::: "memory");
	g_secondary_armed[cpu] = 1u;
}

void ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void))
{
	if (cpu_id == 0u || cpu_id >= (uint32_t)ULMK_ARCH_NUM_CPU || !entry)
		return;

	ipi_src_arm(0u);
	ipi_src_arm(cpu_id);

	g_secondary_entry[cpu_id] = entry;
	g_secondary_armed[cpu_id] = 0u;
	__asm__ volatile("dsync" ::: "memory");
	g_secondary_release[cpu_id] = 1u;

#if defined(ULMK_BOARD_CPU1_PC)
	if (cpu_id == 1u)
		ulmk_board_cpu_start(1u, _ulmk_cpu1_start);
#endif
#if defined(ULMK_BOARD_CPU2_PC)
	if (cpu_id == 2u)
		ulmk_board_cpu_start(2u, _ulmk_cpu2_start);
#endif
#if !defined(ULMK_BOARD_CPU1_PC) && !defined(ULMK_BOARD_CPU2_PC)
	(void)cpu_id;
#endif

	/*
	 * Block until the secondary has BIV/ISP/MPU and its GPSR armed.
	 * Prevents CPU0 from enqueueing remote threads before IPIs can land.
	 */
	while (g_secondary_armed[cpu_id] == 0u)
		;
}

void ulmk_arch_smp_park(void)
{
	uint32_t hart = ulmk_arch_cpu_id();
	void (*entry)(void);

	if (hart == 0u || hart >= (uint32_t)ULMK_ARCH_NUM_CPU) {
		for (;;)
			;
	}

#if defined(ULMK_BOARD_CPU1_PC) || defined(ULMK_BOARD_CPU2_PC)
	/* Own WDT — cannot be unlocked safely from CPU0. */
	ulmk_board_cpu_wdt_disable_self();
#endif

	while (g_smp_gate != SMP_GATE_READY)
		;
	while (g_secondary_release[hart] == 0u)
		;

	entry = g_secondary_entry[hart];
	if (!entry) {
		for (;;)
			;
	}

	entry();
	for (;;)
		;
}

#else /* !ENABLE_SMP */

void ulmk_arch_smp_mark_ready(void) {}
uint32_t ulmk_arch_smp_ready_mask(void) { return 0u; }
void ulmk_arch_smp_wait_ready(uint32_t mask) { (void)mask; }
void ulmk_arch_send_ipi(uint32_t cpu_id) { (void)cpu_id; }
void ulmk_arch_ipi_clear_self(void) {}
void ulmk_arch_ipi_note_enter(void) {}
void ulmk_arch_ipi_pulse_self(void) {}
void ulmk_arch_secondary_init(void) {}
void ulmk_arch_secondary_mark_ready(void) {}
void ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void))
{
	(void)cpu_id;
	(void)entry;
}
void ulmk_arch_smp_park(void)
{
	for (;;)
		;
}

#endif /* ULMK_CONFIG_ENABLE_SMP */
