/* SPDX-License-Identifier: MIT */
/*
 * C29 NMI / fault entry — SSU violations arrive via ESM → NMI.
 * Attempt recoverable kill when a user thread is current; otherwise panic.
 */

	.global	ulmk_arch_nmi_handler
	.global	ulmk_arch_fault_handler
	.extern	ulmk_c29_nmi_dispatch
	.extern	ulmk_kern_trap_panic

	.section .text.ulmk_arch_nmi_handler, "ax"
ulmk_arch_nmi_handler:
	ISR1.PROT
	|| ISR2.PROT
	CALL	@ulmk_c29_nmi_dispatch
.Lhalt_nmi:
	IDLE
	LB	@.Lhalt_nmi

	.section .text.ulmk_arch_fault_handler, "ax"
ulmk_arch_fault_handler:
	CALL	@ulmk_kern_trap_panic
.Lhalt_fault:
	IDLE
	LB	@.Lhalt_fault
