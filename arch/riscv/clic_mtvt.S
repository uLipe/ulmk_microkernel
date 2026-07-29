/* SPDX-License-Identifier: MIT */
/*
 * CLIC MTVT jump table — arch/riscv/clic_mtvt.S
 *
 * Used when ULMK_ARCH_CLIC_VECTORED=1 (ESP32-P4).  Hardware vectors to
 * MTVT + 4 * interrupt_id; every external line lands in _trap_handler
 * (direct-mode save path).  Exception path still uses mtvec base (mode 3).
 */

	.extern _trap_handler

	.section .text.clic_mtvt, "ax", @progbits
	.option push
	.option norvc
	/*
	 * CLIC MTVT alignment: at least 64 B; with 48 IRQs the ignored
	 * low bits of mtvt effectively require 256 B (4 * 64).
	 */
	.balign 256
	.global _ulmk_clic_mtvt
	.type _ulmk_clic_mtvt, @object
_ulmk_clic_mtvt:
	.rept 48
	.word _trap_handler
	.endr
	.size _ulmk_clic_mtvt, . - _ulmk_clic_mtvt
	.option pop
