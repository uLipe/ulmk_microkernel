/* SPDX-License-Identifier: MIT */
/*
 * boards/qemu_mps2_an505/board_config.h
 *
 * SoC constants for qemu-system-arm -machine mps2-an505 (Cortex-M33, ARMv8-M).
 * Snapshotted into generated/ulmk/platform.h and consumed by arch/arm via
 * arch_config.h and by the board sources.
 *
 * Memory map (QEMU hw/arm/mps2-tz.c + IoTKit, AN505).  The board boots Secure
 * and this port is TrustZone-unaware, so every address uses the Secure alias
 * (IDAU bit 28 set):
 *   0x10000000  SSRAM1            code (Secure alias of 0x00000000)
 *   0x30000000  SSRAM2/3          data / user RAM (Secure alias of 0x20000000)
 *   0x50000000  CMSDK APB timer0  (NVIC IRQ 3, Secure alias of 0x40000000)
 *   0x50200000  CMSDK APB UART0   (peripheral expansion, Secure alias)
 */

#ifndef ULMK_BOARD_CONFIG_H
#define ULMK_BOARD_CONFIG_H

/* Uniprocessor Cortex-M — SMP is not supported. */
#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU		1
#endif

/* ARMv8-M mainline (PMSAv8 MPU). */
#define ULMK_ARCH_ARMV8M		1

/* FPU presence follows ULMK_CONFIG_FPU; see cmake/arm_fpu.cmake. */

#ifndef ULMK_ARCH_IDLE_IS_WFI
#define ULMK_ARCH_IDLE_IS_WFI		1
#endif

/* IoTKit MAINCLK / SysTick clock and CMSDK timer clock. */
#define BOARD_CPU_HZ			25000000u

/* CMSDK APB UART0 (peripheral expansion, polled TX console; Secure alias). */
#define BOARD_CONSOLE_UART_BASE		0x50200000u
#define BOARD_CONSOLE_UART_MAP_SIZE	0x1000u

/* IoTKit CMSDK APB timer0 → NVIC line 3 (Secure alias). */
#define BOARD_TIMER_BASE		0x50000000u
#define BOARD_TIMER_MAP_SIZE		0x1000u
#define BOARD_TIMER_NVIC_IRQ		3u
#define BOARD_TIMER_HW_CLOCK_HZ		25000000u

/* Generic MMIO window for ULMK_MMAP_PERIPH tests (CMSDK timer0). */
#define ULMK_BOARD_PERIPH_BASE		0x50000000u
#define ULMK_BOARD_PERIPH_SIZE		0x1000u

#endif /* ULMK_BOARD_CONFIG_H */
