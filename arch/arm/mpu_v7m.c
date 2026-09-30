/* SPDX-License-Identifier: MIT */
/*
 * ARMv7-M MPU (PMSAv7) — arch/arm/mpu_v7m.c
 *
 * PMSAv7 regions must be a power-of-two in size and aligned to that size, so
 * arbitrary kernel stack/heap regions are rounded up (same scheme as the RISC-V
 * PMP NAPOT port).  Privileged code keeps default access via PRIVDEFENA, so only
 * user-visible regions are programmed: a static user-text + MMIO pair plus the
 * per-thread dynamic regions supplied by the scheduler.
 *
 * Compiled only for ARMv7-M builds; the file is empty when ULMK_ARCH_ARMV8M=1.
 */

#include <arch_config.h>

#if !ULMK_ARCH_ARMV8M

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk_arch.h>

#define REG32(a)	(*(volatile uint32_t *)(uintptr_t)(a))

#define RBAR_VALID	(1u << 4)

#define RASR_ENABLE	(1u << 0)
#define RASR_SRD_SHIFT	8u
#define RASR_XN		(1u << 28)
#define RASR_AP_RW_ANY	(0x3u << 24)
#define RASR_AP_RO_ANY	(0x6u << 24)
#define RASR_MEM_NORMAL	((0x1u << 19) | (1u << 18) | (1u << 17) | (1u << 16))
#define RASR_MEM_DEVICE	((1u << 18) | (1u << 16))
/*
 * MAP_SHARED (SDRAM / FB): Normal write-back, non-shareable (TEX=0 C=1 B=1).
 * Device attrs break LTDC AXI bursts.  Framebuffers need D-cache clean before
 * LTDC present (bus master bypasses the CPU cache).
 */
#define RASR_MEM_SHARED	((1u << 17) | (1u << 16))

static uint32_t perm_to_attr(uint32_t perms, uint8_t type)
{
	uint32_t attr;

	attr = (perms & ULMK_PERM_WRITE) ? RASR_AP_RW_ANY : RASR_AP_RO_ANY;
	if (!(perms & ULMK_PERM_EXEC))
		attr |= RASR_XN;
	if (type == ULMK_REGION_PERIPH)
		attr |= RASR_MEM_DEVICE;
	else if (type == ULMK_REGION_SHARED)
		attr |= RASR_MEM_SHARED;
	else
		attr |= RASR_MEM_NORMAL;
	return attr;
}
static uint32_t log2_cover(uintptr_t base, uintptr_t size, uintptr_t *out_base)
{
	uintptr_t end = base + size;
	uint32_t  l   = 5u;		/* 32-byte minimum region */
	uintptr_t rsize;
	uintptr_t rbase;

	for (;;) {
		rsize = (uintptr_t)1u << l;
		rbase = base & ~(rsize - 1u);
		if (rbase + rsize >= end || l >= 31u)
			break;
		l++;
	}
	*out_base = rbase;
	return l;
}

static void region_disable(uint8_t slot)
{
	REG32(ULMK_ARCH_MPU_RNR)  = slot;
	REG32(ULMK_ARCH_MPU_RBAR) = 0u;
	REG32(ULMK_ARCH_MPU_RASR) = 0u;
}

static void region_write(uint8_t slot, uintptr_t rbase, uint32_t l,
			 uint32_t attr)
{
	REG32(ULMK_ARCH_MPU_RNR)  = slot;
	REG32(ULMK_ARCH_MPU_RBAR) = (uint32_t)rbase | RBAR_VALID | slot;
	REG32(ULMK_ARCH_MPU_RASR) = RASR_ENABLE | ((l - 1u) << 1) | attr;
}

static void region_program(uint8_t slot, uintptr_t base, uintptr_t size,
			   uint32_t attr)
{
	uint32_t l;
	uintptr_t rbase;

	if (size == 0u) {
		region_disable(slot);
		return;
	}

	l = log2_cover(base, size, &rbase);
	region_write(slot, rbase, l, attr);
}

/*
 * Exact encoding of [lo, hi): the covering power-of-two region with the
 * subregions outside the range disabled.  Rounding outward instead would
 * hand user mode whatever kernel memory shares the block, so a range that
 * is not a whole number of subregions is refused; the chip linker input
 * (ULMK_USER_*_ALIGN) is what makes the static user ranges representable.
 */
