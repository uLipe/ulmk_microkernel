/* SPDX-License-Identifier: MIT */
/*
 * RISC-V trap entry — arch/riscv/trap.S
 */

	/*
	 * Must save t3–t6: GCC keeps lui bases for .bss/.data in temps across
	 * ecall; leaving them live lets the callee (or IRQ C path) clobber them
	 * and userspace reloads as `lw rd, %lo(sym)(zero)` → mtval=lo12.
	 */
	/*
	 * TF_MCAUSE is mandatory on Espressif CLIC: mret restores mintstatus
	 * from mcause.MPIL.  Leaving a stale/clobbered mcause masks further IRQs.
	 */
	.equ TF_SIZE, 148
	.equ TF_RA, 0
	.equ TF_SP, 4
	.equ TF_GP, 8
	.equ TF_TP, 12
	.equ TF_T0, 16
	.equ TF_T1, 20
	.equ TF_T2, 24
	.equ TF_S0, 28
	.equ TF_S1, 32
	.equ TF_A0, 36
	.equ TF_A1, 40
	.equ TF_A2, 44
	.equ TF_A3, 48
	.equ TF_A4, 52
	.equ TF_A5, 56
	.equ TF_A6, 60
	.equ TF_A7, 64
	.equ TF_S2, 68
	.equ TF_S3, 72
	.equ TF_S4, 76
	.equ TF_S5, 80
	.equ TF_S6, 84
	.equ TF_S7, 88
	.equ TF_S8, 92
	.equ TF_S9, 96
	.equ TF_S10, 100
	.equ TF_S11, 104
	.equ TF_MEPC, 108
	.equ TF_MSTATUS, 112
	.equ TF_T3, 116
	.equ TF_T4, 120
	.equ TF_T5, 124
	.equ TF_T6, 128
	.equ TF_MCAUSE, 132

	.extern _ulmk_trap_dispatch
	.extern g_trap_sp

	.section .text.trap, "ax", @progbits
	/*
	 * CLIC mtvec mode 3: exception PC = mtvec[31:6]<<6 — must be
	 * 64-byte aligned.  Direct mode only needs 4-byte align.
	 */
	.balign 64
	.global _trap_handler
	.type _trap_handler, @function
_trap_handler:
	addi    sp, sp, -TF_SIZE
	sw      ra, TF_RA(sp)
	sw      gp, TF_GP(sp)
	sw      tp, TF_TP(sp)
	sw      t0, TF_T0(sp)
	sw      t1, TF_T1(sp)
	sw      t2, TF_T2(sp)
	sw      t3, TF_T3(sp)
	sw      t4, TF_T4(sp)
	sw      t5, TF_T5(sp)
	sw      t6, TF_T6(sp)
	sw      s0, TF_S0(sp)
	sw      s1, TF_S1(sp)
	sw      a0, TF_A0(sp)
	sw      a1, TF_A1(sp)
	sw      a2, TF_A2(sp)
	sw      a3, TF_A3(sp)
	sw      a4, TF_A4(sp)
	sw      a5, TF_A5(sp)
	sw      a6, TF_A6(sp)
	sw      a7, TF_A7(sp)
	sw      s2, TF_S2(sp)
	sw      s3, TF_S3(sp)
	sw      s4, TF_S4(sp)
	sw      s5, TF_S5(sp)
	sw      s6, TF_S6(sp)
	sw      s7, TF_S7(sp)
	sw      s8, TF_S8(sp)
	sw      s9, TF_S9(sp)
	sw      s10, TF_S10(sp)
	sw      s11, TF_S11(sp)
	csrr    t0, mepc
	sw      t0, TF_MEPC(sp)
	csrr    t0, mstatus
	sw      t0, TF_MSTATUS(sp)
	csrr    t0, mcause
	sw      t0, TF_MCAUSE(sp)
	addi    t0, sp, TF_SIZE
	sw      t0, TF_SP(sp)

	csrr    t0, mhartid
	slli    t0, t0, 2
	la      t1, g_trap_sp
	add     t1, t1, t0
	sw      sp, 0(t1)

	mv      a0, sp
	call    _ulmk_trap_dispatch

.Ltrap_restore:
	lw      ra, TF_RA(sp)
	lw      gp, TF_GP(sp)
	lw      tp, TF_TP(sp)
	lw      t0, TF_T0(sp)
	lw      t1, TF_T1(sp)
	lw      t2, TF_T2(sp)
	lw      t3, TF_T3(sp)
	lw      t4, TF_T4(sp)
	lw      t5, TF_T5(sp)
	lw      t6, TF_T6(sp)
	lw      s0, TF_S0(sp)
	lw      s1, TF_S1(sp)
	lw      a0, TF_A0(sp)
	lw      a1, TF_A1(sp)
	lw      a2, TF_A2(sp)
	lw      a3, TF_A3(sp)
	lw      a4, TF_A4(sp)
	lw      a5, TF_A5(sp)
	lw      a6, TF_A6(sp)
	lw      a7, TF_A7(sp)
	lw      s2, TF_S2(sp)
	lw      s3, TF_S3(sp)
	lw      s4, TF_S4(sp)
	lw      s5, TF_S5(sp)
	lw      s6, TF_S6(sp)
	lw      s7, TF_S7(sp)
	lw      s8, TF_S8(sp)
	lw      s9, TF_S9(sp)
	lw      s10, TF_S10(sp)
	lw      s11, TF_S11(sp)
	lw      t0, TF_MEPC(sp)
	csrw    mepc, t0
	lw      t0, TF_MCAUSE(sp)
	csrw    mcause, t0
	lw      t0, TF_MSTATUS(sp)
	/* MIE must stay clear until mret; nested IRQ here corrupts mepc. */
	andi    t0, t0, ~8
	csrw    mstatus, t0
	addi    sp, sp, TF_SIZE
	mret
	.size _trap_handler, . - _trap_handler
