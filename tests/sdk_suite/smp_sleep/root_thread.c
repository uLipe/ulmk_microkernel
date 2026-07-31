/* SPDX-License-Identifier: MIT */
/*
 * Sleep on a secondary hart must expire.  Boards with a shared tick
 * (wheel parked on CPU0) and per-hart CLINT both need this path.
 *
 * Root blocks on a notif (must not spin at prio 0) so the console server
 * can run if the worker prints.
 */
#include <stdint.h>
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>

static volatile uint32_t g_cpu;
static ulmk_notif_t g_done;

static void worker_cpu1(void *arg)
{
	(void)arg;
	g_cpu = ulmk_cpu_id();
	(void)ulmk_sleep_ms(50u);
	ulmk_notif_signal(g_done, 0x1u);
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	uint32_t bits = 0;

	(void)info;
	board_services_init(info);
	board_console_puts("smp_sleep: begin\n");

	if (ulmk_cpu_id() != 0u) {
		board_console_puts("smp_sleep: FAIL root cpu\n");
		for (;;)
			;
	}

	g_done = ulmk_notif_create();
	attr.name = "slp1";
	attr.entry = worker_cpu1;
	attr.priority = 1u;
	attr.stack_size = 2048u;
	attr.privilege = ULMK_PRIV_DRIVER;
	attr.cpu = 1u;
	if (ulmk_thread_create(&attr) == ULMK_TID_INVALID) {
		board_console_puts("smp_sleep: FAIL spawn\n");
		for (;;)
			;
	}

	ulmk_notif_wait(g_done, 0x1u, &bits);

	if (g_cpu != 1u) {
		board_console_puts("smp_sleep: FAIL wrong cpu\n");
		for (;;)
			;
	}

	board_console_puts("smp_sleep: PASS\n");
	ulmk_thread_exit();
}
