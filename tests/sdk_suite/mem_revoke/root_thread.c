/* SPDX-License-Identifier: MIT */
/*
 * mem_revoke — bookkeeping after grant+revoke.
 *
 * Anon heap lives inside the shared user-RAM window on every current arch,
 * so a peer can still touch the bytes after revoke; proving a fault needs
 * per-allocation MPU/PMP, which the kernel does not do yet.  What we can
 * prove today: revoke removes the peer's region entry (second revoke is
 * ESRCH/EINVAL), and the owner keeps the mapping.
 */
#include "sdk_test_util.h"

#define MAGIC_OWNER	0xC0FFEEu
#define STACK_SZ	2048u
#define BIT_GO		(1u << 0)
#define BIT_DONE	(1u << 1)

static int g_pass;
static int g_fail;
static ulmk_notif_t g_done;
static volatile uint32_t *g_shared;
static volatile int g_peer_saw_after_revoke;

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
	ulmk_notif_wait(g_done, BIT_GO, &bits);
	if (g_shared)
		g_peer_saw_after_revoke = (g_shared[0] == MAGIC_OWNER);
	ulmk_notif_signal(g_done, BIT_DONE);
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	uint32_t *page;
	ulmk_tid_t peer;
	uint32_t bits = 0u;
	int rc;

	board_services_init(info);
	sdk_puts("mem_revoke: begin\n");
	g_pass = 0;
	g_fail = 0;
	g_shared = NULL;
	g_peer_saw_after_revoke = 0;

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

	peer = sdk_spawn("peer", peer_entry, NULL, 10u, STACK_SZ, 0u);
	CHECK("peer", peer != ULMK_TID_INVALID);
	if (peer == ULMK_TID_INVALID)
		goto report;

	rc = ulmk_mem_grant((void *)page, 256u, peer,
			    ULMK_PERM_READ | ULMK_PERM_WRITE);
	CHECK("grant", rc == ULMK_OK);

	rc = ulmk_mem_revoke((void *)page, peer);
	CHECK("revoke", rc == ULMK_OK);

	rc = ulmk_mem_revoke((void *)page, peer);
	CHECK("revoke_gone", rc != ULMK_OK);

	CHECK("revoke_null", ulmk_mem_revoke(NULL, peer) != ULMK_OK);
	CHECK("revoke_bad_tid",
	      ulmk_mem_revoke((void *)page, ULMK_TID_INVALID) != ULMK_OK);

	/* Re-grant still works after a clean revoke. */
	rc = ulmk_mem_grant((void *)page, 256u, peer,
			    ULMK_PERM_READ | ULMK_PERM_WRITE);
	CHECK("regrant", rc == ULMK_OK);
	rc = ulmk_mem_revoke((void *)page, peer);
	CHECK("revoke2", rc == ULMK_OK);

	ulmk_notif_signal(g_done, BIT_GO);
	bits = 0u;
	CHECK("peer_done",
	      ulmk_notif_wait(g_done, BIT_DONE, &bits) == ULMK_OK);

	CHECK("owner_still", page[0] == MAGIC_OWNER);
	CHECK("unmap", ulmk_mem_unmap((void *)page, 256u) == ULMK_OK);

report:
	sdk_puts("mem_revoke: pass=");
	sdk_put_u32((uint32_t)g_pass);
	sdk_puts(" fail=");
	sdk_put_u32((uint32_t)g_fail);
	sdk_puts("\n");
	sdk_puts(g_fail == 0 ? "mem_revoke: PASS\n" : "mem_revoke: FAIL\n");
	ulmk_thread_exit();
}
