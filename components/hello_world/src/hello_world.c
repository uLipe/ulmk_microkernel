/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Hello world task — components/hello_world/src/hello_world.c
 *
 * Demonstrates the board console IPC pattern:
 *   - board_console_putc() / board_console_puts() are the public C API of
 *     the board_console service (defined in boards/<board>/board_console.h).
 *   - All output goes through IPC; no direct MMIO access from this component.
 *
 * The task prints a single greeting and exits.  It deliberately does not use
 * the board timer: with no OS-level mutex yet, only one thread may drive the
 * single board-timer server at a time, so the periodic behaviour lives in the
 * ping_pong demo where synchronous IPC serialises timer access (see
 * ulmk_apps/ping_pong/src/ping_pong.c).
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <ulmk/linker.h>
#include <hello_world.h>

/* board_console_puts is resolved at link time from the board sources.
 * Forward-declared here to avoid a board-specific include inside a portable
 * component. */
void board_console_puts(const char *s);

static void hello_entry(void *arg)
{
	(void)arg;

	board_console_puts("ulmk: hello from userspace — hello world!\n");
	ulmk_thread_exit();
}

ulmk_tid_t hello_world_init(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	ulmk_tid_t         tid;
	static ULMK_PRIVATE int done;

	(void)info;

	if (done)
		return ULMK_TID_INVALID;
	done = 1;

	attr.name       = "hello";
	attr.entry      = hello_entry;
	attr.arg        = NULL;
	attr.priority   = 10u;
	attr.stack_size = 2048u;
	attr.privilege  = ULMK_PRIV_USER;

	tid = ulmk_thread_create(&attr);
	return tid;
}
