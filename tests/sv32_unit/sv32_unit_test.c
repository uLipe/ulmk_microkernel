/* SPDX-License-Identifier: MIT */
/*
 * Sv32 protection backend unit tests — tests/sv32_unit/sv32_unit_test.c
 *
 * Builds arch/riscv/mmu_sv32.c on the host with satp, sfence.vma and the
 * PMP catch-all mocked, and checks the page tables it leaves behind: the
 * static user map, pinned stacks across switches, one page per lazy load,
 * the eviction ring, the level-2 pool and the per-hart split.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

#include "../../arch/riscv/mmu_sv32.c"

static int g_pass;
static int g_fail;

#define CHECK(cond, msg) \
	do { \
		if (cond) { \
			g_pass++; \
		} else { \
			printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); \
			g_fail++; \
		} \
	} while (0)

#define RW	(ULMK_PERM_READ | ULMK_PERM_WRITE)
#define LEAF_RW	(PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)
#define LEAF_RX	(PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
#define FLAGS(p)	((p) & 0x3FFu)

static uint32_t g_cpu;
static uint32_t g_satp;
static bool     g_satp_broken;
static bool     g_pmp_open;
static unsigned g_sfence_all;
static unsigned g_sfence_va;
static uintptr_t g_sfence_last;
static jmp_buf  g_panic_jmp;
static bool     g_panic_armed;

uint32_t ulmk_arch_cpu_id(void) { return g_cpu; }
void ulmk_arch_cpu_halt(void) { }
void ulmk_printk_char_out(char c) { (void)c; }

void sv32_hw_satp_write(uint32_t val) { g_satp = val; }
uint32_t sv32_hw_satp_read(void) { return g_satp_broken ? 0u : g_satp; }
void sv32_hw_sfence(uintptr_t va) { g_sfence_va++; g_sfence_last = va; }
void sv32_hw_sfence_all(void) { g_sfence_all++; }
void sv32_hw_pmp_open(void) { g_pmp_open = true; }
void sv32_hw_pmp_close(void) { g_pmp_open = false; }

void sv32_hw_panic(const char *msg)
{
	(void)msg;
	if (g_panic_armed)
		longjmp(g_panic_jmp, 1);
	printf("  [FAIL] unexpected panic: %s", msg);
	exit(1);
}

/* Leaf PTE the hardware walker would reach for @va on @cpu, 0 if none. */
static uint32_t walk(uint32_t cpu, uintptr_t va)
{
	uint32_t root = g_sv32_root[cpu][VPN1(va)];
	int i;

	if (!(root & PTE_V))
		return 0u;
	if (root & PTE_LEAF)
		return root;
	i = l2_find(&g_sv32_cpu[cpu], VPN1(va));
	if (i < 0 || root != pte_table(g_sv32_l2[cpu][i]))
		return 0xDEADu;
	return g_sv32_l2[cpu][i][VPN0(va)];
}

static bool identity(uint32_t pte, uintptr_t va)
{
	return (pte >> PTE_PPN_SHIFT) == (uint32_t)(va >> PAGE_SHIFT);
}

static ulmk_arch_region_t page(uintptr_t base, uint32_t perms)
{
	ulmk_arch_region_t r = { base, PAGE_SIZE, perms, 0u };

	return r;
}

static void reset_cpu(uint32_t cpu)
{
	g_cpu = cpu;
	g_satp_broken = false;
	g_pmp_open = false;
	ulmk_arch_mpu_init();
}

