/* SPDX-License-Identifier: MIT */
/*
 * mem_grant — peer read/write after grant (sdk_suite promotion of silicon).
 */
#include "sdk_test_util.h"

#define MAGIC_OWNER	0xC0FFEEu
#define MAGIC_PEER	0xBEEFu

static int g_pass;
static int g_fail;
static ulmk_notif_t g_done;
static volatile uint32_t *g_shared;
static volatile int g_peer_read_ok;
static volatile int g_peer_wrote;
static volatile uint32_t g_peer_saw;

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

static void peer_entry(void *arg)
{
	uint32_t bits = 0u;

	(void)arg;
	ulmk_notif_wait(g_done, 0x1u, &bits);
	if (g_shared) {
		g_peer_saw = g_shared[0];
		g_peer_read_ok = (g_peer_saw == MAGIC_OWNER);
		g_shared[0] = MAGIC_PEER;
		g_peer_wrote = 1;
	}
	ulmk_notif_signal(g_done, 0x2u);
	ulmk_thread_exit();
}

static void idle_dead(void *arg)
{
	(void)arg;
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	uint32_t  *page;
	ulmk_tid_t peer;
	ulmk_tid_t dead;
	uint32_t   bits = 0u;
	int        rc;

	board_services_init(info);
	sdk_puts("mem_grant: begin\n");
	g_pass = 0;
	g_fail = 0;
	g_shared = NULL;
	g_peer_read_ok = 0;
	g_peer_wrote = 0;

	g_done = ulmk_notif_create();
	CHECK("notif", g_done != ULMK_NOTIF_INVALID);

	page = (uint32_t *)ulmk_mem_map(NULL, 256u,
					ULMK_PERM_READ | ULMK_PERM_WRITE,
					ULMK_MMAP_ANON);
	CHECK("map", sdk_map_ok(page));
	if (!sdk_map_ok(page))
		goto report;

	page[0] = MAGIC_OWNER;
	g_shared = page;

	/* No inherited areas: the grant is the peer's only way in. */
	peer = sdk_spawn("peer", peer_entry, NULL, 10u, 1024u, ULMK_CAP_NONE);
	CHECK("peer", peer != ULMK_TID_INVALID);
	rc = ulmk_mem_grant((void *)page, 256u, peer,
			    ULMK_PERM_READ | ULMK_PERM_WRITE);
	CHECK("grant", rc == ULMK_OK);

	ulmk_notif_signal(g_done, 0x1u);
	bits = 0u;
	ulmk_notif_wait(g_done, 0x2u, &bits);
	CHECK("peer_read", g_peer_read_ok);
	CHECK("peer_wrote", g_peer_wrote);
	CHECK("owner_sees", page[0] == MAGIC_PEER);

	CHECK("grant_null",
	      ulmk_mem_grant(NULL, 256u, peer, ULMK_PERM_READ) != ULMK_OK);

	dead = sdk_spawn("dead", idle_dead, NULL, 5u, 1024u, 0u);
	if (dead != ULMK_TID_INVALID) {
		CHECK("kill_dead", ulmk_thread_kill(dead) == ULMK_OK);
		CHECK("grant_dead",
		      ulmk_mem_grant((void *)page, 256u, dead,
				     ULMK_PERM_READ) != ULMK_OK);
	} else {
		CHECK("kill_dead", 0);
		CHECK("grant_dead", 0);
	}
	CHECK("unmap", ulmk_mem_unmap((void *)page, 256u) == ULMK_OK);

report:
	sdk_puts("mem_grant: pass=");
	sdk_put_u32((uint32_t)g_pass);
	sdk_puts(" fail=");
	sdk_put_u32((uint32_t)g_fail);
	sdk_puts("\n");
	sdk_puts(g_fail == 0 ? "mem_grant: PASS\n" : "mem_grant: FAIL\n");
	ulmk_thread_exit();
}
