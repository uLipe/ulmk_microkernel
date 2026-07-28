/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * TriCore TC1.6.1 / TC2xx architecture constants
 * Full specification: docs/arch_api_spec.md §4
 *
 * SoC addresses and platform options come from ulmk/platform.h, a generated
 * snapshot of boards/<soc>/board_config.h (see tools/gen_config.py).  This
 * file holds ISA invariants only.
 */

#ifndef ULMK_ARCH_TRICORE_CONFIG_H
#define ULMK_ARCH_TRICORE_CONFIG_H

#include <ulmk/platform.h>

#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU	1
#endif

/* No managed L1 D-cache ops on this port yet — dcache syscalls → ENOTSUP. */
#ifndef ULMK_ARCH_HAS_CACHE
#define ULMK_ARCH_HAS_CACHE	0
#endif

#ifndef ULMK_BOARD_SRC_BASE
#error "board_config.h must define ULMK_BOARD_SRC_BASE (SoC SRC block base)"
#endif

/* Kernel tick: STM0 compare → SRPN (board may override). */
#ifndef ULMK_BOARD_IRQ_TICK
#ifdef ULMK_BOARD_IRQ_STM0
#define ULMK_BOARD_IRQ_TICK		ULMK_BOARD_IRQ_STM0
#else
#define ULMK_BOARD_IRQ_TICK		1u
#endif
#endif

#ifndef ULMK_BOARD_TICK_CLOCK_HZ
#ifdef ULMK_BOARD_FSTM_HZ
#define ULMK_BOARD_TICK_CLOCK_HZ	ULMK_BOARD_FSTM_HZ
#else
#define ULMK_BOARD_TICK_CLOCK_HZ	50000000u
#endif
#endif

#ifndef ULMK_BOARD_SRC_SRE_BIT
#error "board_config.h must define ULMK_BOARD_SRC_SRE_BIT"
#endif

#ifndef ULMK_BOARD_FLASH_BASE
#error "board_config.h must define ULMK_BOARD_FLASH_BASE"
#endif

/* =========================================================================
 * MPU hardware limits (TC2xx data sheet, DCON0/DPRE/DPWE registers)
 * ========================================================================= */

#define ULMK_ARCH_NUM_DPR		18	/* data protection ranges (silicon) */
#define ULMK_ARCH_NUM_CPR		10	/* code protection ranges (silicon) */
#define ULMK_ARCH_NUM_PRS		 4	/* protection register sets */

/*
 * Effective DPR/CPR slot counts — from board_config.h unless overridden
 * via -D (e.g. QEMU Linumiz exposes only 4+4 CSFR slots).
 */
#ifndef ULMK_ARCH_MPU_NUM_DPR
#ifdef ULMK_BOARD_MPU_NUM_DPR
#define ULMK_ARCH_MPU_NUM_DPR	ULMK_BOARD_MPU_NUM_DPR
#else
#define ULMK_ARCH_MPU_NUM_DPR	ULMK_ARCH_NUM_DPR
#endif
#endif

#ifndef ULMK_ARCH_MPU_NUM_CPR
#ifdef ULMK_BOARD_MPU_NUM_CPR
#define ULMK_ARCH_MPU_NUM_CPR	ULMK_BOARD_MPU_NUM_CPR
#else
#define ULMK_ARCH_MPU_NUM_CPR	ULMK_ARCH_NUM_CPR
#endif
#endif

#if ULMK_ARCH_MPU_NUM_DPR <= 4
#define ULMK_ARCH_MPU_USER_DPR_BASE	ULMK_ARCH_MPU_NUM_DPR
#else
#define ULMK_ARCH_MPU_USER_DPR_BASE	6
#endif

#define ULMK_ARCH_MAX_REGIONS	12
#define ULMK_ARCH_REGION_ALIGN	64

/*
 * Optional board ISA revision (1.6.1 = TC2xx, 1.6.2 = TC3xx).  Defaults to
 * 1.6.1 when the board does not declare ULMK_BOARD_TRICORE_ISA_*.
 */
#ifndef ULMK_BOARD_TRICORE_ISA_MAJOR
#define ULMK_BOARD_TRICORE_ISA_MAJOR	1
#endif
#ifndef ULMK_BOARD_TRICORE_ISA_MINOR
#define ULMK_BOARD_TRICORE_ISA_MINOR	6
#endif
#ifndef ULMK_BOARD_TRICORE_ISA_PATCH
#define ULMK_BOARD_TRICORE_ISA_PATCH	1
#endif

#define ULMK_ARCH_TRICORE_ISA_161	\
	(ULMK_BOARD_TRICORE_ISA_PATCH == 1)
#define ULMK_ARCH_TRICORE_ISA_162	\
	(ULMK_BOARD_TRICORE_ISA_PATCH == 2)

/* =========================================================================
 * PSW initial values (reference — ulmk_arch_ctx_init overrides at thread create)
 * ========================================================================= */
#define ULMK_ARCH_PSW_USER	0x00000180u
#define ULMK_ARCH_PSW_DRIVER	0x00000580u
#define ULMK_ARCH_PSW_SUPER	0x00000980u

