/* SPDX-License-Identifier: MIT */
/*
 * Board timer — sleeps on the kernel timing wheel (ulmk_sleep_ms); the
 * free-running count for board_timer_now_ticks() is CLINT mtime.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include "board_timer.h"

#define CLINT_MTIME_OFF		0xBFF8u
#define CLINT_MTIME_PAGE	(ULMK_BOARD_CLINT_BASE + (CLINT_MTIME_OFF & ~0xFFFu))
#define CLINT_MTIME_MAP_SIZE	0x1000u

static volatile uint32_t *g_mtime __attribute__((section(".user_bss")));

void board_timer_sleep_us(uint32_t us)
{
	uint32_t ms = (us + 999u) / 1000u;

	if (ms == 0u)
		ms = 1u;
	(void)ulmk_sleep_ms(ms);
}

uint32_t board_timer_now_ticks(void)
{
	if (!g_mtime)
		return 0u;
	return *g_mtime;
}

uint32_t board_timer_ticks_to_ns(uint32_t dt)
{
	uint64_t ns;

	ns = ((uint64_t)dt * 1000000000ull) / (uint64_t)ULMK_BOARD_FSTM_HZ;
	if (ns > 0xFFFFFFFFu)
		return 0xFFFFFFFFu;
	return (uint32_t)ns;
}

ulmk_tid_t board_timer_start(const ulmk_boot_info_t *info)
{
	uint8_t *page;

	(void)info;
	page = (uint8_t *)ulmk_mem_map((void *)(uintptr_t)CLINT_MTIME_PAGE,
				       CLINT_MTIME_MAP_SIZE, ULMK_PERM_READ,
				       ULMK_MMAP_PERIPH);
	if (page)
		g_mtime = (volatile uint32_t *)(page + (CLINT_MTIME_OFF & 0xFFFu));
	ulmk_tick_start();
	return (ulmk_tid_t)1u;
}
