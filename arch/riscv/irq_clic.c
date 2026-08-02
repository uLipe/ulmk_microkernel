/* SPDX-License-Identifier: MIT */
/*
 * CLIC interrupt backend — arch/riscv/irq_clic.c
 *
 * Core-local interrupts via MMIO (CLIC v0.9 layout).  Enabled when
 * ULMK_ARCH_HAVE_CLIC=1 in arch_config.h / board.cmake.
 *
 * Vectored mode (ULMK_ARCH_CLIC_VECTORED): mtvec mode=3 + MTVT table;
 * Espressif INTMTX maps peripheral sources onto CPU IRQ lines 16..47.
 */

#include <stdint.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk_arch.h>
#include <arch_config.h>
#include <ulmk/board.h>
#include "irq_internal.h"

extern uint32_t g_src_addr[256];
extern uint8_t  g_src_type[256];

#if ULMK_ARCH_HAVE_CLIC

extern uint16_t g_src_clic_irq[256];

#define CLIC_INT_REGION_SIZE	0x1000u
#define CLIC_CFG_NLBITS_SHIFT	1u
#define CLIC_ATTR_SHV		(1u << 0)
#define MTVT_CSR		0x307u
#define MTVEC_MODE_CLIC		3u

#if ULMK_ARCH_CLIC_VECTORED
extern uint32_t _ulmk_clic_mtvt[];
extern void _trap_handler(void);
#endif

/* True if the board took the interrupt over entirely (the tick, typically). */
static inline bool board_claimed(uint32_t irq)
{
#if ULMK_CONFIG_BOARD_IRQ_CLAIM
	return ulmk_board_irq_claim(irq);
#else
	(void)irq;
	return false;
#endif
}

static inline volatile uint8_t *clic_intip(uint32_t irq)
{
	return (volatile uint8_t *)(uintptr_t)
		(ULMK_ARCH_CLIC_INT_BASE + irq * 4u);
}

static inline volatile uint8_t *clic_intie(uint32_t irq)
{
	return clic_intip(irq) + 1u;
}

static inline volatile uint8_t *clic_intattr(uint32_t irq)
{
	return clic_intip(irq) + 2u;
}

static inline volatile uint8_t *clic_intctl(uint32_t irq)
{
	return clic_intip(irq) + 3u;
}

static inline bool clic_src_is_int_reg(uint32_t addr, uint32_t *irq_out)
{
	uint32_t off;

	if (addr < ULMK_ARCH_CLIC_INT_BASE)
		return false;
	off = addr - ULMK_ARCH_CLIC_INT_BASE;
	if (off >= CLIC_INT_REGION_SIZE)
		return false;
	*irq_out = off / 4u;
	return true;
}

void riscv_clic_init(void)
{
	volatile uint32_t *cfg =
		(volatile uint32_t *)(uintptr_t)ULMK_BOARD_CLIC_BASE;

#if ULMK_ARCH_CLIC_VECTORED
	/* NLBITS=3 (matches Espressif CLIC). */
	*cfg = (3u << CLIC_CFG_NLBITS_SHIFT);
	__asm__ volatile("csrw %0, %1" :: "i"(MTVT_CSR),
			 "r"((uint32_t)_ulmk_clic_mtvt));
	__asm__ volatile(
		"csrw mtvec, %0"
		:: "r"(((uint32_t)_trap_handler) | MTVEC_MODE_CLIC));
#else
	(void)cfg;
#endif
}

bool riscv_clic_is_binding(uint8_t srpn)
{
	return g_src_type[srpn] == IRQ_SRC_CLIC;
}

void riscv_clic_register(uint8_t srpn, uint32_t src_reg_addr)
{
	uint32_t irq;

	if (!clic_src_is_int_reg(src_reg_addr, &irq))
		return;

	g_src_type[srpn]     = IRQ_SRC_CLIC;
	g_src_clic_irq[srpn] = (uint16_t)irq;
	g_src_addr[srpn]     = src_reg_addr;

#if ULMK_ARCH_CLIC_VECTORED
	/* Level-triggered + SHV; same ctl level for every line (flat IRQ).
	 * Peers do not nest — riscv_clic_dispatch drains IE∧IP in software. */
	*clic_intattr(irq) = 0u;
	*clic_intctl(irq)  = (1u << 5); /* priority level 1 (NLBITS=3) */
#endif
}

