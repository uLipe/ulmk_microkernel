/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * hello_world component public API — components/hello_world/include/hello_world.h
 *
 * This component is the reference ROOT_THREAD provider.  It demonstrates
 * the IPC-based board console pattern: all output goes through
 * board_console_putc() / board_console_puts() which are the public C API
 * of the board_console service.
 */

#ifndef HELLO_WORLD_H
#define HELLO_WORLD_H

#include <ulmk/microkernel.h>

/*
 * Spawn the hello task thread.  Call once from ulmk_root_thread() after
 * board_services_init() has been called.  Returns the thread ID; returns
 * ULMK_TID_INVALID on a duplicate call (double-init guard).
 */
ulmk_tid_t hello_world_init(const ulmk_boot_info_t *info);

/*
 * Block until the hello task has printed.  The caller creates the task and
 * then has nothing left to do, so without this it would race ahead and end
 * the run before the greeting reaches the console.
 */
void hello_world_wait(void);

#endif /* HELLO_WORLD_H */
