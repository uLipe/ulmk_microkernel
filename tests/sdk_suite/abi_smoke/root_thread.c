/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * SDK consumer end-to-end test — tests/sdk_e2e/sdk_test.c
 *
 * This is a *standalone* application: it is compiled against nothing but the
 * shipped SDK (public headers) and linked against the two distributable
 * archives + the processed linker script — exactly as an external firmware
 * project would consume ulmk.  It never includes kernel-internal headers.
 *
 * Purpose: exercise every public microkernel syscall from the root thread and
 * report a single PASS/FAIL sentinel.  Board services are used only as the
 * output transport (the QEMU boards are already validated elsewhere); the board
 * hardware itself is not under test here.
 */

#include <stdint.h>
#include <stddef.h>
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>

/* ------------------------------------------------------------------------- */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------- */

static int g_pass;
static int g_fail;

static void put_u32(uint32_t v)
{
	char buf[10];
	int  i = 0;

	if (v == 0u) {
		board_console_putc('0');
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (i--)
		board_console_putc(buf[i]);
}

static void check(const char *name, int ok)
{
	board_console_puts(ok ? "  [ok]   " : "  [FAIL] ");
	board_console_puts(name);
	board_console_putc('\n');
	if (ok)
		g_pass++;
	else
		g_fail++;
}

#define CHECK(name, cond) check((name), (cond) ? 1 : 0)

static int map_ok(const void *p)
{
	uintptr_t u = (uintptr_t)p;

	if (u == 0u)
		return 0;
	if (u >= 0x80000000u)
		return 1;
	return (intptr_t)u > 0;
}

/* ------------------------------------------------------------------------- */
/* Shared state for helper-thread handshakes (lives in the shared user BSS)   */
/* ------------------------------------------------------------------------- */

static ulmk_notif_t     g_done;
static ulmk_ep_t        g_ipc_ep;
static volatile int     g_malloc_ok;
static volatile int     g_free_ok;

/* ------------------------------------------------------------------------- */
/* Helper threads                                                            */
/* ------------------------------------------------------------------------- */

/*
 * IPC echo server.  Round 1 uses plain recv + reply; round 2 uses reply_recv.
 * Both echo (label + 1) so the root thread can verify the round-trip.
 * After round 2 the server blocks forever waiting for a request that never
 * comes — harmless, it is the lowest-interest thread in the system.
 */
static void ipc_server(void *arg)
{
	ulmk_ep_t   ep = g_ipc_ep;
	ulmk_msg_t  m;
	ulmk_msg_t  next;
	ulmk_tid_t  snd;

	(void)arg;

	/* Round 1: ulmk_ep_recv + ulmk_ep_reply */
	if (ulmk_ep_recv(ep, &m, &snd) == ULMK_OK) {
		m.label += 1u;
		ulmk_ep_reply(snd, &m);
	}

	/* Round 2: ulmk_ep_reply_recv (first call only receives) */
	snd = ULMK_TID_INVALID;
	if (ulmk_ep_reply_recv(ep, snd, &m, &next, &snd) == ULMK_OK) {
		next.label += 1u;
		/* Reply to that sender; block again (never serviced). */
		ulmk_ep_reply_recv(ep, snd, &next, &m, &snd);
	}
}

/* Heap probe: malloc/free are syscalls, inherited ULMK_CAP_HEAP allows them. */
static void heap_probe(void *arg)
{
	volatile uint32_t *p;

	(void)arg;
	p = (volatile uint32_t *)ulmk_malloc(256u);
	if (p) {
		p[0]  = 0x600dcafeu;
		p[63] = 0x600dcafeu;
		g_malloc_ok = (p[0] == 0x600dcafeu && p[63] == 0x600dcafeu);
		g_free_ok = (ulmk_free((void *)p) == ULMK_OK &&
			     ulmk_free((void *)p) != ULMK_OK);
	}

	ulmk_notif_signal(g_done, 0x1u);
}

/*
 * Console text built on this thread's stack: the board server cannot see
 * that stack, so every later line (PASS banner included) only appears if the
 * bytes reached it by value.  Longer than one IPC message on purpose.
 */
static void console_probe(void *arg)
{
	char line[48];
	uint32_t i;

	(void)arg;
	for (i = 0u; i < sizeof(line) - 2u; i++)
		line[i] = (char)('a' + (i % 26u));
	line[sizeof(line) - 2u] = '\n';
	line[sizeof(line) - 1u] = '\0';
	board_console_puts(line);
	ulmk_notif_signal(g_done, 0x1u);
}

/* Low-priority target for thread-management and grant syscalls; never runs. */
static void idle_target(void *arg)
{
	(void)arg;
	for (;;)
		ulmk_thread_yield();
}

static ulmk_tid_t spawn(const char *name, void (*entry)(void *), void *arg,
			uint8_t prio)
{
	ulmk_thread_attr_t a = {0};

	a.name       = name;
	a.entry      = entry;
	a.arg        = arg;
	a.priority   = prio;
	a.stack_size = 1024;
	a.privilege  = ULMK_PRIV_DRIVER;
	return ulmk_thread_create(&a);
}

/* ------------------------------------------------------------------------- */
/* Test phases                                                               */
/* ------------------------------------------------------------------------- */

static ulmk_tid_t g_target;

static void test_thread_api(void)
{
	ulmk_tid_t self = ulmk_thread_self();

	CHECK("thread_self", self != ULMK_TID_INVALID);
	CHECK("thread_yield", ulmk_thread_yield() == ULMK_OK);

	g_target = spawn("target", idle_target, NULL, 200u);
	CHECK("thread_create", g_target != ULMK_TID_INVALID);

	CHECK("thread_priority_get(self)", ulmk_thread_priority_get(self) == 0);
	CHECK("thread_priority_get(target)",
	      ulmk_thread_priority_get(g_target) == 200);
	CHECK("thread_priority_set", ulmk_thread_priority_set(g_target, 150) == ULMK_OK);
	CHECK("thread_priority_set verify",
	      ulmk_thread_priority_get(g_target) == 150);
	CHECK("thread_suspend", ulmk_thread_suspend(g_target) == ULMK_OK);
	CHECK("thread_resume", ulmk_thread_resume(g_target) == ULMK_OK);
}

static void test_ipc(void)
{
	ulmk_ep_t  ep;
	ulmk_ep_t  tmp;
	ulmk_tid_t srv;
	ulmk_msg_t m;
	int        rc;

	ep = ulmk_ep_create();
	g_ipc_ep = ep;
	CHECK("ep_create", ep != ULMK_EP_INVALID);

	srv = spawn("ipc_srv", ipc_server, NULL, 1u);
	CHECK("ep_grant", ulmk_ep_grant(ep, srv) == ULMK_OK);

	m.label = 0x100u;
	rc = ulmk_ep_call(ep, &m);
	CHECK("ep_call + ep_recv + ep_reply", rc == ULMK_OK && m.label == 0x101u);

	m.label = 0x200u;
	rc = ulmk_ep_call(ep, &m);
	CHECK("ep_reply_recv", rc == ULMK_OK && m.label == 0x201u);

	/* ep_destroy on a throwaway endpoint (the IPC one still has srv blocked) */
	tmp = ulmk_ep_create();
	CHECK("ep_destroy", ulmk_ep_destroy(tmp) == ULMK_OK);
}

static void test_recv_or_notif(void)
{
	ulmk_ep_t    ep;
	ulmk_notif_t n;
	ulmk_msg_t   msg;
	ulmk_tid_t   sender;
	uint32_t     bits = 0u;
	int          rc;

	ep = ulmk_ep_create();
	n  = ulmk_notif_create();
	ulmk_notif_signal(n, 0x8u);

	/* Notification already pending → returns immediately (rc == 1). */
	rc = ulmk_ep_recv_or_notif(ep, n, 0x8u, &msg, &sender, &bits);
	CHECK("ep_recv_or_notif", rc == 1 && bits == 0x8u);

	ulmk_notif_destroy(n);
	ulmk_ep_destroy(ep);
}

static void test_notif(void)
{
	ulmk_notif_t n;
	uint32_t     bits = 0u;

	n = ulmk_notif_create();
	CHECK("notif_create", n != ULMK_NOTIF_INVALID);
	CHECK("notif_signal", ulmk_notif_signal(n, 0x1u) == ULMK_OK);
	CHECK("notif_poll", ulmk_notif_poll(n, 0x1u) == 0x1u);
	CHECK("notif_signal (2)", ulmk_notif_signal(n, 0x2u) == ULMK_OK);
	CHECK("notif_wait", ulmk_notif_wait(n, 0x2u, &bits) == ULMK_OK && bits == 0x2u);
	CHECK("notif_destroy", ulmk_notif_destroy(n) == ULMK_OK);
}

static void test_memory(void)
{
	volatile uint32_t *p;

	p = (volatile uint32_t *)ulmk_mem_map(
		NULL, 256u, ULMK_PERM_READ | ULMK_PERM_WRITE, ULMK_MMAP_ANON);
	CHECK("mem_map (anon)", map_ok((const void *)p));

	if (map_ok((const void *)p)) {
		p[0] = 0xdeadbeefu;
		CHECK("mem_map read/write", p[0] == 0xdeadbeefu);
		CHECK("mem_grant",
		      ulmk_mem_grant((void *)p, 256u, g_target,
				     ULMK_PERM_READ | ULMK_PERM_WRITE) == ULMK_OK);
		CHECK("mem_unmap", ulmk_mem_unmap((void *)p, 256u) == ULMK_OK);
	} else {
		check("mem_map read/write", 0);
		check("mem_grant", 0);
		check("mem_unmap", 0);
	}
}

static void test_heap(void)
{
	uint32_t bits = 0u;

	g_done = ulmk_notif_create();
	spawn("heap", heap_probe, NULL, 1u);
	ulmk_notif_wait(g_done, 0x1u, &bits);

	CHECK("malloc", g_malloc_ok);
	CHECK("free", g_free_ok);

	ulmk_notif_destroy(g_done);
}

static void test_console(void)
{
	uint32_t bits = 0u;

	g_done = ulmk_notif_create();
	spawn("cons", console_probe, NULL, 1u);
	ulmk_notif_wait(g_done, 0x1u, &bits);
	CHECK("console stack buffer", bits & 0x1u);
	ulmk_notif_destroy(g_done);
}

static void test_irq(void)
{
	ulmk_notif_t n = ulmk_notif_create();

	CHECK("irq_bind", ulmk_irq_bind(5u, n, 0u) == ULMK_OK);
	CHECK("irq_enable", ulmk_irq_enable(5u) == ULMK_OK);
	CHECK("irq_ack", ulmk_irq_ack(5u) == ULMK_OK);
	CHECK("irq_disable", ulmk_irq_disable(5u) == ULMK_OK);
	/* bind_hw with src_reg == 0 must be rejected without touching hardware. */
	CHECK("irq_bind_hw (reject)",
	      ulmk_irq_bind_hw(5u, n, 0u, 0u) == ULMK_EINVAL);

	ulmk_notif_destroy(n);
}

static void test_capabilities(void)
{
	CHECK("cap_grant", ulmk_cap_grant(g_target, ULMK_CAP_SPAWN) == ULMK_OK);
	CHECK("thread_kill", ulmk_thread_kill(g_target) == ULMK_OK);
}

/* ------------------------------------------------------------------------- */
/* Root thread                                                               */
/* ------------------------------------------------------------------------- */

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	board_services_init(info);

	board_console_puts("SDK_TEST: begin\n");

	test_thread_api();
	test_ipc();
	test_recv_or_notif();
	test_notif();
	test_memory();
	test_heap();
	test_console();
	test_irq();
	test_capabilities();

	board_console_puts("SDK_TEST: ");
	put_u32((uint32_t)g_pass);
	board_console_puts("/");
	put_u32((uint32_t)(g_pass + g_fail));
	board_console_puts(" checks passed\n");

	if (g_fail == 0)
		board_console_puts("SDK_TEST: PASS\n");
	else
		board_console_puts("SDK_TEST: FAIL\n");

	ulmk_thread_exit();
}
