/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * TriCore TC1.6.1 trap and interrupt vector tables — arch/tricore/vectors.S
 * Full specification: docs/arch_api_spec.md §8
 *
 * Trap classes 0–7, each 8 words (32 bytes) apart within a 256-byte-aligned
 * base address (BTV).  The hardware indexes: BTV + (class * 0x20) + (TIN * 4)
 * for class 6 (SYSCALL); all others jump through a common handler.
 *
 * Interrupt table: 256-byte-aligned BIV; each entry is a 4-byte jump.
 * We register a single generic ISR dispatcher at vector 0 here; individual
 * SRPN vectors are programmed at runtime by ulmk_arch_irq_src_configure().
 */

    .extern ulmk_arch_trap_entry
    .extern ulmk_arch_trap_mem_fault
    .extern ulmk_arch_syscall_entry
    .extern ulmk_kern_irq_dispatch

/* ---------------------------------------------------------------------------
 * Trap vector table
 * ------------------------------------------------------------------------ */

    .section .trap_table, "ax"
    .align  8   /* 256 bytes */

/* Class 0 — MMU / MPU Data Access Trap */
    .global _trap_class0
_trap_class0:
    mov     %d4, 0
    mov     %d5, %d15
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3000
    andn    %d15, %d15, %d14
    mtcr    0xFE04, %d15
    isync
    call    ulmk_arch_trap_entry
    j       .

/* Class 1 — Internal Protection Trap */
    .align  5
    .global _trap_class1
_trap_class1:
    mov     %d8, %d15
    j       _trap_class1_body

/* Class 2 — Instruction Error Trap */
    .align  5
    .global _trap_class2
_trap_class2:
    mov     %d4, 2
    mov     %d5, %d15
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3000
    andn    %d15, %d15, %d14
    mtcr    0xFE04, %d15
    isync
    call    ulmk_arch_trap_entry
    j       .

/* Class 3 — Context Management Trap (FCU/CDU) */
    .align  5
    .global _trap_class3
_trap_class3:
    mov     %d4, 3
    mov     %d5, %d15
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3000
    andn    %d15, %d15, %d14
    mtcr    0xFE04, %d15
    isync
    call    ulmk_arch_trap_entry
    j       .

/* Class 4 — System Bus and Peripheral Errors */
    .align  5
    .global _trap_class4
_trap_class4:
    mov     %d4, 4
    mov     %d5, %d15
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3000
    andn    %d15, %d15, %d14
    mtcr    0xFE04, %d15
    isync
    call    ulmk_arch_trap_entry
    j       .

/* Class 5 — Assertion Traps */
    .align  5
    .global _trap_class5
_trap_class5:
    mov     %d4, 5
    mov     %d5, %d15
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3000
    andn    %d15, %d15, %d14
    mtcr    0xFE04, %d15
    isync
    call    ulmk_arch_trap_entry
    j       .

/* Class 6 — SYSCALL
 *
 * Hardware leaves user TIN in D15 and args in D4–D7.  A C prologue at
 * -O1+ will clobber those registers before any inline asm can read them,
 * so park the five words on the interrupt/kernel stack and pass the
 * frame pointer as the sole C argument (D4).
 */
    .align  5
    .global _trap_class6
_trap_class6:
    /*
     * Elevate PRS=0 + IO=supervisor before any C/stack work — kernel
     * BSS (TLSF ctrl, TCB pools) is not in PRS 1.  Args stay in D4–D7
     * until parked below; TIN is in D15.
     */
    mfcr    %d14, 0xFE04
    mov     %d13, 0x3C00
    andn    %d14, %d14, %d13
    mov     %d13, 0x800
    or      %d14, %d14, %d13
    mtcr    0xFE04, %d14
    isync
    sub.a   %a10, 20
    st.w    [%a10] 0,  %d15
    st.w    [%a10] 4,  %d4
    st.w    [%a10] 8,  %d5
    st.w    [%a10] 12, %d6
    st.w    [%a10] 16, %d7
    mov.d   %d4, %a10
    call    ulmk_arch_syscall_entry
    lea     %a10, [%a10] 20
    rfe

/* Class 7 — NMI */
    .align  5
    .global _trap_class7
_trap_class7:
    mov     %d4, 7
    mov     %d5, %d15
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3000
    andn    %d15, %d15, %d14
    mtcr    0xFE04, %d15
    isync
    call    ulmk_arch_trap_entry
    j       .

/*
 * Class 1 body (outside the 32-byte slot).  An MPR/MPW from a user thread
 * may only mean a lazy area window is not loaded yet, so the lower context
 * is saved and the faulting instruction resumed with RFE once it is.  TIN
 * rides in D8: upper context, already saved by the trap and restored by RFE.
 * PRS 0 comes before SVLCX (the CSA pool is kernel RAM) and interrupts stay
 * off for the kernel part; RFE restores the thread's ICR.IE.
 */
_trap_class1_body:
    disable
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3C00
    andn    %d15, %d15, %d14
    mov     %d14, 0x800
    or      %d15, %d14
    mtcr    0xFE04, %d15
    isync
    svlcx
    mov     %d4, %d8
    call    ulmk_arch_trap_mem_fault
    jz      %d2, 1f
    rslcx
    rfe
