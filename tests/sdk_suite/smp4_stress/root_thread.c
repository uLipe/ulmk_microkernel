/* SPDX-License-Identifier: MIT */
/*
 * smp4_stress — one worker per hart (0..3); pin + yield loop, then report.
 *
 * Same spawn+wait cadence as smp4_smoke (stable under QEMU MTTCG load).
 * Yields exercise remote RQ / syscall paths without a create storm.
 */
#include <stdint.h>
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>

#define NCPU	4u
#define NYIELD	32u

static volatile uint32_t g_seen;
static volatile uint32_t g_bad;
static ulmk_notif_t g_done;

static void worker(void *arg)
{
	uint32_t expect = (uint32_t)(uintptr_t)arg;
	uint32_t i;

	for (i = 0u; i < NYIELD; i++) {
		if (ulmk_cpu_id() != expect) {
			g_bad = 1u;
			ulmk_notif_signal(g_done, 1u);
			ulmk_thread_exit();
		}
		ulmk_thread_yield();
	}

	g_seen = ulmk_cpu_id();
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
	board_console_puts("smp4_stress: begin\n");

	if (ulmk_cpu_id() != 0u) {
		board_console_puts("smp4_stress: FAIL root cpu\n");
		for (;;)
			;
	}

	g_done = ulmk_notif_create();
	if (g_done == ULMK_NOTIF_INVALID) {
		board_console_puts("smp4_stress: FAIL notif\n");
		for (;;)
			;
	}

	for (cpu = 0u; cpu < NCPU; cpu++) {
		g_seen = 0xffffffffu;
		g_bad = 0u;
		attr.name = "st";
		attr.entry = worker;
		attr.arg = (void *)(uintptr_t)cpu;
		attr.priority = 2u;
		attr.stack_size = 4096u;
		attr.privilege = ULMK_PRIV_DRIVER;
		attr.caps = ULMK_CAP_INHERIT;
		attr.cpu = (uint8_t)cpu;

		if (ulmk_thread_create(&attr) == ULMK_TID_INVALID) {
			board_console_puts("smp4_stress: FAIL spawn\n");
			for (;;)
				;
		}

		for (i = 0u; i < 100000u && g_seen == 0xffffffffu && g_bad == 0u;
		     i++)
			ulmk_thread_yield();

		ulmk_notif_wait(g_done, 1u, &bits);

		if (g_bad != 0u || g_seen != cpu) {
			board_console_puts("smp4_stress: FAIL\n");
			for (;;)
				;
		}
	}

	board_console_puts("smp4_stress: PASS\n");
	ulmk_thread_exit();
}
