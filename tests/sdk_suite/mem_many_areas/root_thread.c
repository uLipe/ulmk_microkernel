/* SPDX-License-Identifier: MIT */
/*
 * Many live areas — more than the arch keeps loaded at once.
 *
 * Every area is a lazy window: PMP/MPU ports hold a handful in dynamic
 * slots, Sv32 a ring of pages.  Touching N_AREAS areas twice makes every
 * port evict and refault, and the multi-page area makes a page-granular
 * port load each page of one area on its own.  A thread that holds none of
 * the areas must still fault on them.
 */
#include "sdk_test_util.h"

#define N_AREAS		40
#define AREA_SZ		64u
#define BIG_SZ		(3u * 4096u + 512u)
#define STACK_SZ	2048u
#define BIT_GO		(1u << 0)
#define BIT_ARMED	(1u << 1)
#define BIT_ALIVE	(1u << 2)
#define PROBE_MS	200u

static ulmk_notif_t g_sync;
static volatile uint32_t *volatile g_probe_addr;

static uint32_t pattern(uint32_t i)
{
	return 0x5A000000u | (i << 8) | 0xC3u;
}

static void addr_trigger(void *arg)
{
	volatile uint32_t val;
	uint32_t          bits = 0u;

	(void)arg;
	ulmk_notif_wait(g_sync, BIT_GO, &bits);
	ulmk_notif_signal(g_sync, BIT_ARMED);
	val = *g_probe_addr;
	(void)val;
	ulmk_notif_signal(g_sync, BIT_ALIVE);
	ulmk_thread_exit();
}

/* A fresh thread with no areas reads @addr; the read must fault. */
static int probe_denied(volatile uint32_t *addr)
{
	uint32_t   bits = 0u;
	ulmk_tid_t tid;

	g_probe_addr = addr;
	tid = sdk_spawn("probe", addr_trigger, NULL, 30u, STACK_SZ,
			ULMK_CAP_NONE);
	if (tid == ULMK_TID_INVALID)
		return 0;
	ulmk_notif_signal(g_sync, BIT_GO);
	if (ulmk_notif_wait(g_sync, BIT_ARMED, &bits) != ULMK_OK)
		return 0;
	bits = 0u;
	return ulmk_notif_wait_timeout(g_sync, BIT_ALIVE, &bits,
				       PROBE_MS) == ULMK_ETIMEOUT;
}

static int touch_all(volatile uint32_t *const *areas, int write)
{
	uint32_t i;

	for (i = 0u; i < N_AREAS; i++) {
		if (write)
			areas[i][0] = pattern(i);
		else if (areas[i][0] != pattern(i))
			return 0;
	}
	return 1;
}

static int touch_big(volatile uint8_t *big, int write)
{
	uint32_t off;

	for (off = 0u; off < BIG_SZ; off += 1024u) {
		if (write)
			big[off] = (uint8_t)(off >> 10);
		else if (big[off] != (uint8_t)(off >> 10))
			return 0;
	}
	if (write)
		big[BIG_SZ - 1u] = 0xE7u;
	return big[BIG_SZ - 1u] == 0xE7u;
}

static void worker(void *arg)
{
	volatile uint32_t *areas[N_AREAS];
	volatile uint8_t  *big;
	uint32_t i;
	int      ok = 1;

	(void)arg;
	for (i = 0u; i < N_AREAS; i++) {
		areas[i] = (volatile uint32_t *)ulmk_mem_map(NULL, AREA_SZ,
				ULMK_PERM_READ | ULMK_PERM_WRITE,
				ULMK_MMAP_ANON);
		if (!sdk_map_ok((void *)areas[i])) {
			sdk_puts("mem_many_areas: FAIL map\n");
			sdk_puts("mem_many_areas: FAIL\n");
			ulmk_thread_exit();
		}
	}
	big = (volatile uint8_t *)ulmk_mem_map(NULL, BIG_SZ,
			ULMK_PERM_READ | ULMK_PERM_WRITE, ULMK_MMAP_ANON);
	if (!sdk_map_ok((void *)big)) {
		sdk_puts("mem_many_areas: FAIL map big\n");
		sdk_puts("mem_many_areas: FAIL\n");
		ulmk_thread_exit();
	}

	if (touch_all(areas, 1) && touch_all(areas, 0) &&
	    touch_all(areas, 0)) {
		sdk_puts("mem_many_areas: areas PASS\n");
	} else {
		sdk_puts("mem_many_areas: FAIL areas\n");
		ok = 0;
	}

	if (touch_big(big, 1) && touch_all(areas, 0) && touch_big(big, 0)) {
		sdk_puts("mem_many_areas: multi-page PASS\n");
	} else {
		sdk_puts("mem_many_areas: FAIL multi-page\n");
		ok = 0;
	}

	/*
	 * The QEMU TriCore MPU runs the user RAM window over the whole pool
	 * (see mem_isolation), so there a heap area is not a fault boundary.
	 */
#if defined(__TRICORE__) || defined(__tricore__)
	sdk_puts("mem_many_areas: isolation SKIP\n");
	(void)probe_denied;
#else
	if (probe_denied(areas[N_AREAS - 1]) &&
	    probe_denied((volatile uint32_t *)(big + 2u * 4096u))) {
		sdk_puts("mem_many_areas: isolation PASS\n");
	} else {
		sdk_puts("mem_many_areas: FAIL isolation\n");
		ok = 0;
	}
#endif

	for (i = 0u; i < N_AREAS; i++)
		ulmk_mem_unmap((void *)areas[i], AREA_SZ);
	ulmk_mem_unmap((void *)big, BIG_SZ);

	sdk_puts(ok ? "mem_many_areas: PASS\n" : "mem_many_areas: FAIL\n");
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	board_services_init(info);
	sdk_puts("mem_many_areas: start\n");

	g_sync = ulmk_notif_create();
	(void)sdk_spawn("worker", worker, NULL, 10u, 4096u, ULMK_CAP_INHERIT);
	ulmk_thread_exit();
}
