/* SPDX-License-Identifier: MIT */
/*
 * RISC-V Sv32 memory protection — arch/riscv/mmu_sv32.c
 *
 * ulmk_arch_mpu_* backend for ULMK_CONFIG_MMU=1; mpu_pmp.c is the PMP
 * alternative.  Protection only: every mapping is the identity, the kernel
 * runs untranslated in M-mode and keeps using user pointers as they are.
 * The page tables filter what U-mode may touch, like PMP entries do, but
 * without a slot limit.
 *
 * Each hart owns a root table, loaded into satp once, and a few level-2
 * tables.  The static user map (user text RX, user RAM RW, PERIPH RW) is
 * built at init and never changes; the pinned stack is mapped on a switch;
 * every other page a thread maps is loaded on its first fault, kept in a
 * ring and dropped on flush.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <ulmk_arch.h>

#define PAGE_SHIFT	12u
#define PAGE_SIZE	(1u << PAGE_SHIFT)
#define MEGA_SHIFT	22u
#define MEGA_SIZE	(1u << MEGA_SHIFT)
#define PTES		1024u

#define PTE_V		(1u << 0)
#define PTE_R		(1u << 1)
#define PTE_W		(1u << 2)
#define PTE_X		(1u << 3)
#define PTE_U		(1u << 4)
#define PTE_A		(1u << 6)
#define PTE_D		(1u << 7)
#define PTE_LEAF	(PTE_R | PTE_W | PTE_X)
#define PTE_PPN_SHIFT	10u

#define SATP_MODE_SV32	(1u << 31)

#define VPN1(va)	((uint32_t)(va) >> MEGA_SHIFT)
#define VPN0(va)	(((uint32_t)(va) >> PAGE_SHIFT) & (PTES - 1u))

#define L2_NONE		0xFFFFu

/*
 * Kernel stacks and areas are heap blocks; the stack is the only pinned
 * region the kernel hands over today, a second slot is headroom.
 */
#define SV32_PIN_MAX	2u

#if ULMK_CONFIG_ENABLE_SMP
#define SV32_NCPU	ULMK_ARCH_NUM_CPU
#else
#define SV32_NCPU	1
#endif

#define PMP_NAPOT_RWX	0x1Fu

#if ULMK_ARCH_SV32_L2_NUM > 32 || ULMK_ARCH_SV32_DYN_MAX > 255
#error "ULMK_ARCH_SV32_L2_NUM <= 32 and ULMK_ARCH_SV32_DYN_MAX <= 255"
#endif

#ifdef UL_UNIT_TEST
void sv32_hw_satp_write(uint32_t val);
uint32_t sv32_hw_satp_read(void);
void sv32_hw_sfence(uintptr_t va);
void sv32_hw_sfence_all(void);
void sv32_hw_pmp_open(void);
void sv32_hw_pmp_close(void);
void sv32_hw_panic(const char *msg);
#else
static inline void sv32_hw_satp_write(uint32_t val)
{
	__asm__ volatile("csrw satp, %0" :: "r"(val) : "memory");
}

static inline uint32_t sv32_hw_satp_read(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, satp" : "=r"(val));
	return val;
}

static inline void sv32_hw_sfence(uintptr_t va)
{
	__asm__ volatile("sfence.vma %0, zero" :: "r"(va) : "memory");
}

