/* SPDX-License-Identifier: MIT */
/*
 * C29 SMP — IPC FLAG0 IPI and secondary bring-up via SSU reset vectors.
 */

#include <stdint.h>
#include <stdbool.h>

#include <ulmk/config.h>
#include <ulmk_arch.h>

#define MMIO32(a)	(*(volatile uint32_t *)(uintptr_t)(a))
#define MMIO16(a)	(*(volatile uint16_t *)(uintptr_t)(a))

/* Local-CPU IPC send bases (hw_memmap.h). */
#define IPC_CPU1_TO_CPU2	0x30220000u
#define IPC_CPU1_TO_CPU3	0x30222000u
#define IPC_CPU2_TO_CPU1	0x30228000u
#define IPC_CPU2_TO_CPU3	0x3022A000u
#define IPC_CPU3_TO_CPU1	0x30230000u
#define IPC_CPU3_TO_CPU2	0x30232000u

#define IPC_O_SET		0x00u
#define IPC_O_CLR		0x04u
#define IPC_O_ACK_OFF		0x20004u	/* receive-view ACK relative */

#define SSU_O_RST_VECT		0x00u
#define SSU_O_RST_LINK		0x04u
#define SSU_O_CPU_RST_CTRL	0x08u
#define SSU_O_DEF_NMI_VECT	0x10u
#define SSU_O_DEF_NMI_LINK	0x14u
#define SSU_CORE_RESET_DEACTIVE	0x36u
#define SSU_LINK2		2u

/*
 * SSUGEN LINK2 access-protection override.  The RTS _c_int00 sets this for
 * the running core so LINK2 code may reach every APR; a secondary released
 * with a hand-written stub never runs _c_int00, so CPU1 must grant the
 * override for the target before release or the stub's first store faults.
 */
#define SSUGEN_LINK2_AP_OVERRIDE	0x3008000Cu

/*
 * Handshake in SHARED_RAM (LDA1 @ 0x200F8000) — visible to every C29.
 * Do not use LDA0/high KERNEL_RAM: CPU2/3 data windows are LDA5/CPA-side;
 * absolute stores there silently fault under APR and the stub never
 * completes the ready handshake.
 */
#define C29_CPU2_MAGIC_ADDR	0x200F8000u
#define C29_CPU3_MAGIC_ADDR	0x200F8004u
#define C29_CPU2_MAGIC_VAL	0xC0DE0002u
#define C29_CPU3_MAGIC_VAL	0xC0DE0003u

#define SYSCTL_O_RSTSTAT	0x3B0u
#define SYSCTL_RSTSTAT_CPU2	0x1u
#define SYSCTL_RSTSTAT_CPU3	0x2u

#define SMP_GATE_WAIT		0x11111111u
#define SMP_GATE_READY		0xC0DEC0DEu

extern void ulmk_c29_cpu2_start(void);
extern void ulmk_c29_cpu3_start(void);

#if ULMK_CONFIG_ENABLE_SMP

volatile uint32_t g_c29_smp_gate = SMP_GATE_WAIT;
volatile uint32_t g_c29_secondary_release[ULMK_ARCH_NUM_CPU];
volatile uint32_t g_c29_ready_mask;
/* Bring-up diagnostics for HIL (filled on secondary timeout). */
volatile uint32_t g_c29_smp_diag[6];
static void (*g_secondary_entry[ULMK_ARCH_NUM_CPU])(void);

static uint32_t ipc_send_base(uint32_t from, uint32_t to)
{
	if (from == 0u && to == 1u)
		return IPC_CPU1_TO_CPU2;
	if (from == 0u && to == 2u)
		return IPC_CPU1_TO_CPU3;
	if (from == 1u && to == 0u)
		return IPC_CPU2_TO_CPU1;
	if (from == 1u && to == 2u)
		return IPC_CPU2_TO_CPU3;
	if (from == 2u && to == 0u)
		return IPC_CPU3_TO_CPU1;
	if (from == 2u && to == 1u)
		return IPC_CPU3_TO_CPU2;
	return 0u;
}

static uint32_t ipc_ack_base(uint32_t local_cpu)
{
	/*
	 * Receive-view bases: CPU1 0x30240000, CPU2 0x30248000, CPU3 0x30250000.
	 * ACK for FLAG0 from either peer is channel 0 at +0x4 of the pair.
	 */
	static const uint32_t rcv[3] = {
		0x30240000u, 0x30248000u, 0x30250000u
	};

	if (local_cpu >= 3u)
		return 0u;
	return rcv[local_cpu];
}

