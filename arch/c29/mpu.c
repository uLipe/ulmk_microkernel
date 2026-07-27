/* SPDX-License-Identifier: MIT */
/*
 * C29 SSU / APR — dynamic region programming for the current thread.
 *
 * SSUMODE1 + Link2 AP override remains for bring-up until a CRC-valid SECCFG
 * boots SSUMODE2 (Gate D).  Dynamic APR slots are still programmed so the
 * shadow used by ulmk_arch_mpu_addr_permitted() matches the TCB region list.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <ulmk_arch.h>

#define MMIO32(a)	(*(volatile uint32_t *)(uintptr_t)(a))

#define SSU_O_MODE			0x08u
#define SSU_O_LINK2_AP_OVERRIDE		0x0Cu
#define SSU_AP_STRIDE			0x20u
#define SSU_AP_CFG_APD			(1u << 6)
#define SSU_AP_CFG_XE			(1u << 7)
#define SSU_LINK_RW(link)		(3u << ((link) * 2u))
#define SSU_LINK_RD(link)		(1u << ((link) * 2u))
#define SSU_EXE_LINK_RUNTIME		1u	/* common runtime Link */
#define SSU_DYN_APR_BASE		8u	/* reserve 0-7 for static */

static ulmk_arch_region_t g_active_regions[ULMK_ARCH_MAX_REGIONS];
static uint8_t g_active_count;
static uint8_t g_ssu_enforcing;
static uint8_t g_ssu_mode2;

static uint32_t ssu_ap_base(void)
{
	uint32_t cpu = ulmk_arch_cpu_id();

	return ULMK_BOARD_SSUCPU1AP_BASE + (cpu * 0x1000u);
}

static void apr_disable(uint32_t ap_base, uint8_t region)
{
	uint32_t cfg_addr = ap_base + (region * SSU_AP_STRIDE);

	MMIO32(cfg_addr) |= SSU_AP_CFG_APD;
}

static void apr_program(uint32_t ap_base, uint8_t region,
			uintptr_t start, uintptr_t end,
			uint32_t access, bool executable)
{
	uint32_t cfg_addr = ap_base + (region * SSU_AP_STRIDE);
	uint32_t cfg;

	/* Align to 4 KiB APR rules. */
	start &= ~(uintptr_t)0xFFFu;
	end |= (uintptr_t)0xFFFu;

	apr_disable(ap_base, region);
	MMIO32(cfg_addr + 0x04u) = (uint32_t)start;
	MMIO32(cfg_addr + 0x08u) = (uint32_t)end;
	MMIO32(cfg_addr + 0x14u) = access;

	cfg = (uint32_t)SSU_EXE_LINK_RUNTIME;
	if (executable)
		cfg |= SSU_AP_CFG_XE;
	/* Clear APD last — enable the slot. */
	MMIO32(cfg_addr) = cfg;
}

void ulmk_arch_mpu_init(void)
{
	uint32_t mode;

	g_active_count = 0u;
	g_ssu_enforcing = 0u;
	mode = MMIO32(ULMK_BOARD_SSUGEN_BASE + SSU_O_MODE) & 0x3Fu;
	g_ssu_mode2 = (mode == 0x0Cu) ? 1u : 0u;

	/*
	 * Keep Link2 override while SSUMODE1 (or unknown).  Clear it only
	 * after SSUMODE2 is confirmed and static APRs are installed.
	 */
	if (!g_ssu_mode2) {
		MMIO32(ULMK_BOARD_SSUGEN_BASE + SSU_O_LINK2_AP_OVERRIDE) |=
			(1u << 0) | (1u << 1) | (1u << 2);
	}
}