static inline void sv32_hw_sfence_all(void)
{
	__asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

/*
 * U-mode accesses and the walker's own reads are still PMP-checked, and
 * with no matching entry they fail.  One all-ones NAPOT entry lets both
 * through and leaves the filtering to the page tables.
 */
static inline void sv32_hw_pmp_open(void)
{
	__asm__ volatile("csrw pmpcfg0, zero");
	__asm__ volatile("csrw pmpaddr0, %0" :: "r"(0xFFFFFFFFu));
	__asm__ volatile("csrw pmpcfg0, %0" :: "r"(PMP_NAPOT_RWX));
}

static inline void sv32_hw_pmp_close(void)
{
	__asm__ volatile("csrw pmpcfg0, zero");
}

static void sv32_hw_panic(const char *msg)
{
	while (*msg)
		ulmk_printk_char_out(*msg++);
	for (;;)
		ulmk_arch_cpu_halt();
}
#endif

extern uint8_t _ulmk_user_text_start[];
extern uint8_t _ulmk_user_text_end[];
extern uint8_t _ulmk_user_ram_start[];
extern uint8_t _ulmk_user_pool_start[];
extern uintptr_t _ulmk_mem_periph_base[];
extern uintptr_t _ulmk_mem_periph_end[];

struct sv32_range {
	uintptr_t lo;
	uintptr_t hi;
};

/*
 * @pinned is the TCB's own pinned list, as on the PMP backend: the same
 * thread coming back keeps its pages, any other thread gets a fresh map.
 * Level-2 tables [0, l2_static) hold the static map and are never given
 * back; l2_pin marks the dynamic ones holding pinned pages.
 */
struct sv32_cpu {
	const ulmk_arch_region_t *pinned;
	struct sv32_range pin[SV32_PIN_MAX];
	uint32_t dyn[ULMK_ARCH_SV32_DYN_MAX];
	uint16_t l2_vpn1[ULMK_ARCH_SV32_L2_NUM];
	uint32_t l2_pin;
	uint8_t  l2_static;
	uint8_t  dyn_n;
	uint8_t  dyn_next;
};

static uint32_t g_sv32_root[SV32_NCPU][PTES]
	__attribute__((aligned(PAGE_SIZE)));
static uint32_t g_sv32_l2[SV32_NCPU][ULMK_ARCH_SV32_L2_NUM][PTES]
	__attribute__((aligned(PAGE_SIZE)));
static struct sv32_cpu g_sv32_cpu[SV32_NCPU];

static uint32_t sv32_cpu_idx(void)
{
	uint32_t cpu = ulmk_arch_cpu_id();

	return cpu < (uint32_t)SV32_NCPU ? cpu : 0u;
}

static uint32_t perms_to_pte(uint32_t perms)
{
	uint32_t p = 0u;

	/* W without R is a reserved encoding. */
	if (perms & (ULMK_PERM_READ | ULMK_PERM_WRITE))
		p |= PTE_R;
	if (perms & ULMK_PERM_WRITE)
		p |= PTE_W;
	if (perms & ULMK_PERM_EXEC)
		p |= PTE_X;
	return p;
}

/* A/D preset: nothing here takes access/dirty faults. */
static uint32_t pte_leaf(uintptr_t pa, uint32_t pte_perms)
{
	return ((uint32_t)(pa >> PAGE_SHIFT) << PTE_PPN_SHIFT) | pte_perms |
	       PTE_U | PTE_A | PTE_D | PTE_V;
}

static uint32_t pte_table(const uint32_t *tbl)
{
	return ((uint32_t)((uintptr_t)tbl >> PAGE_SHIFT) << PTE_PPN_SHIFT) |
	       PTE_V;
}

static int l2_find(const struct sv32_cpu *c, uint32_t vpn1)
{
	uint32_t i;

	for (i = 0u; i < ULMK_ARCH_SV32_L2_NUM; i++) {
		if (c->l2_vpn1[i] == vpn1)
			return (int)i;
	}
	return -1;
}

static int l2_alloc(uint32_t cpu, uint32_t vpn1)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];
	uint32_t *tbl;
	uint32_t i;
	uint32_t j;

	if (g_sv32_root[cpu][vpn1] & PTE_V)
		return -1;
	for (i = c->l2_static; i < ULMK_ARCH_SV32_L2_NUM; i++) {
		if (c->l2_vpn1[i] != L2_NONE)
			continue;
		tbl = g_sv32_l2[cpu][i];
		for (j = 0u; j < PTES; j++)
			tbl[j] = 0u;
		c->l2_vpn1[i] = (uint16_t)vpn1;
		g_sv32_root[cpu][vpn1] = pte_table(tbl);
		return (int)i;
	}
	return -1;
}

static void l2_release(uint32_t cpu, uint32_t i)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];

	g_sv32_root[cpu][c->l2_vpn1[i]] = 0u;
	c->l2_vpn1[i] = L2_NONE;
	c->l2_pin &= ~(1u << i);
}

/* Leaf slot for @va, or NULL when a megapage (or nothing) covers it. */
static uint32_t *pte_slot(uint32_t cpu, uintptr_t va, bool alloc, int *l2)
{
	int i;

	i = l2_find(&g_sv32_cpu[cpu], VPN1(va));
	if (i < 0 && alloc)
		i = l2_alloc(cpu, VPN1(va));
	if (i < 0)
		return NULL;
	if (l2)
		*l2 = i;
	return &g_sv32_l2[cpu][i][VPN0(va)];
}

