/* SPDX-License-Identifier: MIT */
/*
 * C29 IRQ and tick — PIPE programming matches TI driverlib/hw_pipe.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include <ulmk/config.h>
#include <ulmk_arch.h>
#include <ulmk/platform.h>

extern void ulmk_c29_int_veneer(void);
extern void ulmk_c29_tick_isr(void);
extern void ulmk_c29_syscall_isr(void);
extern void ulmk_c29_yield_isr(void);
extern void ulmk_c29_ipi1_isr(void);
extern void ulmk_c29_ipi2_isr(void);

extern void ulmk_kern_irq_dispatch(uint8_t srpn);
extern void ulmk_kern_timer_tick(void);
extern void ulmk_kern_sched_dispatch(bool from_isr);
extern uint32_t ulmk_kern_trap_syscall(uint8_t tin, uint32_t args[4]);
extern uint32_t ulmk_kern_syscall_ret_resolve(uint32_t ret);
#if ULMK_CONFIG_ENABLE_SMP
extern void ulmk_kern_ipi_from_isr(void);
#endif

#define MMIO8(a)	(*(volatile uint8_t *)(uintptr_t)(a))
#define MMIO32(a)	(*(volatile uint32_t *)(uintptr_t)(a))

#define TIMER2_BASE	ULMK_BOARD_CPUTIMER2_BASE
#define TIMER2_TIM	((volatile uint32_t *)(TIMER2_BASE + 0x00u))
#define TIMER2_PRD	((volatile uint32_t *)(TIMER2_BASE + 0x04u))
#define TIMER2_TCR	((volatile uint16_t *)(TIMER2_BASE + 0x08u))

#define TCR_TIE		(1u << 14)
#define TCR_TIF		(1u << 15)
#define TCR_TSS		(1u << 4)
#define TCR_TRB		(1u << 5)
#define TCR_FREE	(1u << 11)
#define TCR_SOFT	(1u << 10)

/* Diagnostic: CPUTIMER2 ISR hit count (Gate B tick bring-up). */
volatile uint32_t g_c29_tick_hits;

/* Nesting depth — also used by arch.c / NMI to defer coop switches to RETI. */
uint32_t g_c29_int_depth;

static void pipe_config_channel(uint8_t srpn, void (*handler)(void),
				uint8_t priority)
{
	MMIO32(ULMK_ARCH_PIPE_INT_VECT(srpn)) =
		(uint32_t)(uintptr_t)handler;
	/* PRI in [7:0]; CONTEXT_ID 0 (same as FreeRTOS default path). */
	MMIO32(ULMK_ARCH_PIPE_INT_CONFIG(srpn)) =
		(uint32_t)priority;
	MMIO32(ULMK_ARCH_PIPE_INT_LINK_OWNER(srpn)) =
		ULMK_ARCH_PIPE_OWNER_LINK2;
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_L(srpn)) = 0u;
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_H(srpn)) =
		(uint8_t)ULMK_ARCH_PIPE_FLAG_CLR;
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_L(srpn)) =
		(uint8_t)ULMK_ARCH_PIPE_CTL_L_EN;
}

uint32_t ulmk_c29_int_dispatch(uint32_t frame_sp, uint32_t srpn)
{
	uint32_t args[4];
	uint32_t *frame;
	uint32_t ret;
	uint8_t tin;
	bool outer;

	frame = (uint32_t *)(uintptr_t)frame_sp;
	outer = (g_c29_int_depth == 0u);
	g_c29_int_depth++;

	if (srpn == ULMK_BOARD_IRQ_TIMER2) {
		*TIMER2_TCR |= (uint16_t)TCR_TIF;
		g_c29_tick_hits++;
		if (outer)
			ulmk_kern_timer_tick();
	} else if (srpn == ULMK_BOARD_IRQ_SWINT_SYSCALL) {
		args[0] = frame[72u / 4u];
		args[1] = frame[76u / 4u];
		args[2] = frame[80u / 4u];
		args[3] = frame[84u / 4u];
		tin = (uint8_t)frame[88u / 4u];
		ret = ulmk_kern_trap_syscall(tin, args);
		ret = ulmk_kern_syscall_ret_resolve(ret);
		frame[72u / 4u] = ret;
		if (outer)
			ulmk_kern_sched_dispatch(false);
	} else if (srpn == ULMK_BOARD_IRQ_SWINT_YIELD) {
		if (outer)
			ulmk_kern_sched_dispatch(false);
#if ULMK_CONFIG_ENABLE_SMP
	} else if (srpn == ULMK_BOARD_IRQ_IPC_1 ||
		   srpn == ULMK_BOARD_IRQ_IPC_2) {
		ulmk_arch_ipi_clear_self();
		if (outer)
			ulmk_kern_ipi_from_isr();
#endif
	} else {
		ulmk_kern_irq_dispatch((uint8_t)srpn);
		if (outer)
			ulmk_kern_sched_dispatch(true);
	}

	ulmk_arch_irq_src_ack((uint8_t)srpn);
	g_c29_int_depth--;
	return 0;
}

