/* SPDX-License-Identifier: MIT */
/*
 * Memory isolation — public API + linker symbols.
 */
#include "sdk_test_util.h"

extern uint8_t _ulmk_kernel_text_start[];
extern uint8_t _ulmk_kernel_data_start[];

#define PATTERN_A	0xA5A5A5A5u
#define STACK_SZ	2048u
#define BIT_GO		(1u << 0)
#define BIT_DONE	(1u << 1)
#define BIT_ARMED	(1u << 2)
#define BIT_ALIVE	(1u << 3)
#define PROBE_MS	200u

static ulmk_notif_t   g_sync;
static volatile void *g_shared;
static volatile int   g_grant_result = -1;
static volatile uint32_t *volatile g_probe_addr;

static void grant_reader(void *arg)
{
	volatile uint32_t *buf;
	uint32_t           bits = 0u;

	(void)arg;
	ulmk_notif_wait(g_sync, BIT_GO, &bits);
	buf = (volatile uint32_t *)g_shared;
	g_grant_result = (buf && buf[0] == PATTERN_A) ? 1 : 0;
	ulmk_notif_signal(g_sync, BIT_DONE);
	ulmk_thread_exit();
}

/*
 * Each probe announces BIT_ARMED, performs the access that must be refused,
 * and only reaches BIT_ALIVE if it was let through.  Signalling before the
 * access instead — and then counting yields — cannot tell a thread that was
 * killed from one that merely never got the CPU back, and reports the second
 * as a success.
 */
static void kexec_trigger(void *arg)
{
	typedef void (*fn_t)(void);
	fn_t     fn;
	uint32_t bits = 0u;

	(void)arg;
	ulmk_notif_wait(g_sync, BIT_GO, &bits);
	/*
	 * Prefer an unmapped page over kernel text: executing kernel symbols as
	 * user can run privileged code under PRIVDEFENA rather than faulting.
	 */
	fn = (fn_t)(uintptr_t)0x1000u;
	(void)_ulmk_kernel_text_start;
	ulmk_notif_signal(g_sync, BIT_ARMED);
	fn();
	ulmk_notif_signal(g_sync, BIT_ALIVE);
	ulmk_thread_exit();
}

