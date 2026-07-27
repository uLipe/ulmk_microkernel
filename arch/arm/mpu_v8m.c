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
	extern uint8_t _ulmk_user_pool_end[];
	extern uintptr_t _ulmk_mem_periph_base[];
	extern uintptr_t _ulmk_mem_periph_end[];

	uintptr_t utext_lo = (uintptr_t)_ulmk_user_text_start;
	uintptr_t utext_hi = (uintptr_t)_ulmk_user_text_end;
	uintptr_t uram_lo  = (uintptr_t)_ulmk_user_ram_start;
	uintptr_t uram_hi  = (uintptr_t)_ulmk_user_pool_end;
	uintptr_t mmio_lo  = (uintptr_t)_ulmk_mem_periph_base;
	uintptr_t mmio_hi  = (uintptr_t)_ulmk_mem_periph_end;

	if (utext_hi > utext_lo)
		region_program(ULMK_ARCH_MPU_UTEXT, utext_lo, utext_hi - utext_lo,
			       RBAR_AP_RO_ANY | RBAR_SH_OUTER, RLAR_ATTR_NORMAL);
	else
		region_disable(ULMK_ARCH_MPU_UTEXT);

	/* Shared user data/bss + heap pool: RW, no-execute, all user threads. */
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
 * fault), unlike PMSAv7 where the higher-numbered region simply wins.  The
 * static user RAM / MMIO regions already blanket the whole user pool and
 * peripheral window, so any per-thread region contained within them would
 * overlap.  Skip those (they are already granted); only program regions that
 * fall outside the static windows.
 */
static bool covered_by_static(uintptr_t base, uintptr_t size)
{
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_end[];
	extern uintptr_t _ulmk_mem_periph_base[];
	extern uintptr_t _ulmk_mem_periph_end[];

	uintptr_t end = base + size;

	if (base >= (uintptr_t)_ulmk_user_ram_start &&
	    end  <= (uintptr_t)_ulmk_user_pool_end)
		return true;
	if (base >= (uintptr_t)_ulmk_mem_periph_base &&
	    end  <= (uintptr_t)_ulmk_mem_periph_end)
		return true;
	return false;
}

static const ulmk_arch_region_t *g_mpu_regions;
static uint8_t g_mpu_count;
static uint8_t g_mpu_prs = 0xFFu;
static uint8_t g_mpu_dyn; /* dynamic slots last programmed */

static uint8_t mpu_dyn_needed(const ulmk_arch_region_t *regions, uint8_t count)
{
	uint8_t n = 0u;
	uint8_t i;

	if (!regions)
		return 0u;
	for (i = 0u; i < count; i++) {
		if (regions[i].type == ULMK_REGION_STACK)
			continue;
		if (regions[i].size == 0u ||
		    covered_by_static(regions[i].base, regions[i].size))
			continue;
		n++;
	}
	return n;
}

void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			  uint8_t prs)
{
	uint8_t slot;
	uint8_t i;
	uint8_t eff;

	if (prs == g_mpu_prs && regions == g_mpu_regions && count == g_mpu_count)
		return;

	eff = (prs != ULMK_ARCH_PRS_KERNEL) ? mpu_dyn_needed(regions, count) : 0u;

	/* No unique dynamic regions — static windows already grant access. */
	if (prs == g_mpu_prs && eff == 0u && g_mpu_dyn == 0u &&
	    prs != ULMK_ARCH_PRS_KERNEL) {
		g_mpu_regions = regions;
		g_mpu_count   = count;
		return;
	}

	/* Reprogram with the MPU off so the update is atomic w.r.t. running code. */
	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) = 0u;
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	program_static_user();

	slot = ULMK_ARCH_MPU_USER_BASE;
	if (prs != ULMK_ARCH_PRS_KERNEL && regions) {
		for (i = 0u; i < count && slot < ULMK_ARCH_MPU_REGIONS; i++) {
			if (regions[i].type == ULMK_REGION_STACK)
				continue;
			if (regions[i].size == 0u ||
			    covered_by_static(regions[i].base, regions[i].size))
				continue;
			{
				uint32_t rlar;

				if (regions[i].type == ULMK_REGION_PERIPH)
					rlar = RLAR_ATTR_DEVICE;
				else if (regions[i].type == ULMK_REGION_SHARED)
					rlar = RLAR_ATTR_NORMAL_NC;
				else
					rlar = RLAR_ATTR_NORMAL;
				region_program(slot, regions[i].base,
					       regions[i].size,
					       perm_to_rbar(regions[i].perms),
					       rlar);
			}
			slot++;
		}
	}
	eff = (uint8_t)(slot - ULMK_ARCH_MPU_USER_BASE);
	for (; slot < ULMK_ARCH_MPU_REGIONS; slot++)
		region_disable(slot);

	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) = ULMK_ARCH_MPU_CTRL_ENABLE |
				    ULMK_ARCH_MPU_CTRL_PRIVDEFENA;
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	g_mpu_prs     = prs;
	g_mpu_regions = regions;
	g_mpu_count   = count;
	g_mpu_dyn     = eff;
}

bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms)
{
	(void)addr;
	(void)size;
	(void)perms;
	return true;
}

#endif /* ULMK_ARCH_ARMV8M */
