/* SPDX-License-Identifier: MIT */
/*
 * C29 context switch and INT veneer.
 *
 * Stack grows upward. Software INT frame (264 bytes):
 *   +0    A14
 *   +4    RPC
 *   +8    DSTS
 *   +12   ESTS
 *   +16   XA0..XA12 (A0-A13)   56 bytes
 *   +72   XD0..XD14 (D0-D15)   64 bytes
 *   +136  XM0..XM30 (M0-M31)  128 bytes
 * Total = 264. Inspired by TI FreeRTOS C29 frame; not copied.
 */

	.global	ulmk_arch_ctx_switch
	.global	ulmk_c29_int_veneer
	.global	ulmk_c29_tick_isr
	.global	ulmk_c29_syscall_isr
	.global	ulmk_c29_yield_isr
	.global	ulmk_c29_ipi1_isr
	.global	ulmk_c29_ipi2_isr
	.global	ulmk_arch_idle_entry
	.global	ulmk_c29_thread_trampoline
	.global	g_c29_preempt_old_ctx
	.global	g_c29_preempt_new_ctx
	.extern	ulmk_c29_int_dispatch
	.extern	ulmk_c29_run_thread

	.equ	CTX_SIZE, 264

	.macro	ULMK_SAVE_CONTEXT
	ST.32	*(A15++#8), A14
	|| MV	A14, RPC
	ST.32	*(A15-#4), A14
	ST.32	*(A15++#8), DSTS
	ST.32	*(A15-#4), ESTS
	ST.64	*(A15++#8), XA0
	ST.64	*(A15++#8), XA2
	ST.64	*(A15++#8), XA4
	ST.64	*(A15++#8), XA6
	ST.64	*(A15++#8), XA8
	ST.64	*(A15++#8), XA10
	ST.64	*(A15++#8), XA12
	ST.64	*(A15++#8), XD0
	ST.64	*(A15++#8), XD2
	ST.64	*(A15++#8), XD4
	ST.64	*(A15++#8), XD6
	ST.64	*(A15++#8), XD8
	ST.64	*(A15++#8), XD10
	ST.64	*(A15++#8), XD12
	ST.64	*(A15++#8), XD14
	ST.64	*(A15++#8), XM0
	ST.64	*(A15++#8), XM2
	ST.64	*(A15++#8), XM4
	ST.64	*(A15++#8), XM6
	ST.64	*(A15++#8), XM8
	ST.64	*(A15++#8), XM10
	ST.64	*(A15++#8), XM12
	ST.64	*(A15++#8), XM14
	ST.64	*(A15++#8), XM16
	ST.64	*(A15++#8), XM18
	ST.64	*(A15++#8), XM20
	ST.64	*(A15++#8), XM22
	ST.64	*(A15++#8), XM24
	ST.64	*(A15++#8), XM26
	ST.64	*(A15++#8), XM28
	ST.64	*(A15++#8), XM30
	.endm

	.macro	ULMK_RESTORE_CONTEXT
	LD.64	XM30, *(A15-=#8)
	LD.64	XM28, *(A15-=#8)
	LD.64	XM26, *(A15-=#8)
	LD.64	XM24, *(A15-=#8)
	LD.64	XM22, *(A15-=#8)
	LD.64	XM20, *(A15-=#8)
	LD.64	XM18, *(A15-=#8)
	LD.64	XM16, *(A15-=#8)
	LD.64	XM14, *(A15-=#8)
	LD.64	XM12, *(A15-=#8)
	LD.64	XM10, *(A15-=#8)
	LD.64	XM8, *(A15-=#8)
	LD.64	XM6, *(A15-=#8)
	LD.64	XM4, *(A15-=#8)
	LD.64	XM2, *(A15-=#8)
	LD.64	XM0, *(A15-=#8)
	LD.64	XD14, *(A15-=#8)
	LD.64	XD12, *(A15-=#8)
	LD.64	XD10, *(A15-=#8)
	LD.64	XD8, *(A15-=#8)
	LD.64	XD6, *(A15-=#8)
	LD.64	XD4, *(A15-=#8)
	LD.64	XD2, *(A15-=#8)
	LD.64	XD0, *(A15-=#8)
	LD.64	XA12, *(A15-=#8)
	LD.64	XA10, *(A15-=#8)
	LD.64	XA8, *(A15-=#8)
	LD.64	XA6, *(A15-=#8)
	LD.64	XA4, *(A15-=#8)
	LD.64	XA2, *(A15-=#8)
	LD.64	XA0, *(A15-=#8)
	LD.32	ESTS, *(A15-#4)
	LD.32	DSTS, *(A15-=#8)
	LD.32	RPC, *(A15-#4)
	LD.32	A14, *(A15-=#8)
	.endm

	.section .text.ulmk_arch_ctx_switch, "ax"
ulmk_arch_ctx_switch:
	BCMPZ	.Lfirst_launch, A.EQ, A4
	ULMK_SAVE_CONTEXT
	ST.32	*A4, A15

.Lfirst_launch:
	LD.32	A15, *A5
	ULMK_RESTORE_CONTEXT
	RET

	.section .text.ulmk_c29_int_veneer, "ax"
ulmk_c29_int_veneer:
	ULMK_SAVE_CONTEXT
	/* ABI: uint32 args in D0/D1 — frame_sp, srpn. */
	MV	D1, D0
	MV	D0, A15
	MV	D2, #CTX_SIZE
	SUB	D0, D0, D2
	CALL	@ulmk_c29_int_dispatch

	LD.32	A4, @g_c29_preempt_new_ctx
	BCMPZ	.Lveneer_restore, A.EQ, A4
	LD.32	A5, @g_c29_preempt_old_ctx
	BCMPZ	.Lload_new, A.EQ, A5
	ST.32	*A5, A15
.Lload_new:
	LD.32	A15, *A4
	MV	A4, #0
	ST.32	@g_c29_preempt_new_ctx, A4
	ST.32	@g_c29_preempt_old_ctx, A4

.Lveneer_restore:
	ULMK_RESTORE_CONTEXT
	RETI.INT

	.section .text.ulmk_c29_tick_isr, "ax"
ulmk_c29_tick_isr:
	MV	D0, #8
	LB	@ulmk_c29_int_veneer

	.section .text.ulmk_c29_syscall_isr, "ax"
ulmk_c29_syscall_isr:
	MV	D0, #254
	LB	@ulmk_c29_int_veneer

	.section .text.ulmk_c29_yield_isr, "ax"
ulmk_c29_yield_isr:
	MV	D0, #255
	LB	@ulmk_c29_int_veneer

	.section .text.ulmk_c29_ipi1_isr, "ax"
ulmk_c29_ipi1_isr:
	MV	D0, #12
	LB	@ulmk_c29_int_veneer

	.section .text.ulmk_c29_ipi2_isr, "ax"
ulmk_c29_ipi2_isr:
	MV	D0, #16
	LB	@ulmk_c29_int_veneer

	.section .text.ulmk_arch_idle_entry, "ax"
ulmk_arch_idle_entry:
	ENINT
.Lidle:
	IDLE
	LB	@.Lidle

	.section .text.ulmk_c29_thread_trampoline, "ax"
ulmk_c29_thread_trampoline:
	/* A4=arg, A5=entry already restored from the fabricated frame. */
	CALL	@ulmk_c29_run_thread
.Ltramp_halt:
	IDLE
	LB	@.Ltramp_halt

	.section .data, "aw"
	.align	4
g_c29_preempt_old_ctx:
	.long	0
g_c29_preempt_new_ctx:
	.long	0
