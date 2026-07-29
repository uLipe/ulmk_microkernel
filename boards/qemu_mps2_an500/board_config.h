/* SPDX-License-Identifier: MIT */
/*
 * boards/qemu_mps2_an500/board_config.h
 *
 * SoC constants for qemu-system-arm -machine mps2-an500 (Cortex-M7, ARMv7-M).
 * Snapshotted into generated/ulmk/platform.h and consumed by arch/arm via
 * arch_config.h and by the board sources.
 *
 * Memory map (QEMU hw/arm/mps2.c, AN500):
 *   0x00000000  ZBT SSRAM1        code
 *   0x20000000  ZBT SSRAM2/3      data / user RAM
 *   0x40000000  CMSDK APB timer0  (NVIC IRQ 8)
 *   0x40004000  CMSDK APB UART0
 *   0x60000000  PSRAM (16 MB, unused)
 */

#ifndef ULMK_BOARD_CONFIG_H
#define ULMK_BOARD_CONFIG_H

/* Uniprocessor Cortex-M — SMP is not supported. */
#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU		1
#endif

/* ARMv7-M (PMSAv7 MPU). */
#define ULMK_ARCH_ARMV8M		0

/* FPU presence follows ULMK_CONFIG_FPU; see cmake/arm_fpu.cmake. */

#ifndef ULMK_ARCH_IDLE_IS_WFI
#define ULMK_ARCH_IDLE_IS_WFI		1
#endif

/* Core / SysTick clock and CMSDK timer clock (QEMU SYSCLK_FRQ). */
#define BOARD_CPU_HZ			25000000u

/* CMSDK APB UART0 (polled TX console). */
#define BOARD_CONSOLE_UART_BASE		0x40004000u
#define BOARD_CONSOLE_UART_MAP_SIZE	0x1000u

/* CMSDK APB timer0 → NVIC line 8. */
#define BOARD_TIMER_BASE		0x40000000u
#define BOARD_TIMER_MAP_SIZE		0x1000u
#define BOARD_TIMER_NVIC_IRQ		8u
#define BOARD_TIMER_HW_CLOCK_HZ		25000000u

/* Generic MMIO window for ULMK_MMAP_PERIPH tests (CMSDK timer0). */
#define ULMK_BOARD_PERIPH_BASE		0x40000000u
#define ULMK_BOARD_PERIPH_SIZE		0x1000u

#endif /* ULMK_BOARD_CONFIG_H */
