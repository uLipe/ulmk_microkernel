/* SPDX-License-Identifier: MIT */
/*
 * Board services — boards/qemu_riscv_virt_smp4/board_services.c
 *
 * Console IPC server.  Sleep uses the kernel timing wheel.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include "board_services.h"
#include "board_console.h"
#include "board_timer.h"
#include "board_internal.h"

#define CONSOLE_MSG_PUTC		1u
#define CONSOLE_MSG_WRITE		2u
#define CONSOLE_WRITE_MAX		256u

#define UART_LSR			5u
#define UART_LSR_TX_IDLE		(1u << 5)

static ulmk_ep_t          g_ep __attribute__((section(".user_bss")));
static volatile uint8_t *g_uart __attribute__((section(".user_bss")));

ulmk_ep_t board_service_ep(void)
{
	return g_ep;
}

static void console_putc_hw(char c)
{
	while (!(g_uart[UART_LSR] & UART_LSR_TX_IDLE))
		;
	g_uart[0] = (uint8_t)c;
}

static void board_server(void *arg)
{
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_msg_t next;
	ulmk_tid_t sender;
	ulmk_tid_t next_sender;
	int rc;

	(void)arg;

	g_uart = (volatile uint8_t *)ulmk_mem_map(
		(void *)(uintptr_t)BOARD_CONSOLE_UART_BASE,
		BOARD_CONSOLE_UART_MAP_SIZE,
		ULMK_PERM_READ | ULMK_PERM_WRITE,
		ULMK_MMAP_PERIPH);
	if (!g_uart)
		ulmk_thread_exit();

	reply.label    = 0u;
	reply.words[0] = 0u;

	rc = ulmk_ep_recv(g_ep, &msg, &sender);
	if (rc != ULMK_OK)
		ulmk_thread_exit();

	for (;;) {
		if (msg.label == CONSOLE_MSG_PUTC) {
			console_putc_hw((char)(uint8_t)msg.words[0]);
		} else if (msg.label == CONSOLE_MSG_WRITE) {
			const char *buf =
				(const char *)(uintptr_t)msg.words[0];
			uint32_t len = msg.words[1];
			uint32_t i;

			if (buf && len > 0u) {
				if (len > CONSOLE_WRITE_MAX)
					len = CONSOLE_WRITE_MAX;
				for (i = 0u; i < len; i++)
					console_putc_hw(buf[i]);
			}
		}
		rc = ulmk_ep_reply_recv(g_ep, sender, &reply, &next,
					&next_sender);
		if (rc != ULMK_OK)
			continue;
		msg = next;
		sender = next_sender;
	}
}

void ulmk_board_init(void)
{
}

void board_services_init(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	ulmk_tid_t         tid;

	(void)info;

	g_ep = ulmk_ep_create();

	attr.name       = "bsvc";
	attr.entry      = board_server;
	attr.priority   = 1u;
	attr.stack_size = 4096u;
	attr.privilege  = ULMK_PRIV_DRIVER;

	tid = ulmk_thread_create(&attr);
	if (tid == ULMK_TID_INVALID)
		return;

	ulmk_cap_grant(tid, ULMK_CAP_MAP_PERIPH);
	(void)board_timer_start(info);
}
