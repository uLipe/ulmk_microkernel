/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Simulator exit — boards/qemu_tc3xx/board_sim_exit.c
 *
 * Userspace side of the Linumiz virt console: writing the exit register makes
 * QEMU leave with that status.  The kernel has its own path to the same
 * register in qemu_printk_hook.c; that one is supervisor-only, so a demo
 * running at driver privilege cannot reach it and maps the region itself.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <ulmk/board.h>

#define VIRT_BASE        0xBF000000UL
#define VIRT_REGION_SIZE 0x40U
#define VIRT_EXIT_OFF    0x28U

__attribute__((noreturn)) void ulmk_board_sim_exit(int code)
{
	volatile uint32_t *reg;

	reg = (volatile uint32_t *)((uintptr_t)ulmk_mem_map(
		(void *)VIRT_BASE,
		VIRT_REGION_SIZE,
		ULMK_PERM_READ | ULMK_PERM_WRITE,
		ULMK_MMAP_PERIPH) + VIRT_EXIT_OFF);

	*reg = (uint32_t)code;

	for (;;)
		;
}
