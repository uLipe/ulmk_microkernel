/* SPDX-License-Identifier: MIT */
/* Internal IRQ layer — arch/riscv/irq_internal.h */

#ifndef ULMK_RISCV_IRQ_INTERNAL_H
#define ULMK_RISCV_IRQ_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <arch_config.h>

#define IRQ_SRC_NONE	0u
#define IRQ_SRC_CLINT	1u
#define IRQ_SRC_PLIC	2u
#define IRQ_SRC_CLIC	3u

#define MCAUSE_INT_BIT	(1u << 31)
#define MCAUSE_MSOFT	(0x80000003u)
#define MCAUSE_MTIMER	(0x80000007u)
#define MCAUSE_MEXT	(0x8000000Bu)

void riscv_irq_init(void);

void riscv_irq_handle_interrupt(uint32_t mcause);

void riscv_irq_src_configure(uint8_t srpn, uint8_t priority, uint8_t cpu_id);
void riscv_irq_src_register(uint8_t srpn, uint32_t src_reg_addr);
void riscv_irq_src_enable(uint8_t srpn);
void riscv_irq_src_disable(uint8_t srpn);
void riscv_irq_src_ack(uint8_t srpn);
bool riscv_irq_src_is_pending(uint8_t srpn);
void riscv_irq_src_trigger(uint8_t srpn);

void _arch_generic_isr_handler(void);

#if ULMK_ARCH_HAVE_CLIC
void riscv_clic_drop_mil(void);
#endif

#endif /* ULMK_RISCV_IRQ_INTERNAL_H */