void ulmk_arch_smp_mark_ready(void)
{
	uint32_t id = ulmk_arch_cpu_id();

	__asm__ volatile("" ::: "memory");
	g_c29_smp_gate = SMP_GATE_READY;
	g_c29_ready_mask |= (1u << id);
}

uint32_t ulmk_arch_smp_ready_mask(void)
{
	return g_c29_ready_mask;
}

void ulmk_arch_smp_wait_ready(uint32_t mask)
{
	uint32_t spins;

	for (spins = 0u; spins < 50000000u; spins++) {
		if ((g_c29_ready_mask & mask) == mask)
			return;
	}
}

void ulmk_arch_send_ipi(uint32_t cpu_id)
{
	uint32_t self = ulmk_arch_cpu_id();
	uint32_t base;

	if (cpu_id >= (uint32_t)ULMK_ARCH_NUM_CPU || cpu_id == self)
		return;
	base = ipc_send_base(self, cpu_id);
	if (!base)
		return;
	__asm__ volatile("" ::: "memory");
	MMIO32(base + IPC_O_SET) = ULMK_ARCH_IPC_FLAG0;
}

void ulmk_arch_ipi_clear_self(void)
{
	uint32_t self = ulmk_arch_cpu_id();
	uint32_t rcv = ipc_ack_base(self);

	if (!rcv)
		return;
	/* Ack FLAG0 from both peer directions (CH0 and the +0x2000 pair). */
	MMIO32(rcv + 0x04u) = ULMK_ARCH_IPC_FLAG0;
	MMIO32(rcv + 0x2004u) = ULMK_ARCH_IPC_FLAG0;
}

void ulmk_arch_ipi_note_enter(void)
{
}

void ulmk_arch_ipi_pulse_self(void)
{
	/* No self-MSIP equivalent; force local IPC is not used. */
}

void ulmk_arch_secondary_init(void)
{
	extern char _ulmk_isr_stack_top[];

	ulmk_arch_irq_vectors_init(0u, 0u, (uintptr_t)_ulmk_isr_stack_top);
	ulmk_arch_mpu_init();
}

void ulmk_arch_secondary_mark_ready(void)
{
	uint32_t id = ulmk_arch_cpu_id();

	g_c29_ready_mask |= (1u << id);
}

#define MEMSSLCFG_BASE		0x301D8000u
#define MEMSSCCFG_BASE		0x301D8400u
#define MEMSS_INIT		0x10000u
#define MEMSS_INIT_STS		0x1000000u
#define MEMSS_LPA1_CFG		(MEMSSLCFG_BASE + 0x10u)
#define MEMSS_LDA1_CFG		(MEMSSLCFG_BASE + 0x30u)
#define MEMSS_CPA0_CFG		(MEMSSCCFG_BASE + 0x0u)

static void memss_init_bank(uint32_t cfg)
{
	uint32_t t;

	MMIO32(cfg) |= MEMSS_INIT;
	for (t = 0u; t < 1000000u; t++) {
		if ((MMIO32(cfg) & MEMSS_INIT_STS) != 0u)
			break;
	}
}

#if defined(ULMK_C29_FLASH) && ULMK_C29_FLASH
/*
 * Flash POR has no GEL ram_init: ECC-init the secondary fetch bank and the
 * SHARED_RAM handshake window, then copy the stub out of contiguous FLASH_RP0.
 */
static void plant_secondary_from_flash(uint32_t cpu_id, uint32_t dest)
{
	static uint8_t lda1_ready;
	uint32_t src;
	uint32_t i;
	uint32_t nwords;

	if (cpu_id == 1u) {
		src = (uint32_t)(uintptr_t)&ulmk_c29_cpu2_start;
		memss_init_bank(MEMSS_LPA1_CFG);
	} else {
		src = (uint32_t)(uintptr_t)&ulmk_c29_cpu3_start;
		memss_init_bank(MEMSS_CPA0_CFG);
	}
	if (!lda1_ready) {
		memss_init_bank(MEMSS_LDA1_CFG);
		lda1_ready = 1u;
	}

	nwords = ULMK_C29_SECONDARY_STUB_BYTES / 4u;
	for (i = 0u; i < nwords; i++)
		MMIO32(dest + (i * 4u)) = MMIO32(src + (i * 4u));
}
#endif

