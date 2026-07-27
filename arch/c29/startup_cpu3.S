/* SPDX-License-Identifier: MIT */
/*
 * Secondary CPU3 reset stub — CPA0 @ 0x20110000.
 */

	.global	ulmk_c29_cpu3_start

	.section	.cpu3_codestart, "ax"
ulmk_c29_cpu3_start:
	ISR1.PROT
	|| ISR2.PROT
	MV	D1, #0xC0DE0003
	ST.32	@0x200F8004, D1
	ST.32	@0x20110FF0, D1
1:
	IDLE
	LB	1b

	.section	.cpu3_nmivector, "ax"
	ISR1.PROT
	|| ISR2.PROT
2:
	IDLE
	LB	2b