static void test_static_map(void)
{
	uint32_t pte;

	printf("TEST: static map, satp and PMP catch-all\n");
	reset_cpu(0u);

	CHECK(g_satp == (SATP_MODE_SV32 |
			 (uint32_t)((uintptr_t)g_sv32_root[0] >> PAGE_SHIFT)),
	      "satp = Sv32 | root PPN");
	CHECK(g_pmp_open, "PMP catch-all programmed");

	pte = walk(0u, 0x10000000u);
	CHECK(FLAGS(pte) == LEAF_RW && identity(pte, 0x10000000u),
	      "PERIPH megapage RW, identity");
	CHECK(walk(0u, 0x1FFFF000u) != 0u, "PERIPH last megapage mapped");
	CHECK(walk(0u, 0x20000000u) == 0u, "nothing above PERIPH");

	pte = walk(0u, 0x80001000u);
	CHECK(FLAGS(pte) == LEAF_RX && identity(pte, 0x80001000u),
	      "user text page RX");
	CHECK(FLAGS(walk(0u, 0x80002000u)) == LEAF_RX, "user text 2nd page");
	CHECK(walk(0u, 0x80003000u) == 0u, "page after user text unmapped");
	CHECK(walk(0u, 0x80000000u) == 0u, "kernel text below user text unmapped");

	CHECK(FLAGS(walk(0u, 0x80010000u)) == LEAF_RW, "user RAM RW");
	CHECK(FLAGS(walk(0u, 0x80011000u)) == LEAF_RW, "user RAM 2nd page");
	CHECK(walk(0u, 0x80012000u) == 0u, "heap pool not in static map");
	CHECK(g_sv32_cpu[0].l2_static == 1u, "one static level-2 table");
}

static void test_satp_rejected(void)
{
	printf("TEST: hart without Sv32 panics at init\n");
	g_cpu = 0u;
	g_satp_broken = true;
	g_panic_armed = true;
	if (setjmp(g_panic_jmp) == 0) {
		ulmk_arch_mpu_init();
		CHECK(0, "init returned with satp reading back 0");
	} else {
		CHECK(1, "satp readback mismatch panics");
	}
	g_panic_armed = false;
	g_satp_broken = false;
}

static void test_switch_pinned(void)
{
	ulmk_arch_region_t stk_a = { 0x80020000u, 0x2000u, RW, ULMK_REGION_STACK };
	ulmk_arch_region_t stk_b = { 0x80030000u, 0x1000u, RW, ULMK_REGION_STACK };
	ulmk_arch_region_t in_uram = { 0x80010800u, 0x400u, RW, ULMK_REGION_STACK };
	ulmk_arch_region_t w;
	unsigned n;

	printf("TEST: switch maps the pinned stack, drops the previous one\n");
	reset_cpu(0u);

	ulmk_arch_mpu_switch(&stk_a, 1u, ULMK_ARCH_PRS_USER);
	CHECK(FLAGS(walk(0u, 0x80020000u)) == LEAF_RW, "stack A page 0");
	CHECK(FLAGS(walk(0u, 0x80021000u)) == LEAF_RW, "stack A page 1");
	CHECK(walk(0u, 0x80022000u) == 0u, "past stack A unmapped");

	w = page(0x80040000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "thread A loads a heap page");

	n = g_sfence_all;
	ulmk_arch_mpu_switch(&stk_a, 1u, ULMK_ARCH_PRS_USER);
	CHECK(g_sfence_all == n, "same pinned list: no rebuild");
	CHECK(walk(0u, 0x80040000u) != 0u, "same thread keeps its pages");

	ulmk_arch_mpu_switch(NULL, 0u, ULMK_ARCH_PRS_KERNEL);
	CHECK(walk(0u, 0x80020000u) != 0u, "kernel thread leaves entries");
	ulmk_arch_mpu_switch(&stk_a, 1u, ULMK_ARCH_PRS_USER);
	CHECK(g_sfence_all == n + 1u, "after a kernel thread: rebuilt");
	CHECK(walk(0u, 0x80040000u) == 0u, "rebuild drops dynamic pages");
	CHECK(walk(0u, 0x80020000u) != 0u, "rebuild keeps the stack");

	ulmk_arch_mpu_switch(&stk_b, 1u, ULMK_ARCH_PRS_USER);
	CHECK(walk(0u, 0x80020000u) == 0u, "stack A gone on switch to B");
	CHECK(walk(0u, 0x80021000u) == 0u, "stack A page 1 gone");
	CHECK(FLAGS(walk(0u, 0x80030000u)) == LEAF_RW, "stack B mapped");

	ulmk_arch_mpu_switch(&in_uram, 1u, ULMK_ARCH_PRS_USER);
	CHECK(walk(0u, 0x80030000u) == 0u, "stack B gone");
	CHECK(g_sv32_cpu[0].pin[0].hi == 0u, "stack in user RAM is not pinned");
	CHECK(FLAGS(walk(0u, 0x80010000u)) == LEAF_RW, "user RAM untouched");
}