__attribute__((section(".text.link2.ulmk_c29_release")))
static void release_secondary_cpu(uint32_t cpu_id, uint32_t reset_addr)
{
	uint32_t cfg;
	uint32_t nmi;
	uint32_t i;

	if (cpu_id == 1u)
		cfg = ULMK_BOARD_SSUCPU2CFG_BASE;
	else if (cpu_id == 2u)
		cfg = ULMK_BOARD_SSUCPU3CFG_BASE;
	else
		return;

	/* Split-lock before first independent CPU2 use (≥24 cycles). */
	if (cpu_id == 1u) {
		MMIO32(ULMK_ARCH_SYSCTL_LSEN) = 0u;
		__asm__ volatile("NOP #1\n\tNOP #1\n\tNOP #1\n\tNOP #1\n\t"
				 "NOP #1\n\tNOP #1\n\tNOP #1\n\tNOP #1\n\t"
				 "NOP #1\n\tNOP #1\n\tNOP #1\n\tNOP #1\n\t"
				 "NOP #1\n\tNOP #1\n\tNOP #1\n\tNOP #1\n\t"
				 "NOP #1\n\tNOP #1\n\tNOP #1\n\tNOP #1\n\t"
				 "NOP #1\n\tNOP #1\n\tNOP #1\n\tNOP #1"
				 ::: "memory");
	}

#if defined(ULMK_C29_FLASH) && ULMK_C29_FLASH
	plant_secondary_from_flash(cpu_id, reset_addr);
#endif

	/*
	 * Grant LINK2 AP override for every C29 (and the target) so the
	 * reset stub may store its handshake.  Bit index = SSU_CPUID.
	 */
	MMIO32(SSUGEN_LINK2_AP_OVERRIDE) |= 0x7u;

	/*
	 * Point DEF_NMI at the same stub as RST_VECT (GEL Release_CPUx_Reset
	 * does this).  A distinct +0x40 NMI veneer faults NMI_ISR_ENTRY under
	 * bring-up (INT_TYPE.NMI_ISR_ENTRY_ERR) before the handshake store.
	 */
	nmi = reset_addr;
	/* Program vectors, then release out of reset (matches TI SDK order). */
	MMIO32(cfg + SSU_O_RST_VECT) = reset_addr;
	MMIO32(cfg + SSU_O_RST_LINK) = SSU_LINK2;
	MMIO32(cfg + SSU_O_DEF_NMI_VECT) = nmi;
	MMIO32(cfg + SSU_O_DEF_NMI_LINK) = SSU_LINK2;
	MMIO32(cfg + SSU_O_CPU_RST_CTRL) = SSU_CORE_RESET_DEACTIVE;

	/*
	 * SysCtl_isCPUxReset(): true while RSTSTAT bit is clear.
	 * Wait until the bit sets (core out of reset), matching TI SDK.
	 */
	for (i = 0u; i < 1000000u; i++) {
		uint16_t st = MMIO16(ULMK_BOARD_DEVCFG_BASE + SYSCTL_O_RSTSTAT);
		uint16_t bit = (cpu_id == 1u) ? SYSCTL_RSTSTAT_CPU2
					      : SYSCTL_RSTSTAT_CPU3;

		if ((st & bit) != 0u)
			break;
	}
}

void ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void))
{
	uint32_t reset_addr;

	if (cpu_id == 0u || cpu_id >= (uint32_t)ULMK_ARCH_NUM_CPU || !entry)
		return;

	g_secondary_entry[cpu_id] = entry;
	__asm__ volatile("" ::: "memory");
	g_c29_secondary_release[cpu_id] = 1u;

	g_c29_smp_diag[0] = cpu_id;
	g_c29_smp_diag[1] =
		MMIO16(ULMK_BOARD_DEVCFG_BASE + SYSCTL_O_RSTSTAT);

	reset_addr = (cpu_id == 1u) ? ULMK_C29_CPU2_RESET_ADDR
				    : ULMK_C29_CPU3_RESET_ADDR;
	{
		uint32_t magic_addr =
			(cpu_id == 1u) ? C29_CPU2_MAGIC_ADDR
				       : C29_CPU3_MAGIC_ADDR;

		MMIO32(magic_addr) = 0u;
	}
	release_secondary_cpu(cpu_id, reset_addr);

	/* Poll shared-RAM magic written by the secondary reset stub. */
	{
		uint32_t bit = 1u << cpu_id;
		uint32_t spins;
		uint16_t st;
		uint32_t cfg;
		uint32_t word0;
		uint32_t magic_addr;
		uint32_t magic_exp;
		uint32_t magic;
		volatile uint32_t *dlb =
			(volatile uint32_t *)ULMK_ARCH_MEM_DLB_CONFIG;

		magic_addr = (cpu_id == 1u) ? C29_CPU2_MAGIC_ADDR
					    : C29_CPU3_MAGIC_ADDR;
		magic_exp = (cpu_id == 1u) ? C29_CPU2_MAGIC_VAL
					   : C29_CPU3_MAGIC_VAL;

		MMIO32(magic_addr) = 0u;
		*dlb &= ~(ULMK_ARCH_MEM_DLB_CPU1_EN |
			  ULMK_ARCH_MEM_DLB_CPU2_EN |
			  ULMK_ARCH_MEM_DLB_CPU3_EN |
			  (1u << 6));

		for (spins = 0u; spins < 5000000u; spins++) {
			magic = MMIO32(magic_addr);
			if (magic == magic_exp) {
				g_c29_ready_mask |= bit;
				g_c29_smp_diag[0] = 0u;
				return;
			}
		}

		st = MMIO16(ULMK_BOARD_DEVCFG_BASE + SYSCTL_O_RSTSTAT);
		cfg = (cpu_id == 1u) ? ULMK_BOARD_SSUCPU2CFG_BASE
				     : ULMK_BOARD_SSUCPU3CFG_BASE;
		word0 = MMIO32(reset_addr);
		magic = MMIO32(magic_addr);
		/* Pack: hi=pre-release rststat, lo=post */
		g_c29_smp_diag[1] = (g_c29_smp_diag[1] << 16) | st;
		g_c29_smp_diag[2] = MMIO32(cfg + SSU_O_CPU_RST_CTRL);
		g_c29_smp_diag[3] = MMIO32(cfg + SSU_O_RST_VECT);
		g_c29_smp_diag[4] = word0;
		g_c29_smp_diag[5] = magic;
	}
}

void ulmk_arch_smp_park(void)
{
	uint32_t cpu = ulmk_arch_cpu_id();
	void (*entry)(void);

	/*
	 * CPU2/CPU3 must not enter here via an LPA0 function pointer — local
	 * program RAMs are not interchangeable.  Secondary stubs mark ready
	 * in LPA1/CPA0 and either idle or run a local payload.
	 */
	if (cpu == 0u || cpu >= (uint32_t)ULMK_ARCH_NUM_CPU) {
		for (;;)
			__asm__ volatile("IDLE" ::: "memory");
	}

	while (g_c29_smp_gate != SMP_GATE_READY)
		;
	while (g_c29_secondary_release[cpu] == 0u)
		;

	entry = g_secondary_entry[cpu];
	if (!entry) {
		for (;;)
			__asm__ volatile("IDLE" ::: "memory");
	}
	entry();
	for (;;)
		__asm__ volatile("IDLE" ::: "memory");
}

#else /* !SMP */

void ulmk_arch_send_ipi(uint32_t cpu_id)
{
	(void)cpu_id;
}

void ulmk_arch_ipi_clear_self(void)
{
}

void ulmk_arch_ipi_note_enter(void)
{
}

void ulmk_arch_ipi_pulse_self(void)
{
}

void ulmk_arch_secondary_init(void)
{
}

void ulmk_arch_secondary_mark_ready(void)
{
}

void ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void))
{
	(void)cpu_id;
	(void)entry;
}

void ulmk_arch_smp_mark_ready(void)
{
}

uint32_t ulmk_arch_smp_ready_mask(void)
{
	return 0u;
}

void ulmk_arch_smp_wait_ready(uint32_t mask)
{
	(void)mask;
}

void ulmk_arch_smp_park(void)
{
	for (;;)
		__asm__ volatile("IDLE" ::: "memory");
}

#endif /* ULMK_CONFIG_ENABLE_SMP */
