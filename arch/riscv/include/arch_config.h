/* SPDX-License-Identifier: MIT */
/*
 * RISC-V architecture constants — arch/riscv/include/arch_config.h
 *
 * SoC bases (CLINT/PLIC/CLIC) come from ulmk/platform.h, a generated snapshot
 * of boards/<soc>/board_config.h (see tools/gen_config.py).  This file holds
 * ISA invariants and standard interrupt-controller offsets.
 */

#ifndef ULMK_ARCH_RISCV_CONFIG_H
#define ULMK_ARCH_RISCV_CONFIG_H

#include <ulmk/platform.h>

#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU	1
#endif

#ifndef ULMK_ARCH_HAVE_FPU
#define ULMK_ARCH_HAVE_FPU	0
#endif

/*
 * D-cache maintenance is SoC-specific on RISC-V (no standard CSR recipe),
 * so the board opts in and supplies the ulmk_board_dcache_* primitives.
 * Kernel syscalls return ULMK_ENOTSUP when this is 0.
 */
#ifndef ULMK_ARCH_HAS_CACHE
#ifdef ULMK_BOARD_ENABLE_CPU_CACHE
#define ULMK_ARCH_HAS_CACHE	ULMK_BOARD_ENABLE_CPU_CACHE
#else
#define ULMK_ARCH_HAS_CACHE	0
#endif
#endif

#ifndef ULMK_ARCH_PMP_NUM
#define ULMK_ARCH_PMP_NUM	8
#endif

#define ULMK_ARCH_MAX_REGIONS	12
#define ULMK_ARCH_REGION_ALIGN	64

/*
 * Slot map.  Boards override when boot firmware locks slots: a write to a
 * locked entry is discarded by the hardware, so a role left on top of one is
 * never programmed and the grant silently goes missing.
 */
#ifndef ULMK_ARCH_PMP_KERNEL
#define ULMK_ARCH_PMP_KERNEL	0
#endif
#ifndef ULMK_ARCH_PMP_KRAM
#define ULMK_ARCH_PMP_KRAM	1
#endif
#ifndef ULMK_ARCH_PMP_UTEXT
#define ULMK_ARCH_PMP_UTEXT	2
#endif
#ifndef ULMK_ARCH_PMP_URAM
#define ULMK_ARCH_PMP_URAM	3
#endif
#ifndef ULMK_ARCH_PMP_MMIO
#define ULMK_ARCH_PMP_MMIO	4
#endif
#ifndef ULMK_ARCH_PMP_USER_BASE
#define ULMK_ARCH_PMP_USER_BASE	5
#endif

#ifndef ULMK_ARCH_PMP_DYNAMIC_BASE
#if ULMK_ARCH_PMP_NUM <= 5
#define ULMK_ARCH_PMP_DYNAMIC_BASE	ULMK_ARCH_PMP_NUM
#else
#define ULMK_ARCH_PMP_DYNAMIC_BASE	6
#endif
#endif

/* High slots reserved for on-demand map/unmap (ESP32-P4 has 16). */
#ifndef ULMK_ARCH_PMP_TEMP0
#if ULMK_ARCH_PMP_NUM >= 16
#define ULMK_ARCH_PMP_TEMP0		14
#else
#define ULMK_ARCH_PMP_TEMP0		(ULMK_ARCH_PMP_NUM > 0 ? ULMK_ARCH_PMP_NUM - 1 : 0)
#endif
#endif
#ifndef ULMK_ARCH_PMP_TEMP1
#if ULMK_ARCH_PMP_NUM >= 16
#define ULMK_ARCH_PMP_TEMP1		15
#else
#define ULMK_ARCH_PMP_TEMP1		ULMK_ARCH_PMP_TEMP0
#endif
#endif

/*
 * Slots the dynamic grant sweep must step over: board extras and anything
 * boot firmware locked.  Bit n = slot n is spoken for.
 */
#ifndef ULMK_ARCH_PMP_RESERVED_MASK
#define ULMK_ARCH_PMP_RESERVED_MASK	0u
#endif

/*
 * Describe the user RAM window with TOR instead of NAPOT.  NAPOT can only
 * name a naturally aligned power of two, so on a layout where user RAM
 * neither starts nor ends on such a boundary the window is rounded outwards
 * and swallows whatever sits below it — kernel RAM, typically, which hands
 * U-mode exactly the access the region was meant to withhold.  Costs the
 * slot below ULMK_ARCH_PMP_URAM, which then holds the lower bound.
 */
#ifndef ULMK_ARCH_PMP_URAM_TOR
#define ULMK_ARCH_PMP_URAM_TOR	0
#endif