static void unmap_page(uint32_t cpu, uintptr_t va)
{
	uint32_t *pte = pte_slot(cpu, va, false, NULL);

	if (pte)
		*pte = 0u;
}

static void static_map(uint32_t cpu, uintptr_t lo, uintptr_t hi,
		       uint32_t perms)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];
	uint32_t *pte;
	uint32_t pp = perms_to_pte(perms);

	lo = (lo + PAGE_SIZE - 1u) & ~(uintptr_t)(PAGE_SIZE - 1u);
	hi &= ~(uintptr_t)(PAGE_SIZE - 1u);

	while (lo < hi) {
		if (!(lo & (MEGA_SIZE - 1u)) && hi - lo >= MEGA_SIZE &&
		    !(g_sv32_root[cpu][VPN1(lo)] & PTE_V)) {
			g_sv32_root[cpu][VPN1(lo)] = pte_leaf(lo, pp);
			lo += MEGA_SIZE;
			continue;
		}
		if (l2_find(c, VPN1(lo)) < 0) {
			if (l2_alloc(cpu, VPN1(lo)) < 0)
				sv32_hw_panic("sv32: static map needs more "
					      "ULMK_ARCH_SV32_L2_NUM\n");
			c->l2_static++;
		}
		pte = pte_slot(cpu, lo, false, NULL);
		*pte = pte_leaf(lo, pp);
		lo += PAGE_SIZE;
	}
}

static void static_user(uint32_t cpu)
{
	static_map(cpu, (uintptr_t)_ulmk_user_text_start,
		   (uintptr_t)_ulmk_user_text_end,
		   ULMK_PERM_READ | ULMK_PERM_EXEC);
	/* The pool above user .data/.bss is kernel heap: areas only. */
	static_map(cpu, (uintptr_t)_ulmk_user_ram_start,
		   (uintptr_t)_ulmk_user_pool_start,
		   ULMK_PERM_READ | ULMK_PERM_WRITE);
	static_map(cpu, (uintptr_t)_ulmk_mem_periph_base,
		   (uintptr_t)_ulmk_mem_periph_end,
		   ULMK_PERM_READ | ULMK_PERM_WRITE);
}

static bool covered_by_uram(const ulmk_arch_region_t *r)
{
	return r->base >= (uintptr_t)_ulmk_user_ram_start &&
	       r->base + r->size <= (uintptr_t)_ulmk_user_pool_start;
}

static void dyn_drop(uint32_t cpu)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];
	uint32_t i;

	for (i = 0u; i < c->dyn_n; i++)
		unmap_page(cpu, c->dyn[i]);
	c->dyn_n = 0u;
	c->dyn_next = 0u;
	for (i = c->l2_static; i < ULMK_ARCH_SV32_L2_NUM; i++) {
		if (c->l2_vpn1[i] != L2_NONE && !(c->l2_pin & (1u << i)))
			l2_release(cpu, i);
	}
}

static void dyn_push(uint32_t cpu, uintptr_t va)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];

	if (c->dyn_n < ULMK_ARCH_SV32_DYN_MAX) {
		c->dyn[c->dyn_n++] = (uint32_t)va;
		return;
	}
	unmap_page(cpu, c->dyn[c->dyn_next]);
	sv32_hw_sfence(c->dyn[c->dyn_next]);
	c->dyn[c->dyn_next] = (uint32_t)va;
	c->dyn_next = (uint8_t)((c->dyn_next + 1u) % ULMK_ARCH_SV32_DYN_MAX);
}

static void pin_drop(uint32_t cpu)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];
	uintptr_t va;
	uint32_t i;

	for (i = 0u; i < SV32_PIN_MAX; i++) {
		for (va = c->pin[i].lo; va < c->pin[i].hi; va += PAGE_SIZE)
			unmap_page(cpu, va);
		c->pin[i].lo = 0u;
		c->pin[i].hi = 0u;
	}
	c->l2_pin = 0u;
}

static void pin_map(uint32_t cpu, uint32_t n, const ulmk_arch_region_t *r)
{
	struct sv32_cpu *c = &g_sv32_cpu[cpu];
	uint32_t *pte;
	uintptr_t va;
	uintptr_t hi;
	uint32_t pp = perms_to_pte(r->perms);
	int l2;

	va = r->base & ~(uintptr_t)(PAGE_SIZE - 1u);
	hi = (r->base + r->size + PAGE_SIZE - 1u) &
	     ~(uintptr_t)(PAGE_SIZE - 1u);
	c->pin[n].lo = va;
	for (; va < hi; va += PAGE_SIZE) {
		pte = pte_slot(cpu, va, true, &l2);
		if (!pte)
			break;
		*pte = pte_leaf(va, pp);
		if ((uint32_t)l2 >= c->l2_static)
			c->l2_pin |= 1u << l2;
	}
	c->pin[n].hi = va;
}

