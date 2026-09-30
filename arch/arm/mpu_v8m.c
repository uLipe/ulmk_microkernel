/* SPDX-License-Identifier: MIT */
/*
 * ARMv8-M MPU (PMSAv8) — arch/arm/mpu_v8m.c
 *
 * PMSAv8 uses base/limit register pairs with a 32-byte granule and no
 * power-of-two constraint, so kernel stack/heap regions map exactly.  Two MAIR
 * attribute sets are defined: index 0 = normal write-back, index 1 = device.
 * Privileged code keeps default access via PRIVDEFENA; only user-visible regions
 * are programmed (static user-text + MMIO, then per-thread dynamic regions).
 *
 * Compiled only for ARMv8-M builds; the file is empty when ULMK_ARCH_ARMV8M=0.
 */

#include <arch_config.h>

#if ULMK_ARCH_ARMV8M

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk_arch.h>

#define REG32(a)	(*(volatile uint32_t *)(uintptr_t)(a))

/* RBAR[4:3]=SH, [2:1]=AP, [0]=XN.  AP: 00=RW priv, 01=RW any, 10=RO priv, 11=RO any */
#define RBAR_AP_RW_ANY	(0x1u << 1)
#define RBAR_AP_RO_ANY	(0x3u << 1)
#define RBAR_XN		(1u << 0)
#define RBAR_SH_OUTER	(0x2u << 3)

/* RLAR[31:5]=LIMIT, [3:1]=AttrIndx, [0]=EN */
#define RLAR_EN		(1u << 0)
#define RLAR_ATTR_NORMAL	(0u << 1)	/* MAIR attr0: WB */
#define RLAR_ATTR_DEVICE	(1u << 1)	/* MAIR attr1: Device */
#define RLAR_ATTR_NORMAL_NC	(2u << 1)	/* MAIR attr2: Normal NC */

#define MAIR0_NORMAL_WB	0xFFu	/* attr0: normal, WB non-transient RW alloc */
#define MAIR0_DEVICE	0x00u	/* attr1: device nGnRnE */
#define MAIR0_NORMAL_NC	0x44u	/* attr2: normal non-cacheable */

static void region_disable(uint8_t slot)
{
	REG32(ULMK_ARCH_MPU_RNR)  = slot;
	REG32(ULMK_ARCH_MPU_RBAR) = 0u;
	REG32(ULMK_ARCH_MPU_RLAR) = 0u;
}

static void region_program(uint8_t slot, uintptr_t base, uintptr_t size,
			   uint32_t rbar_attr, uint32_t rlar_attr)
{
	uintptr_t limit;

	if (size == 0u) {
		region_disable(slot);
		return;
	}

	base  &= ~0x1Fu;
	limit  = (base + size - 1u) & ~0x1Fu;

	REG32(ULMK_ARCH_MPU_RNR)  = slot;
	REG32(ULMK_ARCH_MPU_RBAR) = (uint32_t)base | rbar_attr;
	REG32(ULMK_ARCH_MPU_RLAR) = ((uint32_t)limit & ~0x1Fu) | rlar_attr | RLAR_EN;
}

static uint32_t perm_to_rbar(uint32_t perms)
{
	uint32_t attr;

	attr = (perms & ULMK_PERM_WRITE) ? RBAR_AP_RW_ANY : RBAR_AP_RO_ANY;
	if (!(perms & ULMK_PERM_EXEC))
		attr |= RBAR_XN;
	attr |= RBAR_SH_OUTER;
	return attr;
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

	if (utext_hi > utext_lo)
		region_program(ULMK_ARCH_MPU_UTEXT, utext_lo, utext_hi - utext_lo,
			       RBAR_AP_RO_ANY | RBAR_SH_OUTER, RLAR_ATTR_NORMAL);
	else
		region_disable(ULMK_ARCH_MPU_UTEXT);

	/* User .data/.bss only; the pool above it is kernel heap (areas). */
	if (uram_hi > uram_lo)
		region_program(ULMK_ARCH_MPU_URAM, uram_lo, uram_hi - uram_lo,
			       RBAR_AP_RW_ANY | RBAR_XN | RBAR_SH_OUTER,
			       RLAR_ATTR_NORMAL);
	else
		region_disable(ULMK_ARCH_MPU_URAM);

	if (mmio_hi > mmio_lo)
		region_program(ULMK_ARCH_MPU_MMIO, mmio_lo, mmio_hi - mmio_lo,
			       RBAR_AP_RW_ANY | RBAR_XN, RLAR_ATTR_DEVICE);
	else
		region_disable(ULMK_ARCH_MPU_MMIO);
}

