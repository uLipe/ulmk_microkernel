/* SPDX-License-Identifier: MIT */
/*
 * ARM Cortex-M architecture constants — arch/arm/include/arch_config.h
 *
 * Covers ARMv7-M (Cortex-M4/M7) and ARMv8-M mainline (Cortex-M33).  The System
 * Control Space (SCS) block at 0xE000E000 — SysTick, NVIC, SCB, MPU, FPU — is
 * architectural and identical across all Cortex-M cores, so its register
 * addresses live here.  SoC-specific bases (UART, board timer) come from
 * ulmk/platform.h (a snapshot of boards/<board>/board_config.h).
 *
 * The sub-profile is selected by the board via ULMK_ARCH_ARMV8M:
 *   0 = ARMv7-M (PMSAv7 MPU: RBAR/RASR, power-of-2 regions)
 *   1 = ARMv8-M (PMSAv8 MPU: RBAR/RLAR base+limit, MAIR attributes)
 */

#ifndef ULMK_ARCH_ARM_CONFIG_H
#define ULMK_ARCH_ARM_CONFIG_H

#include <ulmk/platform.h>

#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU	1
#endif

/* Sub-profile: 0 = ARMv7-M, 1 = ARMv8-M mainline (board overrides). */
#ifndef ULMK_ARCH_ARMV8M
#define ULMK_ARCH_ARMV8M	0
#endif

/*
 * FPU: Cortex-M4F/M7/M33 ship a single-precision FPU that must be enabled in
 * the SCB CPACR at boot (default on; board may force off with 0).  When on,
 * the callee-saved FP registers s16-s31 are saved/restored on context switch.
 */
#ifndef ULMK_ARCH_HAVE_FPU
#define ULMK_ARCH_HAVE_FPU	1
#endif

/*
 * L1 I/D-cache (Cortex-M7).  Off by default — board must set
 * ULMK_BOARD_ENABLE_CPU_CACHE=1 (see board_config.h).  When on, SHARED/Normal
 * regions become cacheable; drivers must clean/invalidate around DMA/LTDC.
 * Kernel syscalls return ULMK_ENOTSUP when this is 0.
 */
#ifndef ULMK_ARCH_HAS_CACHE
#ifdef ULMK_BOARD_ENABLE_CPU_CACHE
#define ULMK_ARCH_HAS_CACHE	ULMK_BOARD_ENABLE_CPU_CACHE
#else
#define ULMK_ARCH_HAS_CACHE	0
#endif
#endif

/* Idle: WFI suspends the core until the next interrupt (default on). */
#ifndef ULMK_ARCH_IDLE_IS_WFI
#define ULMK_ARCH_IDLE_IS_WFI	1
#endif

/*
 * MPU region budget.  Cortex-M implementations provide 8 or 16 regions; QEMU
 * mps2 cores expose 8.  The first slots are static (kernel/user/mmio), the rest
 * dynamic per-thread.
 */
#ifndef ULMK_ARCH_MPU_REGIONS
#define ULMK_ARCH_MPU_REGIONS	8
#endif

#define ULMK_ARCH_MAX_REGIONS	12
#define ULMK_ARCH_REGION_ALIGN	32	/* MPU granule: 32 bytes minimum */

/* Static MPU region assignment (dynamic per-thread regions follow). */
#define ULMK_ARCH_MPU_KTEXT	0	/* kernel code (RX, priv only)     */
#define ULMK_ARCH_MPU_KRAM	1	/* kernel data (RW, priv only)     */
#define ULMK_ARCH_MPU_UTEXT	2	/* user code (RX)                  */
#define ULMK_ARCH_MPU_URAM	3	/* user pool (RW)                  */
#define ULMK_ARCH_MPU_MMIO	4	/* peripherals + flash read        */
#define ULMK_ARCH_MPU_USER_BASE	5	/* first dynamic per-thread region */

/* Kept for API symmetry with the other ports (no hardware PRS on Cortex-M). */
#define ULMK_ARCH_PRS_KERNEL	0u
#define ULMK_ARCH_PRS_USER	1u

/* Number of external NVIC lines wired in the vector table (arch/arm/vectors.S). */
#define ULMK_ARCH_NUM_IRQ	96u

/*
 * Board→kernel IRQ binding: a board maps a kernel SRPN to an NVIC line by
 * passing ULMK_ARCH_NVIC_SRC(irq) as the src_reg_addr of ulmk_irq_bind_hw().
 * The tag bit lets ulmk_arch_irq_src_register() recover the line number.
 */
#define ULMK_ARCH_NVIC_SRC_TAG	0x8000u
#define ULMK_ARCH_NVIC_SRC(irq)	(ULMK_ARCH_NVIC_SRC_TAG | ((irq) & 0x7FFFu))

/*
 * Per-thread kernel stack.  Exceptions always run on MSP, so a synchronous
 * coroutine context switch (kernel/ipc/ep.c) needs each thread to own a private
 * handler stack; ulmk_arch_ctx_init carves it from the top of the thread stack.
 * Must hold the deepest syscall C chain plus one nested fault frame.
 */
#ifndef ULMK_ARCH_KSTACK_SIZE
#define ULMK_ARCH_KSTACK_SIZE	768u
#endif

/* Software context frame: callee-saved core regs saved on switch. */
#define ULMK_ARCH_SW_CORE_REGS	8u	/* r4-r11 */
#if ULMK_ARCH_HAVE_FPU
#define ULMK_ARCH_SW_FP_REGS	16u	/* s16-s31 */
#else
#define ULMK_ARCH_SW_FP_REGS	0u
#endif

