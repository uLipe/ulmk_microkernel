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
 * board_services_init() is resolved at link time: the board provides a strong
 * definition; stub/board_services_stub.c provides a weak no-op fallback.
 */

#include <ulmk/microkernel.h>
#include <hello_world.h>

/* Resolved at link time by the board's board_services.c (strong) or
 * stub/board_services_stub.c (weak no-op). */
void board_services_init(const ulmk_boot_info_t *info);

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	board_services_init(info);
	hello_world_init(info);
	ulmk_thread_exit();
}