void ulmk_arch_mpu_init(void)
{
	uint8_t slot;

	/* Reconfigure with the MPU off (mpu_init runs again from kernel_main). */
	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) = 0u;
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	REG32(ULMK_ARCH_MPU_MAIR0) = ((uint32_t)MAIR0_NORMAL_NC << 16) |
				     ((uint32_t)MAIR0_DEVICE << 8) |
				     (uint32_t)MAIR0_NORMAL_WB;
	REG32(ULMK_ARCH_MPU_MAIR1) = 0u;

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

/*
 * PMSAv8 forbids overlapping enabled regions (an access that matches two is a
 * fault), unlike PMSAv7 where the higher-numbered region simply wins.  Pinned
 * regions inside a static window are already granted and must be skipped;
 * lazy windows never overlap one, since the fault that asks for them could
 * not have happened inside a static window.
 */
static bool covered_by_static(uintptr_t base, uintptr_t size)
{
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_start[];
	extern uintptr_t _ulmk_mem_periph_base[];
	extern uintptr_t _ulmk_mem_periph_end[];

	uintptr_t end = base + size;

	if (base >= (uintptr_t)_ulmk_user_ram_start &&
	    end  <= (uintptr_t)_ulmk_user_pool_start)
		return true;
	if (base >= (uintptr_t)_ulmk_mem_periph_base &&
	    end  <= (uintptr_t)_ulmk_mem_periph_end)
		return true;
	return false;
}

#define MPU_FREE_SLOTS	(((1u << ULMK_ARCH_MPU_REGIONS) - 1u) & \
			 ~((1u << ULMK_ARCH_MPU_UTEXT) | \
			   (1u << ULMK_ARCH_MPU_URAM) | \
			   (1u << ULMK_ARCH_MPU_MMIO)))

/*
 * Single core.  @g_mpu_pinned is the key: the TCB's own pinned list, so the
 * same thread coming back keeps its lazy windows.
 */
static const ulmk_arch_region_t *g_mpu_pinned;
static uint32_t g_mpu_dyn;
static uint8_t  g_mpu_next;

static uint32_t type_to_rlar(uint8_t type)
{
	if (type == ULMK_REGION_PERIPH)
		return RLAR_ATTR_DEVICE;
	if (type == ULMK_REGION_SHARED)
		return RLAR_ATTR_NORMAL_NC;
	return RLAR_ATTR_NORMAL;
}

static void mpu_sync(void)
{
	__asm__ volatile("dsb\n\tisb" ::: "memory");
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

	/* Only user-visible slots change; PRIVDEFENA keeps the kernel map. */
	for (slot = 0u; slot < ULMK_ARCH_MPU_REGIONS; slot++) {
		if (free & (1u << slot))
			region_disable(slot);
	}
	for (i = 0u; regions && i < count && free; i++) {
		if (regions[i].size == 0u ||
		    covered_by_static(regions[i].base, regions[i].size))
			continue;
		slot = (uint8_t)__builtin_ctz(free);
		free &= ~(1u << slot);
		region_program(slot, regions[i].base, regions[i].size,
			       perm_to_rbar(regions[i].perms),
			       type_to_rlar(regions[i].type));
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
	uint32_t rbar;
	uint8_t  n;
	uint8_t  slot;
	uint8_t  i;

	if (!g_mpu_dyn || win->size < 32u || (win->base & 0x1Fu) ||
	    (win->size & 0x1Fu))
		return false;

	/* The window is live yet the access faulted: a real violation. */
	for (m = g_mpu_dyn; m; m &= m - 1u) {
		REG32(ULMK_ARCH_MPU_RNR) = (uint32_t)__builtin_ctz(m);
		rbar = REG32(ULMK_ARCH_MPU_RBAR);
		if ((REG32(ULMK_ARCH_MPU_RLAR) & RLAR_EN) &&
		    (rbar & ~0x1Fu) == (uint32_t)win->base)
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
	region_program(slot, win->base, win->size, perm_to_rbar(win->perms),
		       type_to_rlar(win->type));
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

#endif /* ULMK_ARCH_ARMV8M */