static bool region_exact(uint8_t slot, uintptr_t lo, uintptr_t hi,
			 uint32_t attr)
{
	uintptr_t rbase;
	uintptr_t sub;
	uint32_t  l;
	uint32_t  first;
	uint32_t  last;
	uint32_t  srd;

	l = log2_cover(lo, hi - lo, &rbase);
	if (rbase == lo && hi - lo == ((uintptr_t)1u << l)) {
		region_write(slot, rbase, l, attr);
		return true;
	}
	/* Subregions exist only for regions of 256 bytes and up. */
	if (l < 8u)
		return false;
	sub = (uintptr_t)1u << (l - 3u);
	if (((lo - rbase) & (sub - 1u)) || ((hi - rbase) & (sub - 1u)))
		return false;
	first = (uint32_t)((lo - rbase) / sub);
	last  = (uint32_t)((hi - rbase) / sub);
	srd   = 0xFFu & ~(((1u << last) - 1u) & ~((1u << first) - 1u));
	region_write(slot, rbase, l, attr | (srd << RASR_SRD_SHIFT));
	return true;
}

static void dump_hex(const char *tag, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	while (*tag)
		ulmk_printk_char_out(*tag++);
	for (i = 28; i >= 0; i -= 4)
		ulmk_printk_char_out(hex[(v >> i) & 0xFu]);
}

static void dump_layout(uint8_t slot, uintptr_t lo, uintptr_t hi)
{
	dump_hex("MPU: static range not encodable slot=", slot);
	dump_hex(" lo=", (uint32_t)lo);
	dump_hex(" hi=", (uint32_t)hi);
	ulmk_printk_char_out('\n');
}

static void static_range(uint8_t slot, uintptr_t lo, uintptr_t hi,
			 uint32_t attr)
{
	if (hi <= lo) {
		region_disable(slot);
		return;
	}
	if (region_exact(slot, lo, hi, attr))
		return;
	dump_layout(slot, lo, hi);
	ulmk_kern_trap_panic();
}

static void program_static_user(void)
{
	extern uint8_t _ulmk_user_text_start[];
	extern uint8_t _ulmk_user_text_end[];
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_start[];
	extern uintptr_t _ulmk_mem_periph_base[];
	extern uintptr_t _ulmk_mem_periph_end[];

	uintptr_t utext_lo = (uintptr_t)_ulmk_user_text_start;
	uintptr_t utext_hi = (uintptr_t)_ulmk_user_text_end;
	uintptr_t uram_lo  = (uintptr_t)_ulmk_user_ram_start;
	uintptr_t uram_hi  = (uintptr_t)_ulmk_user_pool_start;
	uintptr_t mmio_lo  = (uintptr_t)_ulmk_mem_periph_base;
	uintptr_t mmio_hi  = (uintptr_t)_ulmk_mem_periph_end;

	static_range(ULMK_ARCH_MPU_UTEXT, utext_lo, utext_hi,
		     RASR_AP_RO_ANY | RASR_MEM_NORMAL);
	/* User .data/.bss only; the pool above it is kernel heap (areas). */
	static_range(ULMK_ARCH_MPU_URAM, uram_lo, uram_hi,
		     RASR_AP_RW_ANY | RASR_XN | RASR_MEM_NORMAL);

	if (mmio_hi > mmio_lo)
		region_program(ULMK_ARCH_MPU_MMIO, mmio_lo, mmio_hi - mmio_lo,
			       RASR_AP_RW_ANY | RASR_XN | RASR_MEM_DEVICE);
	else
		region_disable(ULMK_ARCH_MPU_MMIO);
}

void ulmk_arch_mpu_init(void)
{
	uint8_t slot;

	/*
	 * Reconfigure with the MPU off: mpu_init runs a second time from
	 * kernel_main after arch_init already enabled it, and reprogramming
	 * live regions from privileged code is not architecturally safe.
	 */
	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) = 0u;
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	for (slot = 0u; slot < ULMK_ARCH_MPU_REGIONS; slot++)
		region_disable(slot);

	program_static_user();

	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) = ULMK_ARCH_MPU_CTRL_ENABLE |
				    ULMK_ARCH_MPU_CTRL_PRIVDEFENA;
	__asm__ volatile("dsb\n\tisb" ::: "memory");
}

void ulmk_arch_mpu_enable(void)
{
	REG32(ULMK_ARCH_MPU_CTRL) |= ULMK_ARCH_MPU_CTRL_ENABLE;
	__asm__ volatile("dsb\n\tisb" ::: "memory");
}

void ulmk_arch_mpu_disable(void)
{
	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) &= ~ULMK_ARCH_MPU_CTRL_ENABLE;
	__asm__ volatile("dsb\n\tisb" ::: "memory");
}

