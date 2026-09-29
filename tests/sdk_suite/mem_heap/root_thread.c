/* SPDX-License-Identifier: MIT */
/*
 * mem_heap — malloc/free as syscalls on the lazy MPU/PMP model.
 *
 * One large block plus more live small blocks than any port has protection
 * slots: every access is faulted in on demand and the windows are recycled,
 * so the data must survive eviction and reload.
 */
#include "sdk_test_util.h"

#define HEAP_SIZE	4096u
#define N_SMALL		24u
#define PATTERN		0x5A5A0000u

static volatile int g_pass;
static volatile int g_fail;
static ulmk_notif_t g_done;

#define CHECK(cond) do { if (cond) g_pass++; else g_fail++; } while (0)

static void heap_test(void *arg)
{
	volatile uint32_t *small[N_SMALL];
	volatile uint8_t  *heap;
	size_t             i;
	int                ok;

	(void)arg;
	heap = (volatile uint8_t *)ulmk_malloc(HEAP_SIZE);
	CHECK(heap != NULL);
	if (!heap)
		goto out;

	for (i = 0; i < HEAP_SIZE; i++)
		heap[i] = (uint8_t)(i & 0xFFu);
	ok = 1;
	for (i = 0; i < HEAP_SIZE; i++) {
		if (heap[i] != (uint8_t)(i & 0xFFu)) {
			ok = 0;
			break;
		}
	}
	CHECK(ok);

	for (i = 0; i < N_SMALL; i++) {
		small[i] = (volatile uint32_t *)ulmk_malloc(64u);
		CHECK(small[i] != NULL);
		if (small[i])
			small[i][0] = PATTERN | (uint32_t)i;
	}
	ok = 1;
	for (i = 0; i < N_SMALL; i++) {
		if (small[i] && small[i][0] != (PATTERN | (uint32_t)i))
			ok = 0;
	}
	CHECK(ok);

	for (i = 0; i < N_SMALL; i++) {
		if (small[i])
			CHECK(ulmk_free((void *)small[i]) == ULMK_OK);
	}
	CHECK(ulmk_free((void *)heap) == ULMK_OK);
	CHECK(ulmk_free((void *)heap) != ULMK_OK);

out:
	ulmk_notif_signal(g_done, 1u);
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	uint32_t bits = 0u;

	board_services_init(info);
	sdk_puts("mem_heap: start\n");
	g_done = ulmk_notif_create();
	sdk_spawn("heap", heap_test, NULL, 1u, 1024u, ULMK_CAP_INHERIT);
	ulmk_notif_wait(g_done, 1u, &bits);

	if (g_fail == 0)
		sdk_puts("mem_heap: PASS\n");
	else
		sdk_puts("mem_heap: FAIL\n");
	ulmk_thread_exit();
}
