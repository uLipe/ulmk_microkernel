/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Simulator exit — boards/qemu_riscv_virt_smp4/board_sim_exit.c
 *
 * The virt machine exposes the SiFive test finisher; writing PASS with a
 * status in the upper half powers the machine down.  Runs at driver privilege
 * out of the board archive, so it maps the register itself.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <ulmk/board.h>

#define FINISHER_BASE   0x00100000UL
#define FINISHER_SIZE   0x1000U
#define FINISHER_PASS   0x5555U

__attribute__((noreturn)) void ulmk_board_sim_exit(int code)
{
	volatile uint32_t *fin;

	fin = (volatile uint32_t *)ulmk_mem_map(
		(void *)FINISHER_BASE,
		FINISHER_SIZE,
		ULMK_PERM_READ | ULMK_PERM_WRITE,
		ULMK_MMAP_PERIPH);

	*fin = FINISHER_PASS | ((uint32_t)(code & 0xFF) << 16);

	for (;;)
		;
}
