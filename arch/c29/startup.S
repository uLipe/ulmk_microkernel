/* SPDX-License-Identifier: MIT */
/*
 * C29 startup — CPU1 prologue only, then ulmk_kern_start.
 * Pattern follows TI device_support codestartbranch.asm (not copied).
 *
 * Stack grows upward: A15 starts at _ulmk_kernel_stack_base.
 */

	.global	code_start
	.global	_start
	.global	main
	.global	reset_vector
	.extern	ulmk_kern_start
	.extern	_ulmk_kernel_stack_base

	.section	codestart, "ax"
code_start:
_start:
main:
	ENTRY1.PROT
	|| ENTRY2.PROT
	MV	D0, 0x68
	ST.W0	@0x30208C52, D0
	/* RTS _c_int00 does this before any C; required under APR after POR. */
	MV	D0, #0xffffffff
	ST.32	@0x3008000C, D0
	MV	A15, #_ulmk_kernel_stack_base
	LB	@ulmk_kern_start

	.section	resetvector, "ax"
reset_vector:
	ISR1.PROT
	|| ISR2.PROT
	MV	D0, 0x68
	ST.W0	@0x30208C52, D0
	MV	A15, #_ulmk_kernel_stack_base
	LB	@ulmk_kern_start
