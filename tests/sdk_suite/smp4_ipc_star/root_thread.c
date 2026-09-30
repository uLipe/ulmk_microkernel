/* SPDX-License-Identifier: MIT */
/*
 * smp4_ipc_star — server on CPU3; clients on CPU0/1/2 ep_call and echo src.
 *
 * Bring the server up first and wait until it is ready, then start clients
 * one CPU at a time so cross-hart IPC is exercised without a spawn storm.
 */
#include <stdint.h>
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>

#define MSG_PING	1u
#define NCLIENT	3u

static volatile ulmk_ep_t g_ep;
static volatile uint32_t g_got[NCLIENT];
static volatile uint32_t g_server_ready;
static volatile uint32_t g_server_n;
static ulmk_notif_t g_done;

static void settle(void)
{
	uint32_t i;

	for (i = 0u; i < 2000u; i++)
		ulmk_thread_yield();
}

static void server_cpu3(void *arg)
{
	ulmk_ep_t ep = (ulmk_ep_t)(uintptr_t)arg;
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_tid_t sender;
	uint32_t src;

	g_server_ready = 1u;

	while (g_server_n < NCLIENT) {
		if (ulmk_ep_recv(ep, &msg, &sender) != ULMK_OK)
			continue;
		if (msg.label != MSG_PING)
			continue;
		src = msg.words[0];
		reply.label = MSG_PING;
		reply.words[0] = src;
		reply.words[1] = ulmk_cpu_id();
		(void)ulmk_ep_reply(sender, &reply);
		if (src < NCLIENT)
			g_got[src] = 1u;
		g_server_n++;
	}

	ulmk_notif_signal(g_done, 0x1u);
	ulmk_thread_exit();
}

static void client(void *arg)
{
	uint32_t expect = (uint32_t)(uintptr_t)arg;
	ulmk_msg_t msg;
	uint32_t i;

	while (g_server_ready == 0u)
		ulmk_thread_yield();

	for (i = 0u; i < 400u; i++) {
		msg.label = MSG_PING;
		msg.words[0] = expect;
		if (ulmk_ep_call(g_ep, &msg) != ULMK_OK) {
			ulmk_thread_yield();
			continue;
		}
		if (msg.words[0] == expect && msg.words[1] == 3u) {
			g_got[expect] = 1u;
			ulmk_notif_signal(g_done, 1u << (expect + 1u));
			ulmk_thread_exit();
		}
		ulmk_thread_yield();
	}

	ulmk_notif_signal(g_done, 1u << (expect + 1u));
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	ulmk_ep_t ep;
	ulmk_tid_t tid;
	uint32_t bits = 0;
	uint32_t cpu;
	uint32_t i;
	uint32_t ok;

	board_services_init(info);
	board_console_puts("smp4_ipc_star: begin\n");

	if (ulmk_cpu_id() != 0u) {
		board_console_puts("smp4_ipc_star: FAIL root cpu\n");
		for (;;)
			;
	}

	for (cpu = 0u; cpu < NCLIENT; cpu++)
		g_got[cpu] = 0u;
	g_server_ready = 0u;
	g_server_n = 0u;
	g_done = ulmk_notif_create();
	ep = ulmk_ep_create();
	if (g_done == ULMK_NOTIF_INVALID || ep == ULMK_EP_INVALID) {
		board_console_puts("smp4_ipc_star: FAIL alloc\n");
		for (;;)
			;
	}
	g_ep = ep;

	attr.name = "srv3";
	attr.entry = server_cpu3;
	attr.arg = (void *)(uintptr_t)ep;
	attr.priority = 2u;
	attr.stack_size = 4096u;
	attr.privilege = ULMK_PRIV_DRIVER;
	attr.caps = ULMK_CAP_INHERIT;
	attr.cpu = 3u;
	tid = ulmk_thread_create(&attr);
	if (tid == ULMK_TID_INVALID) {
		board_console_puts("smp4_ipc_star: FAIL spawn server\n");
		for (;;)
			;
	}
	(void)ulmk_ep_grant(ep, tid);

	for (i = 0u; i < 100000u && g_server_ready == 0u; i++)
		ulmk_thread_yield();
	if (g_server_ready == 0u) {
		board_console_puts("smp4_ipc_star: FAIL server\n");
		for (;;)
			;
	}

	for (cpu = 0u; cpu < NCLIENT; cpu++) {
		attr.name = "cli";
		attr.entry = client;
		attr.arg = (void *)(uintptr_t)cpu;
		attr.priority = 3u;
		attr.stack_size = 4096u;
		attr.privilege = ULMK_PRIV_DRIVER;
		attr.caps = ULMK_CAP_INHERIT;
		attr.cpu = (uint8_t)cpu;
		tid = ulmk_thread_create(&attr);
		if (tid == ULMK_TID_INVALID) {
			board_console_puts("smp4_ipc_star: FAIL spawn client\n");
			for (;;)
				;
		}
		(void)ulmk_ep_grant(ep, tid);
		ulmk_notif_wait(g_done, 1u << (cpu + 1u), &bits);
		settle();
	}

	ulmk_notif_wait(g_done, 0x1u, &bits);

	ok = 1u;
	for (cpu = 0u; cpu < NCLIENT; cpu++) {
		if (g_got[cpu] != 1u)
			ok = 0u;
	}
	if (!ok) {
		board_console_puts("smp4_ipc_star: FAIL clients\n");
		for (;;)
			;
	}

	board_console_puts("smp4_ipc_star: PASS\n");
	ulmk_thread_exit();
}