/* =========================================================================
 * CSA pool constraints
 * ========================================================================= */
#define ULMK_ARCH_CSA_FRAME_SIZE	64
#define ULMK_ARCH_CSA_MIN_COUNT	64

/* CSA frames come from the affinity hart's FCX — never fabricate remotely. */
#define ULMK_ARCH_CTX_FABRICATE_ON_AFFINITY_CPU	1

/* =========================================================================
 * CPU idle — board may force NOP idle (QEMU WAIT quirk)
 * ========================================================================= */
#ifndef ULMK_ARCH_IDLE_IS_WAIT
#ifdef ULMK_BOARD_IDLE_IS_WAIT
#define ULMK_ARCH_IDLE_IS_WAIT	ULMK_BOARD_IDLE_IS_WAIT
#else
#define ULMK_ARCH_IDLE_IS_WAIT	1
#endif
#endif

/* =========================================================================
 * MPU CSFR addresses — TC2xx CPU (not SoC memory map)
 * ========================================================================= */

#define ULMK_ARCH_CSFR_DPR_L(n)	(0xC000u + (n) * 8u)
#define ULMK_ARCH_CSFR_DPR_U(n)	(0xC004u + (n) * 8u)
#define ULMK_ARCH_CSFR_CPR_L(n)	(0xD000u + (n) * 8u)
#define ULMK_ARCH_CSFR_CPR_U(n)	(0xD004u + (n) * 8u)

/*
 * TC2xx/TC3xx MPU enables (UM): CPXE @ E000, DPRE @ E010, DPWE @ E020.
 * There is no CPRE register.  Older internal docs listed CPXE @ E040; on
 * TC27x that address is Safety RGNLA — never program it as CPXE.
 */
#define ULMK_ARCH_CSFR_CPXE_0	0xE000u
#define ULMK_ARCH_CSFR_CPXE_1	0xE004u
#define ULMK_ARCH_CSFR_CPXE_2	0xE008u
#define ULMK_ARCH_CSFR_CPXE_3	0xE00Cu
#define ULMK_ARCH_CSFR_DPRE_0	0xE010u
#define ULMK_ARCH_CSFR_DPRE_1	0xE014u
#define ULMK_ARCH_CSFR_DPRE_2	0xE018u
#define ULMK_ARCH_CSFR_DPRE_3	0xE01Cu
#define ULMK_ARCH_CSFR_DPWE_0	0xE020u
#define ULMK_ARCH_CSFR_DPWE_1	0xE024u
#define ULMK_ARCH_CSFR_DPWE_2	0xE028u
#define ULMK_ARCH_CSFR_DPWE_3	0xE02Cu

#define ULMK_ARCH_CSFR_SYSCON	0xFE14u
#define ULMK_ARCH_SYSCON_PROTEN	(1u << 1)

#define ULMK_ARCH_MPU_KERNEL_DPR	0
#define ULMK_ARCH_MPU_KRAM_DPR	1
#define ULMK_ARCH_MPU_URAM_DPR	2
#define ULMK_ARCH_MPU_MMIO_DPR	3
#define ULMK_ARCH_MPU_CPR_KERNEL	0
#define ULMK_ARCH_MPU_CPR_USER	1
#define ULMK_ARCH_PRS_USER	1u

/* =========================================================================
 * Syscall ABI
 * ========================================================================= */
#define ULMK_ARCH_SYSCALL_NR_REG	 "d15"
#define ULMK_ARCH_SYSCALL_ARG0_REG "d4"
#define ULMK_ARCH_SYSCALL_ARG1_REG "d5"
#define ULMK_ARCH_SYSCALL_ARG2_REG "d6"
#define ULMK_ARCH_SYSCALL_ARG3_REG "d7"
#define ULMK_ARCH_SYSCALL_RET_REG  "d2"

/* =========================================================================
 * TriCore CSFR addresses (CPU core — not SoC peripherals)
 * ========================================================================= */
#define ULMK_ARCH_CSFR_BTV	0xFE24u
#define ULMK_ARCH_CSFR_BIV	0xFE20u
#define ULMK_ARCH_CSFR_ISP	0xFE28u
#define ULMK_ARCH_CSFR_ICR	0xFE2Cu
#define ULMK_ARCH_CSFR_FCX	0xFE38u
#define ULMK_ARCH_CSFR_LCX	0xFE3Cu
#define ULMK_ARCH_CSFR_PCXI	0xFE00u
#define ULMK_ARCH_CSFR_PSW	0xFE04u

/* AURIX SRC register layout (family-wide bit positions) */
#define ULMK_ARCH_SRC_TOS_SHIFT	11u
#define ULMK_ARCH_SRC_SRR_BIT	(1u << 24)
#define ULMK_ARCH_SRC_CLRR_BIT	(1u << 25)
#define ULMK_ARCH_SRC_SETR_BIT	(1u << 26)

#endif /* ULMK_ARCH_TRICORE_CONFIG_H */