static void test_load(void)
{
	ulmk_arch_region_t stk = { 0x80020000u, 0x1000u, RW, ULMK_REGION_STACK };
	ulmk_arch_region_t w;
	uint32_t pte;

	printf("TEST: lazy load, one page per fault\n");
	reset_cpu(0u);
	ulmk_arch_mpu_switch(&stk, 1u, ULMK_ARCH_PRS_USER);

	w = page(0x80050000u, ULMK_PERM_READ);
	CHECK(ulmk_arch_mpu_load(&w), "RO page loads");
	pte = walk(0u, 0x80050000u);
	CHECK(FLAGS(pte) == (PTE_V | PTE_R | PTE_U | PTE_A | PTE_D) &&
	      identity(pte, 0x80050000u), "RO leaf, identity");
	CHECK(g_sfence_last == 0x80050000u, "sfence.vma on the loaded page");
	CHECK(walk(0u, 0x80051000u) == 0u, "neighbour page untouched");

	CHECK(!ulmk_arch_mpu_load(&w), "live page faulting again = violation");

	w = page(0x80060000u, ULMK_PERM_WRITE);
	CHECK(ulmk_arch_mpu_load(&w), "W-only area loads");
	CHECK(FLAGS(walk(0u, 0x80060000u)) == LEAF_RW, "W-only encoded as RW");

	w = page(0x80001000u, RW);
	CHECK(!ulmk_arch_mpu_load(&w), "write to user text = violation");
	w = page(0x80020000u, RW);
	CHECK(!ulmk_arch_mpu_load(&w), "pinned stack page = violation");
	w = page(0x10000000u, ULMK_PERM_EXEC);
	CHECK(!ulmk_arch_mpu_load(&w), "exec in PERIPH megapage = violation");

	w = page(0x80070000u, 0u);
	CHECK(!ulmk_arch_mpu_load(&w), "no perms rejected");
	w = page(0x80070800u, RW);
	CHECK(!ulmk_arch_mpu_load(&w), "unaligned window rejected");
	w.base = 0x80070000u;
	w.size = 2u * PAGE_SIZE;
	CHECK(!ulmk_arch_mpu_load(&w), "multi-page window rejected");
}

static void test_ring(void)
{
	ulmk_arch_region_t w;
	uint32_t i;

	printf("TEST: eviction ring recycles the oldest page\n");
	reset_cpu(0u);
	ulmk_arch_mpu_switch(NULL, 0u, ULMK_ARCH_PRS_USER);

	for (i = 0u; i < ULMK_ARCH_SV32_DYN_MAX; i++) {
		w = page(0x80100000u + i * PAGE_SIZE, RW);
		CHECK(ulmk_arch_mpu_load(&w), "fill the ring");
	}
	w = page(0x80200000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "one past the ring loads");
	CHECK(walk(0u, 0x80100000u) == 0u, "oldest page evicted");
	CHECK(walk(0u, 0x80101000u) != 0u, "second oldest kept");
	CHECK(walk(0u, 0x80200000u) != 0u, "newest mapped");

	w = page(0x80201000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "next load");
	CHECK(walk(0u, 0x80101000u) == 0u, "ring advanced to the next oldest");

	ulmk_arch_mpu_flush();
	CHECK(walk(0u, 0x80200000u) == 0u && walk(0u, 0x80201000u) == 0u &&
	      walk(0u, 0x80103000u) == 0u, "flush drops every dynamic page");
	CHECK(FLAGS(walk(0u, 0x80001000u)) == LEAF_RX, "flush keeps static map");
}