void riscv_clic_ack(uint8_t srpn)
{
	uint16_t irq;

	if (g_src_type[srpn] != IRQ_SRC_CLIC)
		return;
	irq = g_src_clic_irq[srpn];
	*clic_intip(irq) = 0u;
}

void riscv_clic_enable(uint8_t srpn)
{
	uint16_t irq;

	if (g_src_type[srpn] != IRQ_SRC_CLIC)
		return;
	irq = g_src_clic_irq[srpn];
	*clic_intie(irq) = 1u;
	/* Do not csrs mstatus.MIE here — enable from syscall/trap would nest. */
}

void riscv_clic_disable(uint8_t srpn)
{
	uint16_t irq;

	if (g_src_type[srpn] != IRQ_SRC_CLIC)
		return;
	irq = g_src_clic_irq[srpn];
	*clic_intie(irq) = 0u;
}

/* riscv_clic_drop_mil lives in trap.S — mintstatus is read-only; needs mret. */

/*
 * Non-nesting CLIC service (flat IRQ level — trap-irq-flat-priority):
 *
 * 1. mstatus.MIE stays clear for the whole entry (trap epilogue).
 * 2. Drain every IE∧IP peer in software: SHV will not nest same-level lines,
 *    and mnxti only selects software-vectored IRQs on incomplete P4 CLIC.
 * 3. drop_mil once, schedule once, mret — further arrivals become new traps.
 */
#define CLIC_DRAIN_MAX		16u
#define CLIC_EXT_IRQ_LO		16u
#define CLIC_EXT_IRQ_HI		48u
#define CLIC_IRQ_NONE		0xFFFFFFFFu

static uint32_t clic_pick_pending(void)
{
	uint32_t irq;
	uint16_t srpn;
	uint16_t bound;

	/*
	 * Ascending external window among same-level peers (flat IRQ model):
	 * prefer the lowest reserved slot (board tick) in the software drain
	 * so the wheel still advances under an IRQ storm — not a HW level bump.
	 */
	for (irq = CLIC_EXT_IRQ_LO; irq < CLIC_EXT_IRQ_HI; irq++) {
		if (*clic_intie(irq) != 0u && (*clic_intip(irq) & 1u) != 0u)
			return irq;
	}

	for (srpn = 1u; srpn < 256u; srpn++) {
		if (g_src_type[srpn] != IRQ_SRC_CLIC)
			continue;
		bound = g_src_clic_irq[srpn];
		if (bound < CLIC_EXT_IRQ_LO || bound >= CLIC_EXT_IRQ_HI) {
			if (*clic_intie(bound) != 0u &&
			    (*clic_intip(bound) & 1u) != 0u)
				return bound;
		}
	}
	return CLIC_IRQ_NONE;
}

static void clic_service_one(uint32_t irq)
{
	uint16_t srpn;
	uint16_t bound;

	if (board_claimed(irq))
		return;

	for (srpn = 1u; srpn < 256u; srpn++) {
		if (g_src_type[srpn] != IRQ_SRC_CLIC)
			continue;
		bound = g_src_clic_irq[srpn];
		if (bound != irq)
			continue;

		ulmk_kern_irq_dispatch((uint8_t)srpn);
		riscv_clic_ack((uint8_t)srpn);
	}
}

void riscv_clic_dispatch(uint32_t mcause)
{
	uint32_t irq;
	unsigned n;

	irq = mcause & 0xFFFu;

	for (n = 0u; n < CLIC_DRAIN_MAX; n++) {
		clic_service_one(irq);
		irq = clic_pick_pending();
		if (irq == CLIC_IRQ_NONE)
			break;
	}

	riscv_clic_drop_mil();
	_arch_generic_isr_handler();
}

#else /* !ULMK_ARCH_HAVE_CLIC */

void riscv_clic_init(void) { }
bool riscv_clic_is_binding(uint8_t srpn) { (void)srpn; return false; }
void riscv_clic_register(uint8_t srpn, uint32_t a) { (void)srpn; (void)a; }
void riscv_clic_enable(uint8_t srpn) { (void)srpn; }
void riscv_clic_disable(uint8_t srpn) { (void)srpn; }
void riscv_clic_ack(uint8_t srpn) { (void)srpn; }
void riscv_clic_dispatch(uint32_t mcause) { (void)mcause; }

#endif /* ULMK_ARCH_HAVE_CLIC */
