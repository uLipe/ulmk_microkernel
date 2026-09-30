/* SPDX-License-Identifier: MIT */
/*
 * smp4_smoke — one DRIVER worker pinned per hart (0..3); expect all seen.
 *
 * Spawn+wait per CPU (same pattern as smp_smoke) so each hart is proven
 * before the next remote create. Concurrent multi-hart first-schedule is
 * covered by smp4_stress.
 */
#include <stdint.h>
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>

#define NCPU	4u

static volatile uint32_t g_seen;
static ulmk_notif_t g_done;

static void worker(void *arg)
{
	uint32_t expect = (uint32_t)(uintptr_t)arg;

	g_seen = ulmk_cpu_id();
	(void)expect;
	ulmk_notif_signal(g_done, 1u);
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	uint32_t bits = 0;
	uint32_t cpu;
	uint32_t i;

	board_services_init(info);
	board_console_puts("smp4_smoke: begin\n");

	if (ulmk_cpu_id() != 0u) {
		board_console_puts("smp4_smoke: FAIL root not on CPU0\n");
		for (;;)
			;
	}

	g_done = ulmk_notif_create();
	if (g_done == ULMK_NOTIF_INVALID) {
		board_console_puts("smp4_smoke: FAIL notif\n");
		for (;;)
			;
	}

	for (cpu = 0u; cpu < NCPU; cpu++) {
		g_seen = 0xffffffffu;
		attr.name = "w";
		attr.entry = worker;
		attr.arg = (void *)(uintptr_t)cpu;
		attr.priority = 2u;
		attr.stack_size = 4096u;
		attr.privilege = ULMK_PRIV_DRIVER;
		attr.caps = ULMK_CAP_INHERIT;
		attr.cpu = (uint8_t)cpu;
		if (ulmk_thread_create(&attr) == ULMK_TID_INVALID) {
			board_console_puts("smp4_smoke: FAIL spawn\n");
			for (;;)
				;
		}

		for (i = 0u; i < 100000u && g_seen == 0xffffffffu; i++)
			ulmk_thread_yield();

		ulmk_notif_wait(g_done, 1u, &bits);

		if (g_seen != cpu) {
			board_console_puts("smp4_smoke: FAIL cpu\n");
			for (;;)
				;
		}
	}

	board_console_puts("smp4_smoke: PASS\n");
	ulmk_thread_exit();
}
