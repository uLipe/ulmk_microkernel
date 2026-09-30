/* SPDX-License-Identifier: MIT */
/* boards/qemu_riscv_virt/board_timer.h */

#ifndef BOARD_TIMER_H
#define BOARD_TIMER_H

#include <stdint.h>
#include <ulmk/microkernel.h>
#include "board_config.h"

#define BOARD_TIMER_RTC_BASE		ULMK_BOARD_TIMER_RTC_BASE
#define BOARD_TIMER_RTC_MAP_SIZE	ULMK_BOARD_TIMER_RTC_MAP_SIZE
#define BOARD_TIMER_PLIC_IRQ		ULMK_BOARD_TIMER_PLIC_IRQ
#define BOARD_TIMER_HW_CLOCK_HZ		ULMK_BOARD_TIMER_HW_CLOCK_HZ

ulmk_tid_t board_timer_start(const ulmk_boot_info_t *info);
void       board_timer_sleep_us(uint32_t us);
uint32_t   board_timer_now_ticks(void);
uint32_t   board_timer_ticks_to_ns(uint32_t dt);

#endif /* BOARD_TIMER_H */
