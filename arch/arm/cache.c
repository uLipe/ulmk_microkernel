/* SPDX-License-Identifier: MIT */
/*
 * Cortex-M7 L1 I/D-cache maintenance — arch/arm/cache.c
 *
 * CMSIS-compatible register recipe.  No-ops when ULMK_ARCH_HAS_CACHE=0
 * (QEMU boards without opt-in, or cores without D-cache).
 */
#include <stdint.h>
#include <stddef.h>
#include <ulmk_arch.h>
#include <arch_config.h>

#define REG32(a)	(*(volatile uint32_t *)(uintptr_t)(a))

#define SCB_CCR		0xE000ED14u
#define SCB_CSSELR	0xE000ED84u
#define SCB_CCSIDR	0xE000ED80u
#define SCB_ICIALLU	0xE000EF50u
#define SCB_DCISW	0xE000EF60u
#define SCB_DCCSW	0xE000EF68u
#define SCB_DCCISW	0xE000EF74u
#define SCB_DCIMVAC	0xE000EF5Cu
#define SCB_DCCMVAC	0xE000EF6Cu
#define SCB_DCCIMVAC	0xE000EF70u

#define SCB_CCR_IC	(1u << 17)
#define SCB_CCR_DC	(1u << 16)

#define CCSIDR_WAYS(x)	(((x) >> 3) & 0x3FFu)
#define CCSIDR_SETS(x)	(((x) >> 13) & 0x7FFFu)

#define DCACHE_LINE	32u

static void dsb_isb(void)
{
	__asm__ volatile("dsb" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}

#if ULMK_ARCH_HAS_CACHE

static void dcache_setway_op(uint32_t reg)
{
	uint32_t ccsidr;
	uint32_t sets;
	uint32_t ways;
	uint32_t s;
	uint32_t w;

	REG32(SCB_CSSELR) = 0u;
	dsb_isb();
	ccsidr = REG32(SCB_CCSIDR);
	sets = CCSIDR_SETS(ccsidr);
	do {
		s = sets;
		ways = CCSIDR_WAYS(ccsidr);
		do {
			w = ways;
			REG32(reg) = (s << 5) | (w << 30);
		} while (ways-- != 0u);
	} while (sets-- != 0u);
	dsb_isb();
}

static void dcache_invalidate_all(void)
{
	dcache_setway_op(SCB_DCISW);
}

static void dcache_clean_all(void)
{
	dcache_setway_op(SCB_DCCSW);
}

void ulmk_arch_cache_enable(void)
{
	/* I-cache */
	dsb_isb();
	REG32(SCB_ICIALLU) = 0u;
	dsb_isb();
	REG32(SCB_CCR) |= SCB_CCR_IC;
	dsb_isb();

	/* D-cache: invalidate then enable */
	dcache_invalidate_all();
	REG32(SCB_CCR) |= SCB_CCR_DC;
	dsb_isb();
}

void ulmk_arch_dcache_clean_all(void)
{
	if ((REG32(SCB_CCR) & SCB_CCR_DC) == 0u)
		return;
	dcache_clean_all();
}

void ulmk_arch_dcache_invalidate_all(void)
{
	if ((REG32(SCB_CCR) & SCB_CCR_DC) == 0u)
		return;
	dcache_invalidate_all();
}

void ulmk_arch_dcache_clean_invalidate_all(void)
{
	if ((REG32(SCB_CCR) & SCB_CCR_DC) == 0u)
		return;
	dcache_setway_op(SCB_DCCISW);
}

void ulmk_arch_icache_invalidate_all(void)
{
	if ((REG32(SCB_CCR) & SCB_CCR_IC) == 0u)
		return;
	dsb_isb();
	REG32(SCB_ICIALLU) = 0u;
	dsb_isb();
}

void ulmk_arch_dcache_clean(void *addr, size_t len)
{
	uintptr_t a;
	uintptr_t end;

	if ((REG32(SCB_CCR) & SCB_CCR_DC) == 0u)
		return;
	/* len == 0 → clean entire D-cache (cheap vs by-addr on a full FB). */
	if (len == 0u) {
		dcache_clean_all();
		return;
	}
	if (!addr)
		return;

	a = (uintptr_t)addr & ~(uintptr_t)(DCACHE_LINE - 1u);
	end = (uintptr_t)addr + len;
	dsb_isb();
	while (a < end) {
		REG32(SCB_DCCMVAC) = (uint32_t)a;
		a += DCACHE_LINE;
	}
	dsb_isb();
}

void ulmk_arch_dcache_invalidate(void *addr, size_t len)
{
	uintptr_t a;
	uintptr_t end;

	if (!addr || len == 0u)
		return;
	if ((REG32(SCB_CCR) & SCB_CCR_DC) == 0u)
		return;

	a = (uintptr_t)addr & ~(uintptr_t)(DCACHE_LINE - 1u);
	end = (uintptr_t)addr + len;
	dsb_isb();
	while (a < end) {
		REG32(SCB_DCIMVAC) = (uint32_t)a;
		a += DCACHE_LINE;
	}
	dsb_isb();
}

void ulmk_arch_dcache_clean_invalidate(void *addr, size_t len)
{
	uintptr_t a;
	uintptr_t end;

	if (!addr || len == 0u)
		return;
	if ((REG32(SCB_CCR) & SCB_CCR_DC) == 0u)
		return;

	a = (uintptr_t)addr & ~(uintptr_t)(DCACHE_LINE - 1u);
	end = (uintptr_t)addr + len;
	dsb_isb();
	while (a < end) {
		REG32(SCB_DCCIMVAC) = (uint32_t)a;
		a += DCACHE_LINE;
	}
	dsb_isb();
}

#else /* !ULMK_ARCH_HAS_CACHE */

void ulmk_arch_cache_enable(void)
{
}

void ulmk_arch_dcache_clean_all(void)
{
}

void ulmk_arch_dcache_invalidate_all(void)
{
}

void ulmk_arch_dcache_clean_invalidate_all(void)
{
}

void ulmk_arch_icache_invalidate_all(void)
{
}

void ulmk_arch_dcache_clean(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

void ulmk_arch_dcache_invalidate(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

void ulmk_arch_dcache_clean_invalidate(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

#endif /* ULMK_ARCH_HAS_CACHE */
