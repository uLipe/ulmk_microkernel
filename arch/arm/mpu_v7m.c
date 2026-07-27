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
#define RASR_XN		(1u << 28)
#define RASR_AP_RW_ANY	(0x3u << 24)
#define RASR_AP_RO_ANY	(0x6u << 24)
#define RASR_MEM_NORMAL	((0x1u << 19) | (1u << 18) | (1u << 17) | (1u << 16))
#define RASR_MEM_DEVICE	((1u << 18) | (1u << 16))
/*
 * MAP_SHARED (SDRAM / FB): Normal, write-back, non-shareable (TEX=0 C=1 B=1
 * S=0) — Cube/LTDC recipe.  Device attrs break LTDC AXI bursts (streaks /
 * flicker).  D-cache stays off in this port, so WB is still CPU-coherent.
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

	REG32(ULMK_ARCH_MPU_RNR)  = slot;
	REG32(ULMK_ARCH_MPU_RBAR) = (uint32_t)rbase | RBAR_VALID | slot;
	REG32(ULMK_ARCH_MPU_RASR) = RASR_ENABLE | ((l - 1u) << 1) | attr;
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
			       RASR_AP_RO_ANY | RASR_MEM_NORMAL);
	else
		region_disable(ULMK_ARCH_MPU_UTEXT);

	/* Shared user data/bss + heap pool: RW, no-execute, all user threads. */
	if (uram_hi > uram_lo)
		region_program(ULMK_ARCH_MPU_URAM, uram_lo, uram_hi - uram_lo,
			       RASR_AP_RW_ANY | RASR_XN | RASR_MEM_NORMAL);
	else
		region_disable(ULMK_ARCH_MPU_URAM);

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

static const ulmk_arch_region_t *g_mpu_regions;
static uint8_t g_mpu_count;
static uint8_t g_mpu_prs = 0xFFu;
static uint8_t g_mpu_dyn; /* non-STACK dynamic slots last programmed */

static uint8_t mpu_dyn_count(const ulmk_arch_region_t *regions, uint8_t count)
{
	uint8_t n = 0u;
	uint8_t i;

	if (!regions)
		return 0u;
	for (i = 0u; i < count; i++) {
		if (regions[i].type != ULMK_REGION_STACK)
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

	eff = (prs != ULMK_ARCH_PRS_KERNEL) ? mpu_dyn_count(regions, count) : 0u;

	/*
	 * STACK is inside the static URAM window.  Stack-only address spaces
	 * share that window — avoid tear-down/reprogram on every IPC switch.
	 */
	if (prs == g_mpu_prs && eff == 0u && g_mpu_dyn == 0u &&
	    prs != ULMK_ARCH_PRS_KERNEL) {
		g_mpu_regions = regions;
		g_mpu_count   = count;
		return;
	}

	/*
	 * Reprogram with the MPU off.  A per-thread region's power-of-two
	 * round-up (log2_cover) can transiently over-cover the kernel .text
	 * that this very code executes from; with the MPU live that raises an
	 * IACCVIOL mid-switch.  Disabling first makes the update atomic w.r.t.
	 * the running privileged code.
	 */
	__asm__ volatile("dsb" ::: "memory");
	REG32(ULMK_ARCH_MPU_CTRL) = 0u;
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	program_static_user();

	slot = ULMK_ARCH_MPU_USER_BASE;
	if (prs != ULMK_ARCH_PRS_KERNEL && regions) {
		for (i = 0u; i < count && slot < ULMK_ARCH_MPU_REGIONS; i++) {
			if (regions[i].type == ULMK_REGION_STACK)
				continue;
			region_program(slot, regions[i].base, regions[i].size,
				       perm_to_attr(regions[i].perms,
						    regions[i].type));
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

#endif /* !ULMK_ARCH_ARMV8M */