void ulmk_arch_irq_vectors_init(uintptr_t btv, uintptr_t biv, uintptr_t isp_top)
{
	uint32_t spins;

	(void)btv;
	(void)biv;
	(void)isp_top;

	/* PIPE MMR + vector RAM init (TI Interrupt_initModule). */
	MMIO32(ULMK_ARCH_PIPE_MMR_CLR) = 0x3u;
	MMIO32(ULMK_ARCH_PIPE_MEM_INIT) =
		ULMK_ARCH_PIPE_MEM_INIT_KEY | 0x3u;
	for (spins = 0u; spins < 1000000u; spins++) {
		if (MMIO32(ULMK_ARCH_PIPE_MEM_INIT_STS) == 0x2u)
			break;
	}

	/* Keep RTINT_THRESHOLD at 0 so priority-255 channels stay INT
	 * (RETI.INT).  Supervisor delivery uses SUP_IGN_INTE_EN instead.
	 */
	MMIO32(ULMK_ARCH_PIPE_RTINT_THRESHOLD) = 0u;
	MMIO32(ULMK_ARCH_PIPE_TASK_CTRL) =
		ULMK_ARCH_PIPE_SUP_IGN_INTE_EN | ULMK_ARCH_PIPE_TASK_CTRL_KEY;
	MMIO8(ULMK_ARCH_PIPE_INTSP) = (uint8_t)ULMK_ARCH_PIPE_INTSP_STACK;

	pipe_config_channel(ULMK_BOARD_IRQ_TIMER2, ulmk_c29_tick_isr,
			    ULMK_ARCH_PIPE_SUP_PRI);
	pipe_config_channel(ULMK_BOARD_IRQ_SWINT_SYSCALL, ulmk_c29_syscall_isr,
			    ULMK_ARCH_PIPE_SUP_PRI);
	pipe_config_channel(ULMK_BOARD_IRQ_SWINT_YIELD, ulmk_c29_yield_isr,
			    ULMK_ARCH_PIPE_SUP_PRI);
#if ULMK_CONFIG_ENABLE_SMP
	pipe_config_channel(ULMK_BOARD_IRQ_IPC_1, ulmk_c29_ipi1_isr,
			    ULMK_ARCH_PIPE_SUP_PRI);
	pipe_config_channel(ULMK_BOARD_IRQ_IPC_2, ulmk_c29_ipi2_isr,
			    ULMK_ARCH_PIPE_SUP_PRI);
#endif

	MMIO32(ULMK_ARCH_PIPE_GLOBAL_EN) =
		ULMK_ARCH_PIPE_GLOBAL_EN_VAL | ULMK_ARCH_PIPE_GLOBAL_EN_KEY;
}

void ulmk_arch_irq_src_configure(uint8_t srpn, uint8_t priority, uint8_t cpu_id)
{
	(void)cpu_id;
	pipe_config_channel(srpn, ulmk_c29_int_veneer, priority);
}

void ulmk_arch_irq_src_register(uint8_t srpn, uint32_t src_reg_addr)
{
	(void)src_reg_addr;
	MMIO32(ULMK_ARCH_PIPE_INT_VECT(srpn)) =
		(uint32_t)(uintptr_t)ulmk_c29_int_veneer;
}

void ulmk_arch_irq_src_enable(uint8_t srpn)
{
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_L(srpn)) =
		(uint8_t)ULMK_ARCH_PIPE_CTL_L_EN;
}

void ulmk_arch_irq_src_disable(uint8_t srpn)
{
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_L(srpn)) = 0u;
}

void ulmk_arch_irq_src_ack(uint8_t srpn)
{
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_H(srpn)) =
		(uint8_t)ULMK_ARCH_PIPE_FLAG_CLR;
}

bool ulmk_arch_irq_src_is_pending(uint8_t srpn)
{
	return (MMIO8(ULMK_ARCH_PIPE_INT_CTL_L(srpn)) & 0x2u) != 0u;
}

void ulmk_arch_irq_src_trigger(uint8_t srpn)
{
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_H(srpn)) =
		(uint8_t)ULMK_ARCH_PIPE_FLAG_FRC;
}

bool ulmk_arch_irq_attach_call(ulmk_irq_attach_fn_t fn, void *data,
			       const ulmk_arch_region_t *regions, uint8_t count)
{
	(void)fn;
	(void)data;
	(void)regions;
	(void)count;
	return false;
}

void ulmk_arch_tick_init(uint32_t tick_hz)
{
	uint32_t period;
	uint16_t tcr;

	if (tick_hz == 0u)
		tick_hz = 1000u;
	period = ULMK_BOARD_TICK_CLOCK_HZ / tick_hz;
	if (period < 2u)
		period = 2u;

	/* Stop, load period, enable IRQ, free-run, reload, start. */
	tcr = *TIMER2_TCR;
	*TIMER2_TCR = (uint16_t)((tcr & (uint16_t)~TCR_TIF) | TCR_TSS);
	*TIMER2_PRD = period;
	*TIMER2_TIM = period;
	tcr = *TIMER2_TCR;
	*TIMER2_TCR = (uint16_t)((tcr & (uint16_t)~TCR_TIF) |
				 TCR_TIE | TCR_FREE);
	tcr = *TIMER2_TCR;
	*TIMER2_TCR = (uint16_t)((tcr & (uint16_t)~TCR_TIF) | TCR_TRB);
	*TIMER2_TCR = (uint16_t)(*TIMER2_TCR & (uint16_t)~TCR_TSS);

	/* Ensure PIPE channel still enabled after timer bring-up. */
	MMIO8(ULMK_ARCH_PIPE_INT_CTL_L(ULMK_BOARD_IRQ_TIMER2)) =
		(uint8_t)ULMK_ARCH_PIPE_CTL_L_EN;
}

void ulmk_arch_tick_ack(void)
{
}

uint32_t ulmk_arch_timer_wheel_cpu(void)
{
	return ulmk_arch_cpu_id();
}

void ulmk_arch_syscall_entry(void)
{
}

void ulmk_arch_trap_entry(uint8_t trap_class, uint8_t tin)
{
	(void)trap_class;
	(void)tin;
	ulmk_kern_trap_panic();
}

void ulmk_arch_trap_dump(uint8_t trap_class, uint8_t tin)
{
	(void)trap_class;
	(void)tin;
}