/* =========================================================================
 * System Control Space (SCS) — architectural, common to all Cortex-M.
 * ========================================================================= */

#define ULMK_ARCH_SCS_BASE	0xE000E000u

/* SysTick */
#define ULMK_ARCH_SYSTICK_CTRL	0xE000E010u
#define ULMK_ARCH_SYSTICK_LOAD	0xE000E014u
#define ULMK_ARCH_SYSTICK_VAL	0xE000E018u
#define ULMK_ARCH_SYSTICK_CALIB	0xE000E01Cu

#define ULMK_ARCH_SYSTICK_ENABLE	(1u << 0)
#define ULMK_ARCH_SYSTICK_TICKINT	(1u << 1)
#define ULMK_ARCH_SYSTICK_CLKSOURCE	(1u << 2)
#define ULMK_ARCH_SYSTICK_COUNTFLAG	(1u << 16)

/* NVIC */
#define ULMK_ARCH_NVIC_ISER	0xE000E100u
#define ULMK_ARCH_NVIC_ICER	0xE000E180u
#define ULMK_ARCH_NVIC_ISPR	0xE000E200u
#define ULMK_ARCH_NVIC_ICPR	0xE000E280u
#define ULMK_ARCH_NVIC_IPR	0xE000E400u

/* System Control Block (SCB) */
#define ULMK_ARCH_SCB_CPUID	0xE000ED00u
#define ULMK_ARCH_SCB_ICSR	0xE000ED04u
#define ULMK_ARCH_SCB_VTOR	0xE000ED08u
#define ULMK_ARCH_SCB_AIRCR	0xE000ED0Cu
#define ULMK_ARCH_SCB_SCR	0xE000ED10u
#define ULMK_ARCH_SCB_CCR	0xE000ED14u
#define ULMK_ARCH_SCB_SHPR1	0xE000ED18u
#define ULMK_ARCH_SCB_SHPR2	0xE000ED1Cu
#define ULMK_ARCH_SCB_SHPR3	0xE000ED20u
#define ULMK_ARCH_SCB_SHCSR	0xE000ED24u
#define ULMK_ARCH_SCB_CFSR	0xE000ED28u
#define ULMK_ARCH_SCB_HFSR	0xE000ED2Cu
#define ULMK_ARCH_SCB_MMFAR	0xE000ED34u
#define ULMK_ARCH_SCB_BFAR	0xE000ED38u
#define ULMK_ARCH_SCB_CPACR	0xE000ED88u

#define ULMK_ARCH_SHCSR_MEMFAULTENA	(1u << 16)
#define ULMK_ARCH_SHCSR_BUSFAULTENA	(1u << 17)
#define ULMK_ARCH_SHCSR_USGFAULTENA	(1u << 18)

/* MPU (PMSAv7 / PMSAv8 register file) */
#define ULMK_ARCH_MPU_TYPE	0xE000ED90u
#define ULMK_ARCH_MPU_CTRL	0xE000ED94u
#define ULMK_ARCH_MPU_RNR	0xE000ED98u
#define ULMK_ARCH_MPU_RBAR	0xE000ED9Cu
#define ULMK_ARCH_MPU_RASR	0xE000EDA0u	/* v7-M attr/size register */
#define ULMK_ARCH_MPU_RLAR	0xE000EDA0u	/* v8-M limit register     */
#define ULMK_ARCH_MPU_MAIR0	0xE000EDC0u	/* v8-M memory attributes  */
#define ULMK_ARCH_MPU_MAIR1	0xE000EDC4u

#define ULMK_ARCH_MPU_CTRL_ENABLE	(1u << 0)
#define ULMK_ARCH_MPU_CTRL_HFNMIENA	(1u << 1)
#define ULMK_ARCH_MPU_CTRL_PRIVDEFENA	(1u << 2)

/* FPU control */
#define ULMK_ARCH_FPU_FPCCR	0xE000EF34u
#define ULMK_ARCH_FPU_FPCAR	0xE000EF38u
#define ULMK_ARCH_FPU_FPDSCR	0xE000EF3Cu

#define ULMK_ARCH_CPACR_CP10_CP11_FULL	(0xFu << 20)
#define ULMK_ARCH_FPCCR_ASPEN		(1u << 31)
#define ULMK_ARCH_FPCCR_LSPEN		(1u << 30)

/* EXC_RETURN values (Thread mode). */
#define ULMK_ARCH_EXC_RETURN_PSP	0xFFFFFFFDu	/* no FP frame       */
#define ULMK_ARCH_EXC_RETURN_PSP_FP	0xFFFFFFEDu	/* extended FP frame */

/*
 * Reserved SVC immediate that requests the very first thread launch.  The first
 * thread must enter unprivileged Thread mode via an exception return, but
 * ulmk_sched_start() runs in privileged Thread mode; it raises this SVC so the
 * launch happens from Handler mode.  Outside the syscall number range
 * (ULMK_SYS_MAX = 128) so it never collides with a real syscall.
 */
#define ULMK_ARCH_SVC_LAUNCH	0xFFu

/* CONTROL register bits. */
#define ULMK_ARCH_CONTROL_NPRIV	(1u << 0)	/* 1 = unprivileged thread */
#define ULMK_ARCH_CONTROL_SPSEL	(1u << 1)	/* 1 = thread uses PSP     */
#define ULMK_ARCH_CONTROL_FPCA	(1u << 2)	/* FP context active       */

#endif /* ULMK_ARCH_ARM_CONFIG_H */