1:
    rslcx
    mov     %d4, 1
    mov     %d5, %d8
    call    ulmk_arch_trap_entry
    j       .

/* ---------------------------------------------------------------------------
 * Interrupt vector table — 32-byte slots, BIV-relative (VSS=0)
 *
 * On interrupt entry the hardware:
 *   1. Saves the Upper Context (PSW, A10, A11, D8–D15) into a fresh CSA frame
 *   2. Sets A10 = ISP, IS=1, IE=0, CCPN=SRPN
 *   3. Jumps to BIV + SRPN * 32
 *
 * Each ISR entry must save the Lower Context (SVLCX), call the C handler,
 * restore the Lower Context (RSLCX), and return (RFE).
 * DISABLE before RSLCX prevents a higher-priority interrupt from nesting
 * during the critical restore sequence.
 * ------------------------------------------------------------------------ */

    .extern _arch_generic_isr_handler
    .section .int_table, "ax"
    .align  8   /* 256-byte aligned BIV base */

    .global _ulmk_int_table
_ulmk_int_table:

/*
 * Every slot, 0 included, is the same generic dispatcher: the kernel reads
 * the SRPN from ICR.CCPN, not from the slot.  The entry address is
 * BIV | (PIPN << 5) -- an OR, not an add -- and BIV is only 256-byte
 * aligned, so with BIV bits 8..12 set a PIPN lands on another slot of this
 * table (on 0x..300, SRPN 8/16/24 land on slot 0).  Identical slots make
 * that harmless without aligning the table to its 8 KB size.
 */
    .rept   256
    j       _arch_generic_preempt_isr
    .align  5
    .endr

/* ---------------------------------------------------------------------------
 * _arch_generic_preempt_isr — generic IRQ handler with preemption support.
 *
 * Jumped to by every SRPN=2..255 vector slot.  The SRPN is read from
 * ICR.CCPN inside _arch_generic_isr_handler, which also calls
 * ulmk_kern_sched_dispatch() to potentially set g_preempt_old/new_ctx.
 *
 * If g_preempt_new_ctx is set on return from the C handler, perform the
 * preemptive context switch identically to _arch_tick_preempt_isr.
 * ------------------------------------------------------------------------ */

    .extern _arch_generic_isr_handler
    .global _arch_generic_preempt_isr
    .type   _arch_generic_preempt_isr, @function

_arch_generic_preempt_isr:
    /*
     * PRS 0 + supervisor IO before svlcx — CSA pool lives in kernel RAM
     * and is unreachable at PRS 1 (userspace protection set).
     */
    mfcr    %d15, 0xFE04
    mov     %d14, 0x3C00
    andn    %d15, %d15, %d14
    mov     %d14, 0x800
    or      %d15, %d14
    mtcr    0xFE04, %d15
    isync
    svlcx
    call    _arch_generic_isr_handler

    /*
     * Per-CPU deferred preempt slot: CORE_ID * sizeof(ptr) into the arrays.
     * Shared g_preempt_* (UP-era) races with remote IPI on CPU1.
     */
    mfcr    %d15, 0xFE1C
    and     %d15, %d15, 7
    sh      %d15, %d15, 2

    movh.a  %a14, hi:g_preempt_new_ctx
    lea     %a14, [%a14] lo:g_preempt_new_ctx
    addsc.a %a14, %a14, %d15, 0
    ld.a    %a14, [%a14]
    jz.a    %a14, .Lgeneric_no_preempt

    movh.a  %a15, hi:g_preempt_old_ctx
    lea     %a15, [%a15] lo:g_preempt_old_ctx
    addsc.a %a15, %a15, %d15, 0
    ld.a    %a15, [%a15]

    mov.a   %a12, 0
    movh.a  %a13, hi:g_preempt_new_ctx
    lea     %a13, [%a13] lo:g_preempt_new_ctx
    addsc.a %a13, %a13, %d15, 0
    st.a    [%a13], %a12

    /*
     * Book path: store from PCXI, load to PCXI, one dsync, mtcr, rslcx, rfe.
     * No C mid-switch (avoids an extra Upper CSA from CALL).
     *
     * disable BEFORE mtcr PCXI — same rule as ulmk_arch_ctx_switch.  A nested
     * tick between mtcr and rslcx would splice ISR frames onto the destination
     * chain and CTYP (class 3 tin 6) on the next RSLCX/RET.
     */
    mfcr    %d0, 0xFE00
    st.w    [%a15] 0, %d0
    ld.w    %d0, [%a14] 0
    disable
    dsync
    mtcr    0xFE00, %d0
    isync

    mfcr    %d1, 0xFE04
    mov     %d2, 0x7f
    andn    %d1, %d1, %d2
    mtcr    0xFE04, %d1
    isync

    rslcx
    rfe

.Lgeneric_no_preempt:
    disable
    rslcx
    rfe

    .size _arch_generic_preempt_isr, . - _arch_generic_preempt_isr
