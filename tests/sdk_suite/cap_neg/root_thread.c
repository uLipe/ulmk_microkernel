/* SPDX-License-Identifier: MIT */
/*
 * cap_neg — CAP/privilege EPERM matrix (sdk_suite promotion of silicon_cap_neg).
 */
#include "sdk_test_util.h"
#include "board_config.h"

#ifndef ULMK_BOARD_PERIPH_BASE
#define ULMK_BOARD_PERIPH_BASE	0xF0000000u
#endif

static int g_pass;
static int g_fail;
static ulmk_notif_t g_done;
static volatile int g_user_spawn_eperm;
static volatile int g_user_kill_eperm;
static volatile int g_user_sus_eperm;
static volatile int g_user_irq_eperm;
static volatile int g_user_heap_eperm;
static volatile int g_user_mmap_eperm;
static volatile int g_user_cap_eperm;
static volatile int g_drv_spawn_eperm;
static volatile int g_drv_kill_eperm;
static volatile int g_drv_cap_eperm;
static volatile int g_drv_irq_ok;
static volatile int g_drv_mmap_ok;
static ulmk_tid_t g_victim;

static void check(const char *name, int ok)
{
	sdk_puts(ok ? ".ok " : ".FAIL ");
	sdk_puts(name);
	sdk_puts("\n");
	if (ok)
		g_pass++;
	else
		g_fail++;
}

#define CHECK(name, cond) check((name), (cond) ? 1 : 0)

static int is_eperm(int rc)
{
	return rc == ULMK_EPERM;
}

static int tid_is_eperm(ulmk_tid_t tid)
{
	return (int32_t)(uintptr_t)tid == ULMK_EPERM;
}

static void idle_victim(void *arg)
{
	(void)arg;
	for (;;)
		ulmk_thread_yield();
}

static void user_probe(void *arg)
{
	ulmk_thread_attr_t a = {0};
	ulmk_notif_t       n;
	void              *p;
	ulmk_tid_t         tid;

	(void)arg;
	a.name       = "x";
	a.entry      = idle_victim;
	a.arg        = NULL;
	a.priority   = 200u;
	a.stack_size = 512u;
	a.privilege  = ULMK_PRIV_USER;
	tid = ulmk_thread_create(&a);
	g_user_spawn_eperm = tid_is_eperm(tid);
	g_user_kill_eperm = is_eperm(ulmk_thread_kill(g_victim));
	g_user_sus_eperm  = is_eperm(ulmk_thread_suspend(g_victim));
	n = ulmk_notif_create();
	g_user_irq_eperm = is_eperm(ulmk_irq_bind(5u, n, 0u));
	if (n != ULMK_NOTIF_INVALID)
		ulmk_notif_destroy(n);
	g_user_heap_eperm = (ulmk_malloc(64u) == NULL);
	p = ulmk_mem_map((void *)(uintptr_t)ULMK_BOARD_PERIPH_BASE, 64u,
			 ULMK_PERM_READ | ULMK_PERM_WRITE, ULMK_MMAP_PERIPH);
	g_user_mmap_eperm = (p == NULL);
	g_user_cap_eperm = is_eperm(ulmk_cap_grant(g_victim, ULMK_CAP_SPAWN));
	ulmk_notif_signal(g_done, 0x1u);
	ulmk_thread_exit();
}

static void driver_probe(void *arg)
{
	ulmk_thread_attr_t a = {0};
	ulmk_notif_t       n;
	void              *p;
	ulmk_tid_t         tid;

	(void)arg;
	a.name       = "y";
	a.entry      = idle_victim;
	a.arg        = NULL;
	a.priority   = 200u;
	a.stack_size = 512u;
	a.privilege  = ULMK_PRIV_USER;
	tid = ulmk_thread_create(&a);
	g_drv_spawn_eperm = tid_is_eperm(tid);
	g_drv_kill_eperm = is_eperm(ulmk_thread_kill(g_victim));
	g_drv_cap_eperm  = is_eperm(ulmk_cap_grant(g_victim, ULMK_CAP_SPAWN));
	n = ulmk_notif_create();
	g_drv_irq_ok = (n != ULMK_NOTIF_INVALID) &&
		       (ulmk_irq_bind(6u, n, 0u) == ULMK_OK);
	if (n != ULMK_NOTIF_INVALID)
		ulmk_notif_destroy(n);
	p = ulmk_mem_map((void *)(uintptr_t)ULMK_BOARD_PERIPH_BASE, 64u,
			 ULMK_PERM_READ | ULMK_PERM_WRITE, ULMK_MMAP_PERIPH);
	g_drv_mmap_ok = sdk_map_ok(p);
	if (g_drv_mmap_ok)
		(void)ulmk_mem_unmap(p, 64u);
	ulmk_notif_signal(g_done, 0x2u);
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	uint32_t bits = 0u;

	board_services_init(info);
	sdk_puts("cap_neg: begin\n");
	g_pass = 0;
	g_fail = 0;
	g_done = ulmk_notif_create();
	g_victim = sdk_spawn_priv("victim", idle_victim, NULL, 200u, 1024u,
				  ULMK_CAP_NONE,
				  ULMK_PRIV_USER);
	CHECK("victim", g_victim != ULMK_TID_INVALID);

	sdk_spawn_priv("uprobe", user_probe, NULL, 10u, 1024u, ULMK_CAP_NONE,
		       ULMK_PRIV_USER);
	bits = 0u;
	ulmk_notif_wait(g_done, 0x1u, &bits);
	CHECK("u_spawn", g_user_spawn_eperm);
	CHECK("u_kill", g_user_kill_eperm);
	CHECK("u_sus", g_user_sus_eperm);
	CHECK("u_irq", g_user_irq_eperm);
	CHECK("u_heap", g_user_heap_eperm);
	CHECK("u_mmap", g_user_mmap_eperm);
	CHECK("u_cap", g_user_cap_eperm);

	sdk_spawn_priv("dprobe", driver_probe, NULL, 10u, 1024u,
		       ULMK_CAP_MAP_PERIPH | ULMK_CAP_IRQ, ULMK_PRIV_DRIVER);
	bits = 0u;
	ulmk_notif_wait(g_done, 0x2u, &bits);
	CHECK("d_spawn", g_drv_spawn_eperm);
	CHECK("d_kill", g_drv_kill_eperm);
	CHECK("d_cap", g_drv_cap_eperm);
	CHECK("d_irq_ok", g_drv_irq_ok);
	CHECK("d_mmap_ok", g_drv_mmap_ok);

	CHECK("r_spawn",
	      sdk_spawn_priv("ok", idle_victim, NULL, 200u, 1024u, 0u,
			     ULMK_PRIV_USER) != ULMK_TID_INVALID);
	CHECK("r_cap", ulmk_cap_grant(g_victim, ULMK_CAP_SPAWN) == ULMK_OK);
	/* Root runs as DRIVER: a KERNEL child would escape its privilege. */
	CHECK("r_priv_esc",
	      sdk_spawn_priv("k", idle_victim, NULL, 200u, 1024u, 0u,
			     ULMK_PRIV_KERNEL) == ULMK_TID_INVALID);

	sdk_puts("cap_neg: pass=");
	sdk_put_u32((uint32_t)g_pass);
	sdk_puts(" fail=");
	sdk_put_u32((uint32_t)g_fail);
	sdk_puts("\n");
	sdk_puts(g_fail == 0 ? "cap_neg: PASS\n" : "cap_neg: FAIL\n");
	ulmk_thread_exit();
}
