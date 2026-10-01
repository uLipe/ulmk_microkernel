/* SPDX-License-Identifier: MIT */
/*
 * qemu_riscv_virt_smp4/board_hil.c — userspace milestone marker (ulmk_board_hil_mark).
 *
 * Same contract as the silicon boards: a word in .user_bss any thread may
 * write, read back from the ELF symbol by a debugger (QEMU gdbstub).  QEMU
 * runs are judged by the console sentinels; this keeps the cases linking.
 */

#include <stdint.h>

volatile uint32_t g_ulmk_board_hil_scratch __attribute__((section(".user_bss")));

void ulmk_board_hil_mark(uint32_t n)
{
	g_ulmk_board_hil_scratch = n;
}
