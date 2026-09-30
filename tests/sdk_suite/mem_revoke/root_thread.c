/* SPDX-License-Identifier: MIT */
/*
 * mem_revoke — revoke removes access, and takes derived grants with it.
 *
 * The owner grants a block to a peer, the peer re-grants it to a third
 * thread, then the owner revokes the peer.  Both must fault on their next
 * access: the pool is not covered by any static user window, so an area
 * that is gone leaves nothing to fall back on.
 */
#include "sdk_test_util.h"

#define MAGIC_OWNER	0xC0FFEEu
#define STACK_SZ	2048u
#define PROBE_MS	200u
#define BIT_GO		(1u << 0)
#define BIT_READ	(1u << 1)
#define BIT_GO2		(1u << 2)
#define BIT_ARMED	(1u << 3)
#define BIT_ALIVE	(1u << 4)

static int g_pass;
static int g_fail;
static ulmk_notif_t g_peer_sync;
static ulmk_notif_t g_gc_sync;
static volatile uint32_t *g_shared;
static volatile ulmk_tid_t g_gc;
static volatile int g_peer_read_ok;
static volatile int g_peer_regrant_ok;

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

/*
 * Read once while granted, then — after the owner revoked — read again
 * between ARMED and ALIVE.  Reaching ALIVE means the revoke left access.
 */
static void reader(ulmk_notif_t sync, volatile int *read_ok)
{
	volatile uint32_t val;
	uint32_t bits = 0u;

	ulmk_notif_wait(sync, BIT_GO, &bits);
	*read_ok = (g_shared[0] == MAGIC_OWNER);
	ulmk_notif_signal(sync, BIT_READ);

	bits = 0u;
	ulmk_notif_wait(sync, BIT_GO2, &bits);
	ulmk_notif_signal(sync, BIT_ARMED);
	val = g_shared[0];
	(void)val;
	ulmk_notif_signal(sync, BIT_ALIVE);
	ulmk_thread_exit();
}

static volatile int g_gc_read_ok;

static void peer_entry(void *arg)
{
	(void)arg;
	g_peer_regrant_ok = (ulmk_mem_grant((void *)g_shared, 256u, g_gc,
					    ULMK_PERM_READ) == ULMK_OK);
	reader(g_peer_sync, &g_peer_read_ok);
}

static void gc_entry(void *arg)
{
	(void)arg;
	reader(g_gc_sync, &g_gc_read_ok);
}

static int killed_on_access(ulmk_notif_t sync)
{
	uint32_t bits = 0u;

	ulmk_notif_signal(sync, BIT_GO2);
	if (ulmk_notif_wait(sync, BIT_ARMED, &bits) != ULMK_OK)
		return 0;
	bits = 0u;
	return ulmk_notif_wait_timeout(sync, BIT_ALIVE, &bits, PROBE_MS) ==
	       ULMK_ETIMEOUT;
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

	g_peer_sync = ulmk_notif_create();
	g_gc_sync   = ulmk_notif_create();
	CHECK("notif", g_peer_sync != ULMK_NOTIF_INVALID &&
		       g_gc_sync != ULMK_NOTIF_INVALID);

	page = (uint32_t *)ulmk_mem_map(NULL, 256u,
					ULMK_PERM_READ | ULMK_PERM_WRITE,
					ULMK_MMAP_ANON);
	CHECK("map", sdk_map_ok(page));
	if (!sdk_map_ok(page))
		goto report;

	page[0] = MAGIC_OWNER;
	g_shared = page;

	/* Explicit caps: neither thread inherits the block. */
	g_gc = sdk_spawn("gc", gc_entry, NULL, 20u, STACK_SZ, ULMK_CAP_NONE);
	peer = sdk_spawn("peer", peer_entry, NULL, 10u, STACK_SZ,
			 ULMK_CAP_NONE);
	CHECK("spawn", g_gc != ULMK_TID_INVALID && peer != ULMK_TID_INVALID);
	if (g_gc == ULMK_TID_INVALID || peer == ULMK_TID_INVALID)
		goto report;

	rc = ulmk_mem_grant((void *)page, 256u, peer,
			    ULMK_PERM_READ | ULMK_PERM_WRITE);
	CHECK("grant", rc == ULMK_OK);

	/* Peer runs its re-grant, then both read once while granted. */
	ulmk_notif_signal(g_peer_sync, BIT_GO);
	ulmk_notif_wait(g_peer_sync, BIT_READ, &bits);
	bits = 0u;
	ulmk_notif_signal(g_gc_sync, BIT_GO);
	ulmk_notif_wait(g_gc_sync, BIT_READ, &bits);
	CHECK("peer_regrant", g_peer_regrant_ok);
	CHECK("peer_read", g_peer_read_ok);
	CHECK("gc_read", g_gc_read_ok);

	CHECK("revoke_foreign",
	      ulmk_mem_revoke((void *)page, ulmk_thread_self()) != ULMK_OK);
	rc = ulmk_mem_revoke((void *)page, peer);
	CHECK("revoke", rc == ULMK_OK);
	rc = ulmk_mem_revoke((void *)page, peer);
	CHECK("revoke_gone", rc != ULMK_OK);
	CHECK("revoke_cascade",
	      ulmk_mem_revoke((void *)page, g_gc) != ULMK_OK);
	CHECK("revoke_null", ulmk_mem_revoke(NULL, peer) != ULMK_OK);
	CHECK("revoke_bad_tid",
	      ulmk_mem_revoke((void *)page, ULMK_TID_INVALID) != ULMK_OK);

	/*
	 * QEMU's TriCore MPU has too few ranges for lazy windows, so the user
	 * RAM window there runs to the end of the pool and a revoked block
	 * stays reachable.  silicon_mem_grant checks both faults on the TC275.
	 */
#if defined(__TRICORE__) || defined(__tricore__)
	sdk_puts(".skip peer_fault\n.skip gc_fault\n");
	(void)killed_on_access;
#else
	CHECK("peer_fault", killed_on_access(g_peer_sync));
	CHECK("gc_fault", killed_on_access(g_gc_sync));
#endif

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
