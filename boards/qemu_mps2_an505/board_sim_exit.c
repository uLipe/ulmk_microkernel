/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Simulator exit — boards/qemu_mps2_an505/board_sim_exit.c
 *
 * MPS2 has no exit register, so this goes through ARM semihosting instead:
 * BKPT 0xAB needs no peripheral mapping, which suits a demo running at driver
 * privilege.  QEMU must be started with userspace=on (see board.cmake) — it
 * ignores semihosting from unprivileged code otherwise, and the breakpoint
 * escalates to a HardFault.
 */

#include <stdint.h>
#include <ulmk/board.h>

#define SYS_EXIT_EXTENDED		0x20u
#define ADP_STOPPED_APPLICATION_EXIT	0x20026u

__attribute__((noreturn)) void ulmk_board_sim_exit(int code)
{
	uint32_t block[2];

	block[0] = ADP_STOPPED_APPLICATION_EXIT;
	block[1] = (uint32_t)code;

	{
		register uint32_t r0 __asm__("r0") = SYS_EXIT_EXTENDED;
		register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)block;

		__asm__ volatile("bkpt 0xAB"
				 : : "r"(r0), "r"(r1) : "memory");
	}

	for (;;)
		;
}
