/* SPDX-License-Identifier: MIT */
/*
 * smp_mem_shootdown — revoke and kill aimed at a thread running on CPU1.
 *
 * The CPU1 threads spin without a syscall, so nothing on that CPU ever
 * switches and a window loaded there stays live until someone flushes it:
 *
 *   revoke: root (CPU0) revokes a page the reader is spinning on, then
 *           writes it.  Without a shootdown the reader sees that write.
 *   kill:   root kills a spinner while it runs.  Freeing its TCB from CPU0
 *           left CPU1 running a thread whose record was already reused.
 */
#include <stdint.h>
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>

#define KILL_ROUNDS	16u

static volatile uint32_t *g_page;
static volatile uint32_t g_reads;
static volatile uint32_t g_leak;
static volatile uint32_t g_spins;
static ulmk_notif_t g_go;

static void fail(const char *why)
{
	board_console_puts("smp_mem_shootdown: FAIL ");
	board_console_puts(why);
	board_console_puts("\n");
	for (;;)
		ulmk_sleep_ms(1000u);
}

static void reader(void *arg)
{
	uint32_t bits = 0u;

	(void)arg;
	ulmk_notif_wait(g_go, 0x1u, &bits);
	for (;;) {
		if (g_page[0] != 1u)
			g_leak = 1u;
		g_reads++;
	}
}

static void spinner(void *arg)
{
	(void)arg;
	for (;;)
		g_spins++;
}

static ulmk_tid_t spawn_cpu1(const char *name, void (*entry)(void *),
			     uint32_t caps)
{
	ulmk_thread_attr_t attr = {0};

	attr.name       = name;
	attr.entry      = entry;
	attr.priority   = 1u;
	attr.stack_size = 1024u;
	attr.privilege  = ULMK_PRIV_USER;
	attr.caps       = caps;
	attr.cpu        = 1u;
	return ulmk_thread_create(&attr);
}

static int stopped(volatile uint32_t *counter)
{
	uint32_t snap;

	snap = *counter;
	ulmk_sleep_ms(20u);
	return *counter == snap;
}

static void test_revoke(void)
{
	ulmk_tid_t tid;
	uint32_t i;

	g_page = ulmk_malloc(256u);
	if (!g_page)
		fail("malloc");
	g_page[0] = 1u;
	g_go = ulmk_notif_create();

	tid = spawn_cpu1("reader", reader, ULMK_CAP_NONE);
	if (tid == ULMK_TID_INVALID)
		fail("spawn reader");
	if (ulmk_mem_grant((void *)g_page, 256u, tid, ULMK_PERM_READ) !=
	    ULMK_OK)
		fail("grant");
	ulmk_notif_signal(g_go, 0x1u);

	for (i = 0u; i < 100u && g_reads < 1000u; i++)
		ulmk_sleep_ms(1u);
	if (g_reads < 1000u)
		fail("reader never ran");

	if (ulmk_mem_revoke((void *)g_page, tid) != ULMK_OK)
		fail("revoke");
	g_page[0] = 2u;
	ulmk_sleep_ms(20u);
	if (g_leak)
		fail("reader saw a write made after revoke");
	if (ulmk_thread_priority_get(tid) != ULMK_ESRCH)
		fail("reader not killed");
	board_console_puts("smp_mem_shootdown: revoke ok\n");
}

static void test_kill(void)
{
	ulmk_tid_t tid;
	uint32_t round;
	uint32_t i;
	uint32_t start;

	for (round = 0u; round < KILL_ROUNDS; round++) {
		start = g_spins;
		tid = spawn_cpu1("spin", spinner, ULMK_CAP_NONE);
		if (tid == ULMK_TID_INVALID)
			fail("spawn spinner");
		for (i = 0u; i < 100u && g_spins == start; i++)
			ulmk_sleep_ms(1u);
		if (g_spins == start)
			fail("spinner never ran");
		if (ulmk_thread_kill(tid) != ULMK_OK)
			fail("kill");
		if (!stopped(&g_spins))
			fail("spinner still runs after kill");
	}
	board_console_puts("smp_mem_shootdown: kill ok\n");
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	board_services_init(info);
	board_console_puts("smp_mem_shootdown: begin\n");
	test_revoke();
	test_kill();
	board_console_puts("smp_mem_shootdown: PASS\n");
	ulmk_thread_exit();
}