static void kread_trigger(void *arg)
{
	volatile uint32_t val;
	uint32_t          bits = 0u;

	(void)arg;
	ulmk_notif_wait(g_sync, BIT_GO, &bits);
	ulmk_notif_signal(g_sync, BIT_ARMED);
	val = *(volatile uint32_t *)(uintptr_t)_ulmk_kernel_data_start;
	(void)val;
	ulmk_notif_signal(g_sync, BIT_ALIVE);
	ulmk_thread_exit();
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

static int probe_run(const char *label, ulmk_tid_t tid)
{
	uint32_t bits = 0u;

	if (tid == ULMK_TID_INVALID) {
		sdk_puts("mem_isolation: FAIL spawn ");
		sdk_puts(label);
		sdk_puts("\n");
		return 0;
	}

	ulmk_notif_signal(g_sync, BIT_GO);
	if (ulmk_notif_wait(g_sync, BIT_ARMED, &bits) != ULMK_OK) {
		sdk_puts("mem_isolation: FAIL arm ");
		sdk_puts(label);
		sdk_puts("\n");
		return 0;
	}

	bits = 0u;
	if (ulmk_notif_wait_timeout(g_sync, BIT_ALIVE, &bits,
				    PROBE_MS) == ULMK_ETIMEOUT) {
		sdk_puts("mem_isolation: ");
		sdk_puts(label);
		sdk_puts(" PASS\n");
		return 1;
	}

	sdk_puts("mem_isolation: FAIL ");
	sdk_puts(label);
	sdk_puts(" (access allowed)\n");
	return 0;
}

static int probe_denied(const char *label, const char *name,
			void (*entry)(void *))
{
	return probe_run(label, sdk_spawn(name, entry, NULL, 30u, STACK_SZ,
					  ULMK_CAP_INHERIT));
}

/* @addr is read by a fresh thread holding @caps; the read must fault. */
static int probe_addr_denied(const char *label, volatile uint32_t *addr,
			     uint32_t caps)
{
	g_probe_addr = addr;
	return probe_run(label, sdk_spawn("paddr", addr_trigger, NULL, 30u,
					  STACK_SZ, caps));
}

static void supervisor(void *arg)
{
	uint32_t bits;
	int      overall = 1;
	void    *base;
	volatile uint32_t *buf;
	int      i;
	int      ok;

	(void)arg;
	sdk_puts("mem_isolation: supervisor\n");

	base = ulmk_mem_map(NULL, 128u, ULMK_PERM_READ | ULMK_PERM_WRITE,
			    ULMK_MMAP_ANON);
	ok = sdk_map_ok(base);
	if (ok) {
		buf = (volatile uint32_t *)base;
		for (i = 0; i < 4; i++)
			buf[i] = PATTERN_A;
		for (i = 0; i < 4; i++) {
			if (buf[i] != PATTERN_A)
				ok = 0;
		}
	}
	if (ok)
		sdk_puts("mem_isolation: scenario 1 (anon mmap) PASS\n");
	else {
		sdk_puts("mem_isolation: FAIL scenario 1\n");
		sdk_puts("mem_isolation: FAIL\n");
		ulmk_thread_exit();
	}

	/* Scenario 2 — cross-thread grant. */
	{
		ulmk_tid_t reader;
		void      *gbuf;

		g_grant_result = -1;
		gbuf = ulmk_mem_map(NULL, 64u, ULMK_PERM_READ | ULMK_PERM_WRITE,
				    ULMK_MMAP_ANON);
		if (!sdk_map_ok(gbuf)) {
			sdk_puts("mem_isolation: FAIL scenario 2\n");
			overall = 0;
		} else {
			*(volatile uint32_t *)gbuf = PATTERN_A;
			g_shared = gbuf;
			/* Explicit caps: the reader must not inherit gbuf. */
			reader = sdk_spawn("reader", grant_reader, NULL, 30u,
					   STACK_SZ, ULMK_CAP_NONE);
			if (reader == ULMK_TID_INVALID ||
			    ulmk_mem_grant(gbuf, 64u, reader,
					  ULMK_PERM_READ) < 0) {
				sdk_puts("mem_isolation: FAIL scenario 2\n");
				overall = 0;
			} else {
				bits = 0u;
				ulmk_notif_signal(g_sync, BIT_GO);
				ulmk_notif_wait(g_sync, BIT_DONE, &bits);
				if (g_grant_result == 1)
					sdk_puts("mem_isolation: scenario 2 (grant read) PASS\n");
				else {
					sdk_puts("mem_isolation: FAIL scenario 2\n");
					overall = 0;
				}
			}
			ulmk_mem_unmap(gbuf, 64u);
		}
	}

	/*
	 * QEMU's TriCore MPU does not hold U-mode out of kernel memory the way
	 * the silicon does, so the probe would report a kernel bug that is
	 * really an emulation gap.  Kept out of the run rather than softened: a
	 * probe whose failure is tolerated is the reason this case went years
	 * without noticing a real hole.  The same ground is covered on hardware
	 * by the TC275 silicon suite.
	 */
#if defined(__TRICORE__) || defined(__tricore__)
	sdk_puts("mem_isolation: scenario 5 (kernel data fault) SKIP\n");
	sdk_puts("mem_isolation: scenario 4 (kernel exec fault) SKIP\n");
	(void)kread_trigger;
	(void)kexec_trigger;
#else
	if (!probe_denied("scenario 5 (kernel data fault)", "kread",
			  kread_trigger))
		overall = 0;

	if (!probe_denied("scenario 4 (kernel exec fault)", "kexec",
			  kexec_trigger))
		overall = 0;
#endif

	/*
	 * Scenarios 3, 6 and 7 fault only because the pool is in no static
	 * window.  QEMU's TriCore MPU has too few ranges for lazy windows, so
	 * the user RAM window there runs to the end of the pool.  The TC275
	 * silicon suite (silicon_mem_grant) covers them on hardware.
	 */
#if defined(__TRICORE__) || defined(__tricore__)
	sdk_puts("mem_isolation: scenario 3 (unmap revoke) SKIP\n");
	sdk_puts("mem_isolation: scenario 6 (no inherited area) SKIP\n");
	sdk_puts("mem_isolation: scenario 7 (TCB read) SKIP\n");
	(void)probe_denied;
	(void)probe_addr_denied;
#else
	/*
	 * Scenario 3 — the pool is kernel heap, not a static user window, so
	 * an area really goes away: a child that inherited the block loses it
	 * when the owner unmaps (cascade), and must fault on the next access.
	 */
	{
		volatile uint32_t *blk;
		ulmk_tid_t         t;

		blk = (volatile uint32_t *)ulmk_malloc(64u);
		g_probe_addr = blk;
		t = blk ? sdk_spawn("paddr", addr_trigger, NULL, 30u, STACK_SZ,
				    ULMK_CAP_INHERIT) : ULMK_TID_INVALID;
		if (blk)
			ulmk_free((void *)blk);
		if (!probe_run("scenario 3 (unmap revoke)", t))
			overall = 0;
	}

	/* Scenario 6 — explicit caps carry no areas: the parent's block is out. */
	{
		volatile uint32_t *blk = (volatile uint32_t *)ulmk_malloc(64u);

		if (!blk || !probe_addr_denied("scenario 6 (no inherited area)",
					       blk, ULMK_CAP_NONE))
			overall = 0;
		if (blk)
			ulmk_free((void *)blk);
	}

	/* Scenario 7 — a TCB is kernel heap; its handle must not be a door. */
	if (!probe_addr_denied("scenario 7 (TCB read)",
			       (volatile uint32_t *)(uintptr_t)ulmk_thread_self(),
			       ULMK_CAP_INHERIT))
		overall = 0;
#endif

	ulmk_mem_unmap(base, 128u);
	base = NULL;

	/* Last fault may leave the CPU wedged — report before more syscalls. */
	if (overall)
		sdk_puts("mem_isolation: PASS\n");
	else
		sdk_puts("mem_isolation: FAIL\n");
	ulmk_thread_exit();
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	ulmk_tid_t tid;

	board_services_init(info);
	sdk_puts("mem_isolation: start\n");

	g_sync = ulmk_notif_create();
	tid = sdk_spawn("sup", supervisor, NULL, 10u, 4096u, ULMK_CAP_INHERIT);
	(void)tid;
	ulmk_thread_exit();
}
