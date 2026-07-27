/* SPDX-License-Identifier: MIT */
/*
 * Secondary CPU2 reset stub — LPA1 @ 0x20108000.
 * Minimal: ISR1.PROT then store handshake into SHARED_RAM / LPA1.
 */

	.global	ulmk_c29_cpu2_start

	.section	.cpu2_codestart, "ax"
ulmk_c29_cpu2_start:
	ISR1.PROT
	|| ISR2.PROT
	MV	D1, #0xC0DE0002
	ST.32	@0x200F8000, D1
	ST.32	@0x20108FF0, D1
1:
	IDLE
	LB	1b

	.section	.cpu2_nmivector, "ax"
	ISR1.PROT
	|| ISR2.PROT
2:
	IDLE
	LB	2b