static void ssu_install_static_aprs(uint32_t ap_base)
{
	extern char _ulmk_kernel_text_start[];
	extern char _ulmk_kernel_text_end[];
	extern char _ulmk_kernel_data_start[];
	extern char _ulmk_kernel_bss_end[];
	extern char _ulmk_isr_stack_base[];
	extern char _ulmk_isr_stack_top[];

	/* Slot 0: kernel + user text (RX). */
	apr_program(ap_base, 0u,
		    (uintptr_t)_ulmk_kernel_text_start,
		    (uintptr_t)_ulmk_kernel_text_end - 1u,
		    SSU_LINK_RD(SSU_EXE_LINK_RUNTIME) | SSU_LINK_RD(2u),
		    true);
	/* Slot 1: kernel data/bss (RW). */
	apr_program(ap_base, 1u,
		    (uintptr_t)_ulmk_kernel_data_start,
		    (uintptr_t)_ulmk_kernel_bss_end - 1u,
		    SSU_LINK_RW(SSU_EXE_LINK_RUNTIME) | SSU_LINK_RW(2u),
		    false);
	/* Slot 2: ISR / kernel stacks (RW). */
	apr_program(ap_base, 2u,
		    (uintptr_t)_ulmk_isr_stack_base,
		    (uintptr_t)_ulmk_isr_stack_top - 1u,
		    SSU_LINK_RW(SSU_EXE_LINK_RUNTIME) | SSU_LINK_RW(2u),
		    false);
	/* Slot 3: PIPE + SSU MMIO window (RW, Link2). */
	apr_program(ap_base, 3u,
		    ULMK_BOARD_PIPE_BASE,
		    ULMK_BOARD_PIPE_BASE + 0xFFFFu,
		    SSU_LINK_RW(2u),
		    false);
	apr_program(ap_base, 4u,
		    ULMK_BOARD_SSUGEN_BASE,
		    ULMK_BOARD_SSUGEN_BASE + 0xFFFFu,
		    SSU_LINK_RW(2u),
		    false);
}

void ulmk_arch_mpu_enable(void)
{
	if (g_ssu_mode2) {
		ssu_install_static_aprs(ssu_ap_base());
		MMIO32(ULMK_BOARD_SSUGEN_BASE + SSU_O_LINK2_AP_OVERRIDE) = 0u;
		g_ssu_enforcing = 1u;
	} else {
		g_ssu_enforcing = 0u;
	}
}

bool ulmk_arch_ssu_is_enforcing(void)
{
	return g_ssu_enforcing != 0u;
}

uint32_t ulmk_arch_ssu_mode(void)
{
	return MMIO32(ULMK_BOARD_SSUGEN_BASE + SSU_O_MODE) & 0x3Fu;
}

void ulmk_arch_mpu_disable(void)
{
	g_ssu_enforcing = 0u;
	MMIO32(ULMK_BOARD_SSUGEN_BASE + SSU_O_LINK2_AP_OVERRIDE) |=
		(1u << 0) | (1u << 1) | (1u << 2);
}

void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			     uint8_t count)
{
	uint8_t i;
	uint8_t slot;
	uint32_t ap_base;
	uint32_t access;
	bool exec;

	(void)prs;
	ap_base = ssu_ap_base();

	if (!regions || count > ULMK_ARCH_MAX_REGIONS)
		count = 0u;

	for (i = 0u; i < count; i++)
		g_active_regions[i] = regions[i];
	g_active_count = count;

	/* Tear down previous dynamic slots before programming. */
	for (slot = 0u; slot < ULMK_ARCH_MAX_REGIONS; slot++)
		apr_disable(ap_base, (uint8_t)(SSU_DYN_APR_BASE + slot));

	for (i = 0u; i < count; i++) {
		const ulmk_arch_region_t *r = &regions[i];
		uintptr_t end;

		if (r->size == 0u)
			continue;
		end = r->base + r->size - 1u;
		exec = (r->type == ULMK_REGION_CODE) ||
		       ((r->perms & ULMK_PERM_EXEC) != 0u);
		if (r->perms & ULMK_PERM_WRITE)
			access = SSU_LINK_RW(SSU_EXE_LINK_RUNTIME) |
				 SSU_LINK_RW(2u);
		else
			access = SSU_LINK_RD(SSU_EXE_LINK_RUNTIME) |
				 SSU_LINK_RD(2u);
		apr_program(ap_base, (uint8_t)(SSU_DYN_APR_BASE + i),
			    r->base, end, access, exec);
	}
}

void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			  uint8_t prs)
{
	ulmk_arch_mpu_configure(prs, regions, count);
}

bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms)
{
	uint8_t i;
	uintptr_t end;

	if (!g_ssu_enforcing)
		return true;
	if (size == 0u)
		return false;
	end = addr + size - 1u;
	for (i = 0u; i < g_active_count; i++) {
		const ulmk_arch_region_t *r = &g_active_regions[i];
		uintptr_t r_end;

		if ((r->perms & perms) != perms)
			continue;
		if (r->size == 0u)
			continue;
		r_end = r->base + r->size - 1u;
		if (addr >= r->base && end <= r_end)
			return true;
	}
	return false;
}