static void test_l2_pool(void)
{
	ulmk_arch_region_t stk = { 0x80020000u, 0x1000u, RW, ULMK_REGION_STACK };
	ulmk_arch_region_t shared_stk = { 0x90010000u, 0x1000u, RW,
					  ULMK_REGION_STACK };
	ulmk_arch_region_t w;

	printf("TEST: level-2 pool outside the static megapage\n");
	reset_cpu(0u);
	ulmk_arch_mpu_switch(&stk, 1u, ULMK_ARCH_PRS_USER);

	w = page(0x90000000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "SHARED page gets a dynamic table");
	CHECK(FLAGS(walk(0u, 0x90000000u)) == LEAF_RW, "SHARED page mapped");
	CHECK(g_sv32_cpu[0].l2_vpn1[1] == VPN1(0x90000000u), "table 1 taken");

	w = page(0xA0000000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "pool exhausted: drop and reload");
	CHECK(walk(0u, 0x90000000u) == 0u, "older table given back");
	CHECK(FLAGS(walk(0u, 0xA0000000u)) == LEAF_RW, "new page mapped");
	CHECK(FLAGS(walk(0u, 0x80020000u)) == LEAF_RW, "stack survives the drop");

	ulmk_arch_mpu_flush();
	CHECK(g_sv32_root[0][VPN1(0xA0000000u)] == 0u,
	      "flush clears the root entry of a dynamic table");
	CHECK(g_sv32_cpu[0].l2_vpn1[1] == L2_NONE, "flush frees the table");

	/* A stack outside the static megapage pins its table. */
	ulmk_arch_mpu_switch(&shared_stk, 1u, ULMK_ARCH_PRS_USER);
	CHECK(FLAGS(walk(0u, 0x90010000u)) == LEAF_RW, "SHARED stack mapped");
	ulmk_arch_mpu_flush();
	CHECK(FLAGS(walk(0u, 0x90010000u)) == LEAF_RW, "flush keeps pinned table");
	w = page(0x90020000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "page next to the stack shares its table");
	w = page(0xA0000000u, RW);
	CHECK(!ulmk_arch_mpu_load(&w), "every table pinned: load refused");
	CHECK(FLAGS(walk(0u, 0x90010000u)) == LEAF_RW, "stack still mapped");

	ulmk_arch_mpu_switch(&stk, 1u, ULMK_ARCH_PRS_USER);
	CHECK(g_sv32_cpu[0].l2_vpn1[1] == L2_NONE,
	      "switch away frees the pinned table");
	CHECK(walk(0u, 0x90010000u) == 0u, "old SHARED stack unmapped");
}

static void test_per_hart(void)
{
	ulmk_arch_region_t stk = { 0x80020000u, 0x1000u, RW, ULMK_REGION_STACK };
	ulmk_arch_region_t w;
	uint32_t satp0;

	printf("TEST: each hart owns its tables\n");
	reset_cpu(0u);
	satp0 = g_satp;
	reset_cpu(1u);
	CHECK(g_satp != satp0, "hart 1 root differs from hart 0");
	CHECK(FLAGS(walk(1u, 0x80001000u)) == LEAF_RX, "hart 1 static map");

	g_cpu = 0u;
	ulmk_arch_mpu_switch(&stk, 1u, ULMK_ARCH_PRS_USER);
	w = page(0x80080000u, RW);
	CHECK(ulmk_arch_mpu_load(&w), "load on hart 0");
	CHECK(walk(1u, 0x80080000u) == 0u, "hart 1 does not see it");
	CHECK(walk(1u, 0x80020000u) == 0u, "nor hart 0's stack");

	g_cpu = 1u;
	ulmk_arch_mpu_flush();
	CHECK(walk(0u, 0x80080000u) != 0u, "flush on hart 1 leaves hart 0");
}

static void test_disable(void)
{
	printf("TEST: disable turns translation and the catch-all off\n");
	reset_cpu(0u);
	ulmk_arch_mpu_disable();
	CHECK(g_satp == 0u, "satp bare");
	CHECK(!g_pmp_open, "PMP catch-all removed");
}

int main(void)
{
	test_static_map();
	test_satp_rejected();
	test_switch_pinned();
	test_load();
	test_ring();
	test_l2_pool();
	test_per_hart();
	test_disable();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
