/* SPDX-License-Identifier: MIT */
/*
 * boards/qemu_riscv_virt_smp4/board_config.h
 *
 * SoC constants for qemu-system-riscv32 -machine virt -smp 4.
 * Consumed by arch/riscv via arch_config.h and board sources.
 */

#ifndef ULMK_BOARD_CONFIG_H
#define ULMK_BOARD_CONFIG_H

/*
 * Sibling of qemu_riscv_virt for 4-hart SMP scale tests (CI + sdk_suite/smp4_*).
 * QEMU: -smp 4.  UP builds still compile with this constant but only hart 0
 * is started unless ULMK_CONFIG_ENABLE_SMP=1.
 */
#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU		4
#endif

/* CLINT for MSIP soft-IRQ / IPI; board timer stays on Goldfish RTC via PLIC. */
#ifndef ULMK_ARCH_HAVE_CLINT
#define ULMK_ARCH_HAVE_CLINT		1
#endif

#ifndef ULMK_ARCH_HAVE_CLIC
#define ULMK_ARCH_HAVE_CLIC		0
#endif

#ifndef ULMK_ARCH_HAVE_PLIC
#define ULMK_ARCH_HAVE_PLIC		1
#endif

#define ULMK_BOARD_PLIC_BASE		0x0C000000u

#ifndef ULMK_BOARD_CLINT_BASE
#define ULMK_BOARD_CLINT_BASE		0x02000000u
#endif

#define ULMK_BOARD_TIMER_RTC_BASE	0x00101000u
#define ULMK_BOARD_TIMER_RTC_MAP_SIZE	0x1000u
#define ULMK_BOARD_TIMER_PLIC_IRQ	11u
#define ULMK_BOARD_TIMER_HW_CLOCK_HZ	1000000000u
#define ULMK_BOARD_TICK_CLOCK_HZ	10000000u

#ifndef ULMK_ARCH_HAVE_FPU
#define ULMK_ARCH_HAVE_FPU		0
#endif

#ifndef ULMK_ARCH_PMP_NUM
#define ULMK_ARCH_PMP_NUM		8
#endif

/* See qemu_riscv_virt: NAPOT rounds user RAM out over kernel RAM. */
#define ULMK_ARCH_PMP_URAM		4
#define ULMK_ARCH_PMP_URAM_TOR		1
#define ULMK_ARCH_PMP_MMIO		5

#ifndef ULMK_ARCH_IDLE_IS_WFI
#define ULMK_ARCH_IDLE_IS_WFI		0
#endif

/* Generic MMIO window for ULMK_MMAP_PERIPH tests (UART0). */
#define ULMK_BOARD_PERIPH_BASE		0x10000000u
#define ULMK_BOARD_PERIPH_SIZE		0x1000u

#endif /* ULMK_BOARD_CONFIG_H */