void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			     uint8_t count)
{
	(void)prs;
	(void)regions;
	(void)count;
}

#define MPU_FREE_SLOTS	(((1u << ULMK_ARCH_MPU_REGIONS) - 1u) & \
			 ~((1u << ULMK_ARCH_MPU_UTEXT) | \
			   (1u << ULMK_ARCH_MPU_URAM) | \
			   (1u << ULMK_ARCH_MPU_MMIO)))

/*
 * Single core.  @g_mpu_pinned is the key: the TCB's own pinned list, so the
 * same thread coming back keeps its lazy windows.  Pinned and lazy regions
 * take the highest free slots so they win any overlap with a static one.
 */
static const ulmk_arch_region_t *g_mpu_pinned;
static uint32_t g_mpu_dyn;
static uint8_t  g_mpu_next;

static uint8_t top_slot(uint32_t m)
{
	return (uint8_t)(31u - (uint32_t)__builtin_clz(m));
}

static void mpu_sync(void)
{
	__asm__ volatile("dsb\n\tisb" ::: "memory");
}

static bool covered_by_uram(const ulmk_arch_region_t *r)
{
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_start[];

	return r->base >= (uintptr_t)_ulmk_user_ram_start &&
	       r->base + r->size <= (uintptr_t)_ulmk_user_pool_start;
}

void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			  uint8_t prs)
{
	uint32_t free = MPU_FREE_SLOTS;
	uint8_t  slot;
	uint8_t  i;

	/*
	 * Kernel threads run privileged under PRIVDEFENA and never touch user
	 * memory: leave the windows, but forget the owner so the next user
	 * thread (even the same one) is rebuilt against its current areas.
	 */
	if (prs == ULMK_ARCH_PRS_KERNEL) {
		g_mpu_pinned = NULL;
		return;
	}
	if (regions && regions == g_mpu_pinned)
		return;

	/*
	 * Keep MPU ENABLE + PRIVDEFENA while reprogramming.  Turning the MPU
	 * fully off with D-cache live requires a whole-cache clean/invalidate
	 * (stale memory types); doing that on every switch kills FB/WB
	 * performance.  Only user-visible slots change.
	 */
	for (slot = 0u; slot < ULMK_ARCH_MPU_REGIONS; slot++) {
		if (free & (1u << slot))
			region_disable(slot);
	}
	for (i = 0u; regions && i < count && free; i++) {
		if (regions[i].size == 0u || covered_by_uram(&regions[i]))
			continue;
		slot = top_slot(free);
		free &= ~(1u << slot);
		region_program(slot, regions[i].base, regions[i].size,
			       perm_to_attr(regions[i].perms, regions[i].type));
	}
	mpu_sync();

	g_mpu_pinned = regions;
	g_mpu_dyn    = free;
	g_mpu_next   = 0u;
}

void ulmk_arch_mpu_flush(void)
{
	uint32_t m;

	for (m = g_mpu_dyn; m; m &= m - 1u)
		region_disable((uint8_t)__builtin_ctz(m));
	mpu_sync();
}

bool ulmk_arch_mpu_load(const ulmk_arch_region_t *win)
{
	uint32_t m;
	uint8_t  n;
	uint8_t  slot;
	uint8_t  i;

	if (!g_mpu_dyn || win->size < 32u || (win->size & (win->size - 1u)) ||
	    (win->base & (win->size - 1u)))
		return false;

	/* The window is live yet the access faulted: a real violation. */
	for (m = g_mpu_dyn; m; m &= m - 1u) {
		REG32(ULMK_ARCH_MPU_RNR) = (uint32_t)__builtin_ctz(m);
		if ((REG32(ULMK_ARCH_MPU_RASR) & RASR_ENABLE) &&
		    (REG32(ULMK_ARCH_MPU_RBAR) & ~0x1Fu) == (uint32_t)win->base)
			return false;
	}

	n = (uint8_t)__builtin_popcount(g_mpu_dyn);
	m = g_mpu_dyn;
	for (i = 0u; i < g_mpu_next % n; i++)
		m &= m - 1u;
	slot = (uint8_t)__builtin_ctz(m);
	g_mpu_next = (uint8_t)((g_mpu_next + 1u) % n);

	/* Disable first: a half-written pair must never describe a region. */
	region_disable(slot);
	region_program(slot, win->base, win->size,
		       perm_to_attr(win->perms, win->type));
	mpu_sync();
	return true;
}

bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms)
{
	(void)addr;
	(void)size;
	(void)perms;
	return true;
}

#endif /* !ULMK_ARCH_ARMV8M */