static void user_layout(uint32_t cpu, const ulmk_arch_region_t *pinned,
			uint8_t count)
{
	uint32_t n = 0u;
	uint8_t  i;

	pin_drop(cpu);
	dyn_drop(cpu);

	for (i = 0u; pinned && i < count && n < SV32_PIN_MAX; i++) {
		if (!pinned[i].size || covered_by_uram(&pinned[i]))
			continue;
		pin_map(cpu, n++, &pinned[i]);
	}
	sv32_hw_sfence_all();
}

void ulmk_arch_mpu_init(void)
{
	uint32_t cpu = sv32_cpu_idx();
	struct sv32_cpu *c = &g_sv32_cpu[cpu];
	uint32_t satp;
	uint32_t i;

	sv32_hw_satp_write(0u);
	for (i = 0u; i < PTES; i++)
		g_sv32_root[cpu][i] = 0u;
	for (i = 0u; i < ULMK_ARCH_SV32_L2_NUM; i++)
		c->l2_vpn1[i] = L2_NONE;
	for (i = 0u; i < SV32_PIN_MAX; i++) {
		c->pin[i].lo = 0u;
		c->pin[i].hi = 0u;
	}
	c->pinned = NULL;
	c->l2_pin = 0u;
	c->l2_static = 0u;
	c->dyn_n = 0u;
	c->dyn_next = 0u;

	static_user(cpu);
	sv32_hw_pmp_open();

	satp = SATP_MODE_SV32 |
	       (uint32_t)((uintptr_t)g_sv32_root[cpu] >> PAGE_SHIFT);
	sv32_hw_satp_write(satp);
	/* satp is WARL: a hart without Sv32 reads the write back as zero. */
	if (sv32_hw_satp_read() != satp)
		sv32_hw_panic("sv32: satp rejected, hart has no Sv32\n");
	sv32_hw_sfence_all();
}

void ulmk_arch_mpu_enable(void)
{
}

void ulmk_arch_mpu_disable(void)
{
	sv32_hw_satp_write(0u);
	sv32_hw_pmp_close();
	sv32_hw_sfence_all();
}

void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			   uint8_t count)
{
	(void)prs;
	(void)regions;
	(void)count;
}

void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			uint8_t prs)
{
	uint32_t cpu = sv32_cpu_idx();
	struct sv32_cpu *c = &g_sv32_cpu[cpu];

	/* M-mode is untranslated: see the PMP backend for the reasoning. */
	if (prs == ULMK_ARCH_PRS_KERNEL) {
		c->pinned = NULL;
		return;
	}
	if (c->pinned == regions && regions)
		return;

	user_layout(cpu, regions, count);
	c->pinned = regions;
}

void ulmk_arch_mpu_flush(void)
{
	dyn_drop(sv32_cpu_idx());
	sv32_hw_sfence_all();
}

bool ulmk_arch_mpu_load(const ulmk_arch_region_t *win)
{
	uint32_t cpu = sv32_cpu_idx();
	uint32_t *pte;
	uint32_t pp = perms_to_pte(win->perms);

	if (!pp || win->size != PAGE_SIZE || (win->base & (PAGE_SIZE - 1u)))
		return false;
	/* Static megapage: the access is outside what it grants. */
	if (g_sv32_root[cpu][VPN1(win->base)] & PTE_LEAF)
		return false;

	pte = pte_slot(cpu, win->base, true, NULL);
	if (!pte) {
		/* Out of level-2 tables: start over from the pinned set. */
		dyn_drop(cpu);
		sv32_hw_sfence_all();
		pte = pte_slot(cpu, win->base, true, NULL);
		if (!pte)
			return false;
	}
	/* The page is live yet the access faulted: a real violation. */
	if (*pte & PTE_V)
		return false;

	*pte = pte_leaf(win->base, pp);
	dyn_push(cpu, win->base);
	sv32_hw_sfence(win->base);
	return true;
}

bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms)
{
	(void)addr;
	(void)size;
	(void)perms;
	return true;
}