#define ULMK_ARCH_PRS_KERNEL	0u
#define ULMK_ARCH_PRS_USER	1u

#ifndef ULMK_ARCH_IDLE_IS_WFI
#define ULMK_ARCH_IDLE_IS_WFI	1
#endif

#ifndef ULMK_ARCH_HAVE_CLINT
#define ULMK_ARCH_HAVE_CLINT	0
#endif

#ifndef ULMK_ARCH_HAVE_PLIC
#define ULMK_ARCH_HAVE_PLIC	0
#endif

#ifndef ULMK_ARCH_HAVE_CLIC
#define ULMK_ARCH_HAVE_CLIC	0
#endif

/*
 * Espressif CLIC: mtvec mode=3 + MTVT jump table (interrupt_id 0..47).
 * Leave 0 for QEMU/direct-mode CLIC experiments.
 */
#ifndef ULMK_ARCH_CLIC_VECTORED
#define ULMK_ARCH_CLIC_VECTORED	0
#endif

#if !ULMK_ARCH_HAVE_CLINT && !ULMK_ARCH_HAVE_PLIC && !ULMK_ARCH_HAVE_CLIC
#error "RISC-V: enable ULMK_ARCH_HAVE_CLINT, ULMK_ARCH_HAVE_PLIC, and/or ULMK_ARCH_HAVE_CLIC in board_config.h"
#endif

#ifndef ULMK_BOARD_CLINT_BASE
#define ULMK_BOARD_CLINT_BASE	0u
#endif

#define ULMK_ARCH_CLINT_MSIP0		(ULMK_BOARD_CLINT_BASE + 0x0u)
#define ULMK_ARCH_CLINT_MTIMECMP0	(ULMK_BOARD_CLINT_BASE + 0x4000u)
#define ULMK_ARCH_CLINT_MTIMECMP(n) \
	(ULMK_BOARD_CLINT_BASE + 0x4000u + ((uint32_t)(n) * 8u))
#define ULMK_ARCH_CLINT_MTIME		(ULMK_BOARD_CLINT_BASE + 0xBFF8u)

/* CLINT timebase Hz for kernel tick (board may override). */
#ifndef ULMK_BOARD_TICK_CLOCK_HZ
#define ULMK_BOARD_TICK_CLOCK_HZ	10000000u
#endif

#ifndef ULMK_BOARD_CLIC_BASE
#define ULMK_BOARD_CLIC_BASE	0u
#endif

#define ULMK_ARCH_CLIC_INT_BASE		(ULMK_BOARD_CLIC_BASE + 0x1000u)
#define ULMK_ARCH_CLIC_INT_REG(irq)	(ULMK_ARCH_CLIC_INT_BASE + (irq) * 4u)

#ifndef ULMK_ARCH_PLIC_SRC
#define ULMK_ARCH_PLIC_SRC(id)		((uint32_t)(id) << 2)
#endif

#ifndef ULMK_BOARD_PLIC_BASE
#define ULMK_BOARD_PLIC_BASE	0u
#endif

#if ULMK_ARCH_HAVE_PLIC
#if ULMK_BOARD_PLIC_BASE == 0u
#error "board_config.h must define ULMK_BOARD_PLIC_BASE when ULMK_ARCH_HAVE_PLIC=1"
#endif

#define ULMK_ARCH_PLIC_PRIORITY_BASE	(ULMK_BOARD_PLIC_BASE + 0x0u)
#define ULMK_ARCH_PLIC_PENDING_BASE	(ULMK_BOARD_PLIC_BASE + 0x1000u)
#define ULMK_ARCH_PLIC_ENABLE_BASE	(ULMK_BOARD_PLIC_BASE + 0x2000u)
#define ULMK_ARCH_PLIC_ENABLE_STRIDE	0x80u
#define ULMK_ARCH_PLIC_CONTEXT_BASE	(ULMK_BOARD_PLIC_BASE + 0x200000u)
#define ULMK_ARCH_PLIC_CONTEXT_STRIDE	0x1000u
#endif /* ULMK_ARCH_HAVE_PLIC */

#define ULMK_ARCH_CTX_FRAME_SIZE	64u

#define MSTATUS_MIE_BIT		(1u << 3)
#define MSTATUS_MPIE_BIT	(1u << 7)
#define MSTATUS_MPP_SHIFT	11u
#define MSTATUS_MPP_U		(0u << MSTATUS_MPP_SHIFT)
#define MSTATUS_MPP_M		(3u << MSTATUS_MPP_SHIFT)

#endif /* ULMK_ARCH_RISCV_CONFIG_H */
