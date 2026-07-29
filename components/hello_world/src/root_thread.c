/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Root thread entry — components/hello_world/src/root_thread.c
 *
 * Provides the ulmk_root_thread() entry point required by the kernel boot model.
 * Initialisation order:
 *   1. board_services_init() — starts board hardware services (console, etc.)
 *      and returns with every service endpoint ready.
 *   2. hello_world_init()    — spawns the hello task.
 *   3. ulmk_thread_exit()    — root thread terminates; scheduler takes over.
 *
 * On a board that can stop its simulator the demo ends the run instead, once
 * the greeting is out: there is no further work, and idling forever would
 * leave the caller waiting on a timeout to reclaim the machine.
 *
 * board_services_init() is resolved at link time: the board provides a strong
 * definition; stub/board_services_stub.c provides a weak no-op fallback.
 */

#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <hello_world.h>

#if ULMK_CONFIG_SIM_EXIT
#include <ulmk/board.h>
#endif

/* Resolved at link time by the board's board_services.c (strong) or
 * stub/board_services_stub.c (weak no-op). */
void board_services_init(const ulmk_boot_info_t *info);

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	board_services_init(info);
	hello_world_init(info);
#if ULMK_CONFIG_SIM_EXIT
	hello_world_wait();
	ulmk_board_sim_exit(0);
#endif
	ulmk_thread_exit();
}
