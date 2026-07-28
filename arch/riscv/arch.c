/* SPDX-License-Identifier: MIT */
/*
 * RISC-V RV32 arch port — arch/riscv/arch.c
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <ulmk_arch.h>
#include "irq_internal.h"

#define TF_SIZE		144u
#define TF_RA		0u
#define TF_S0		28u
#define TF_S1		32u
#define TF_A0		36u
#define TF_A1		40u
#define TF_A2		44u
#define TF_A3		48u
#define TF_A7		64u
#define TF_MEPC		108u
#define TF_MSTATUS	112u

#define MCAUSE_INT_BIT	(1u << 31)
#define MCAUSE_EC_MASK	0x7FFFFFFFu

#define MCAUSE_ECALL_U	8u
#define MCAUSE_ECALL_M	11u
#define MCAUSE_LOAD_FAULT	5u
#define MCAUSE_STORE_FAULT	7u
#define MCAUSE_INST_FAULT	1u
#define MCAUSE_ILLEGAL_INST	2u

#define PMP_R	0x01u
#define PMP_W	0x02u
#define PMP_X	0x04u
#define PMP_A_NAPOT	0x18u

struct riscv_trap_frame {
	uint32_t regs[TF_SIZE / 4u];
};

/* Indexed by mhartid — trap.S stores the frame pointer per hart. */
uintptr_t g_trap_sp[ULMK_ARCH_NUM_CPU];

static inline uint32_t read_mstatus(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, mstatus" : "=r"(val));
	return val;
}

static inline void write_mstatus(uint32_t val)
{
	__asm__ volatile("csrw mstatus, %0" :: "r"(val));
}

static inline uint32_t read_mcause(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, mcause" : "=r"(val));
	return val;
}

static inline void clear_mstatus_mie(void)
{
	__asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE_BIT));
}

static inline void set_mstatus_mie(void)
{
	__asm__ volatile("csrs mstatus, %0" :: "r"(MSTATUS_MIE_BIT));
}

static inline uint32_t read_pmpcfg0(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, pmpcfg0" : "=r"(val));
	return val;
}

static inline void write_pmpcfg0(uint32_t val)
{
	__asm__ volatile("csrw pmpcfg0, %0" :: "r"(val));
}

static inline uint32_t read_pmpcfg1(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, pmpcfg1" : "=r"(val));
	return val;
}

static inline void write_pmpcfg1(uint32_t val)
{
	__asm__ volatile("csrw pmpcfg1, %0" :: "r"(val));
}

static void pmp_write_addr(uint8_t idx, uint32_t val)
{
	switch (idx) {
	case 0: __asm__ volatile("csrw pmpaddr0, %0" :: "r"(val)); break;
	case 1: __asm__ volatile("csrw pmpaddr1, %0" :: "r"(val)); break;
	case 2: __asm__ volatile("csrw pmpaddr2, %0" :: "r"(val)); break;
	case 3: __asm__ volatile("csrw pmpaddr3, %0" :: "r"(val)); break;
	case 4: __asm__ volatile("csrw pmpaddr4, %0" :: "r"(val)); break;
	case 5: __asm__ volatile("csrw pmpaddr5, %0" :: "r"(val)); break;
	case 6: __asm__ volatile("csrw pmpaddr6, %0" :: "r"(val)); break;
	case 7: __asm__ volatile("csrw pmpaddr7, %0" :: "r"(val)); break;
	default: break;
	}
}

static inline uint32_t pmp_addr_encode(uintptr_t addr)
{
	return (uint32_t)(addr >> 2);
}

static void pmp_write_cfg(uint8_t idx, uint8_t cfg)
{
	uint32_t v;

	if (idx >= ULMK_ARCH_PMP_NUM)
		return;

	if (idx < 4u) {
		v = read_pmpcfg0();
		v = (v & ~(0xFFu << (idx * 8u))) | ((uint32_t)cfg << (idx * 8u));
		write_pmpcfg0(v);
		return;
	}

	v = read_pmpcfg1();
	idx -= 4u;
	v = (v & ~(0xFFu << (idx * 8u))) | ((uint32_t)cfg << (idx * 8u));
	write_pmpcfg1(v);
}

static void pmp_clear_all(void)
{
	uint8_t i;

	for (i = 0u; i < ULMK_ARCH_PMP_NUM; i++) {
		pmp_write_addr(i, 0u);
		pmp_write_cfg(i, 0u);
	}
}

static uintptr_t napot_round_size(uintptr_t size)
{
	uintptr_t s = 8u;

	if (size <= 8u)
		return 8u;
	while (s < size)
		s <<= 1u;
	return s;
}

static void pmp_set_napot(uint8_t idx, uintptr_t base, uintptr_t size, uint8_t perm)
{
	uintptr_t napot;
	uint32_t  addr;

	if (idx >= ULMK_ARCH_PMP_NUM || size == 0u)
		return;

	napot = napot_round_size(size);
	base &= ~(napot - 1u);
	addr = pmp_addr_encode(base) | (pmp_addr_encode(napot) - 1u);
	pmp_write_addr(idx, addr);
	pmp_write_cfg(idx, perm | PMP_A_NAPOT);
}

static void pmp_set_tor(uint8_t idx, uintptr_t lo, uintptr_t hi, uint8_t perm)
{
	uintptr_t size;

	if (hi <= lo)
		return;
	size = hi - lo;
	pmp_set_napot(idx, lo, size, perm);
}

static uint32_t user_mstatus_init(void)
{
	return MSTATUS_MPIE_BIT | MSTATUS_MPP_U;
}

/* =========================================================================
 * CPU control
 * ========================================================================= */

#define ULMK_IRQ_KEY_SKIP	(1u << 31)

ulmk_arch_irq_key_t ulmk_arch_cpu_irq_save(void)
{
	uint32_t mstatus = read_mstatus();

	/* Syscall / nested path already has MIE clear — skip csr traffic. */
	if ((mstatus & MSTATUS_MIE_BIT) == 0u)
		return (ulmk_arch_irq_key_t)(mstatus | ULMK_IRQ_KEY_SKIP);
	clear_mstatus_mie();
	return mstatus;
}

void ulmk_arch_cpu_irq_restore(ulmk_arch_irq_key_t key)
{
	uint32_t mstatus = (uint32_t)key;

	if (mstatus & ULMK_IRQ_KEY_SKIP)
		return;
	write_mstatus(mstatus);
}

void ulmk_arch_cpu_irq_enable(void)
{
	set_mstatus_mie();
}

void ulmk_arch_cpu_irq_disable(void)
{
	clear_mstatus_mie();
}

void ulmk_arch_cpu_idle(void)
{
#if ULMK_ARCH_IDLE_IS_WFI
	__asm__ volatile("wfi" ::: "memory");
#else
	__asm__ volatile("nop");
#endif
}

void ulmk_arch_cpu_halt(void)
{
	for (;;)
		;
}

uint32_t ulmk_arch_cpu_clz(uint32_t val)
{
	if (val == 0u)
		return 32u;
	return (uint32_t)__builtin_clz(val);
}

#if ULMK_CONFIG_SYSCALL_WCET
void ulmk_arch_cycle_enable(void)
{
	/* mcycle is free-running from reset; nothing to unlock in M-mode. */
}

uint32_t ulmk_arch_cycle_read(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, mcycle" : "=r"(v));
	return v;
}
#else
void ulmk_arch_cycle_enable(void)
{
}

uint32_t ulmk_arch_cycle_read(void)
{
	return 0u;
}
#endif

/* =========================================================================
 * Context management
 * ========================================================================= */

extern void _ulmk_thread_trampoline_m(void);
extern void _ulmk_thread_trampoline_u(void);

void ulmk_arch_csa_pool_init(uintptr_t pool_base, size_t pool_size)
{
	(void)pool_base;
	(void)pool_size;
}

void ulmk_arch_ctx_init(ulmk_arch_ctx_t *ctx,
		      void (*entry)(void *arg), void *arg,
		      uintptr_t stack_top, ulmk_privilege_t priv)
{
	uint32_t *frame;
	uint32_t  i;

	void (*trampoline)(void);

	frame = (uint32_t *)(stack_top - ULMK_ARCH_CTX_FRAME_SIZE);
	for (i = 0u; i < (ULMK_ARCH_CTX_FRAME_SIZE / 4u); i++)
		frame[i] = 0u;

	trampoline = (priv == ULMK_PRIV_KERNEL) ?
		     _ulmk_thread_trampoline_m : _ulmk_thread_trampoline_u;
	frame[0] = (uint32_t)(uintptr_t)trampoline;
	frame[1] = (uint32_t)(uintptr_t)entry;
	frame[2] = (uint32_t)(uintptr_t)arg;
	ctx->sp = (uint32_t)(uintptr_t)frame;
}

void ulmk_arch_ctx_free(ulmk_arch_ctx_t *ctx)
{
	if (ctx)
		ctx->sp = 0u;
}

bool ulmk_arch_sched_isr_preempt_deferred(void)
{
	return false;
}

void ulmk_arch_sched_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to,
			    unsigned int flags)
{
	(void)flags;

	ulmk_arch_ctx_switch(from, to);
}

/* =========================================================================
 * PMP (ulmk_arch_mpu_* API)
 * ========================================================================= */

static void pmp_kernel_layout(void)
{
	uintptr_t kexec_lo;
	uintptr_t kexec_hi;
	uintptr_t utext_lo;
	uintptr_t utext_hi;
	uintptr_t kram_lo;
	uintptr_t kram_hi;
	uintptr_t uram_lo;
	uintptr_t uram_hi;
	uintptr_t mmio_lo;
	uintptr_t mmio_hi;

	extern uint8_t _ulmk_kernel_exec_start[];
	extern uint8_t _ulmk_kernel_exec_end[];
	extern uint8_t _ulmk_user_text_start[];
	extern uint8_t _ulmk_user_text_end[];
	extern uint8_t _ulmk_kernel_data_start[];
	extern uint8_t _ulmk_kernel_ram_end[];
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_end[];
	extern uintptr_t _ulmk_mem_periph_base[];
	extern uintptr_t _ulmk_mem_periph_end[];

	kexec_lo = (uintptr_t)_ulmk_kernel_exec_start;
	kexec_hi = (uintptr_t)_ulmk_kernel_exec_end;
	utext_lo = (uintptr_t)_ulmk_user_text_start;
	utext_hi = (uintptr_t)_ulmk_user_text_end;
	kram_lo  = (uintptr_t)_ulmk_kernel_data_start;
	kram_hi  = (uintptr_t)_ulmk_kernel_ram_end;
	uram_lo  = (uintptr_t)_ulmk_user_ram_start;
	uram_hi  = (uintptr_t)_ulmk_user_pool_end;
	mmio_lo  = (uintptr_t)_ulmk_mem_periph_base;
	mmio_hi  = (uintptr_t)_ulmk_mem_periph_end;

	pmp_clear_all();

	if (kexec_hi > kexec_lo)
		pmp_set_napot(ULMK_ARCH_PMP_KERNEL, kexec_lo, kexec_hi - kexec_lo,
			      PMP_R | PMP_X);

	if (kram_hi > kram_lo)
		pmp_set_napot(ULMK_ARCH_PMP_KRAM, kram_lo, kram_hi - kram_lo,
			      PMP_R | PMP_W);

	if (utext_hi > utext_lo)
		pmp_set_napot(ULMK_ARCH_PMP_UTEXT, utext_lo, utext_hi - utext_lo,
			      PMP_R | PMP_X);

	if (uram_hi > uram_lo)
		pmp_set_napot(ULMK_ARCH_PMP_URAM, uram_lo, uram_hi - uram_lo,
			      PMP_R | PMP_W);

	if (mmio_hi > mmio_lo)
		pmp_set_napot(ULMK_ARCH_PMP_MMIO, mmio_lo, mmio_hi - mmio_lo,
			      PMP_R | PMP_W);

	(void)kexec_lo;
}

static void pmp_user_layout(const ulmk_arch_region_t *regions, uint8_t count)
{
	uintptr_t utext_lo;
	uintptr_t utext_hi;
	uintptr_t uram_lo;
	uintptr_t uram_hi;
	uintptr_t mmio_lo;
	uintptr_t mmio_hi;
	uint8_t   slot;
	uint8_t   i;

	extern uint8_t _ulmk_user_text_start[];
	extern uint8_t _ulmk_user_text_end[];
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_end[];
	extern uintptr_t _ulmk_mem_periph_base[];
	extern uintptr_t _ulmk_mem_periph_end[];

	utext_lo = (uintptr_t)_ulmk_user_text_start;
	utext_hi = (uintptr_t)_ulmk_user_text_end;
	uram_lo  = (uintptr_t)_ulmk_user_ram_start;
	uram_hi  = (uintptr_t)_ulmk_user_pool_end;
	mmio_lo  = (uintptr_t)_ulmk_mem_periph_base;
	mmio_hi  = (uintptr_t)_ulmk_mem_periph_end;

	pmp_clear_all();

	if (utext_hi > utext_lo)
		pmp_set_napot(ULMK_ARCH_PMP_UTEXT, utext_lo, utext_hi - utext_lo,
			      PMP_R | PMP_X);

	if (uram_hi > uram_lo)
		pmp_set_napot(ULMK_ARCH_PMP_URAM, uram_lo, uram_hi - uram_lo,
			      PMP_R | PMP_W);

	if (mmio_hi > mmio_lo)
		pmp_set_napot(ULMK_ARCH_PMP_MMIO, mmio_lo, mmio_hi - mmio_lo,
			      PMP_R | PMP_W);

	/*
	 * STACK sits inside the static URAM NAPOT window — skip it.  Remaining
	 * dynamic entries (heap / shared / spare) use TOR slots.
	 */
	slot = ULMK_ARCH_PMP_USER_BASE;
	if (regions && count > 0u) {
		for (i = 0u; i < count && slot < ULMK_ARCH_PMP_NUM; i++) {
			uint8_t perm = 0u;

			if (regions[i].type == ULMK_REGION_STACK)
				continue;

			if (regions[i].perms & ULMK_PERM_READ)
				perm |= PMP_R;
			if (regions[i].perms & ULMK_PERM_WRITE)
				perm |= PMP_W;
			if (regions[i].perms & ULMK_PERM_EXEC)
				perm |= PMP_X;

			pmp_set_tor(slot, regions[i].base,
				    regions[i].base + regions[i].size, perm);
			slot++;
		}
	}
}

void ulmk_arch_mpu_init(void)
{
	pmp_kernel_layout();
}

void ulmk_arch_mpu_enable(void)
{
}

void ulmk_arch_mpu_disable(void)
{
	pmp_clear_all();
}

void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			   uint8_t count)
{
	(void)prs;
	(void)regions;
	(void)count;
}

/*
 * PMP CSRs are per-hart.  The "last programmed" cache must not be global or
 * one CPU's switch causes another's mpu_switch to skip a real rewrite.
 */
struct pmp_cpu_cache {
	const ulmk_arch_region_t *regions;
	uint8_t count;
	uint8_t prs;
	uint8_t dyn;
};

/*
 * prs=0xFF forces the first mpu_switch on every hart to program PMP.
 * Zero-init would equal ULMK_ARCH_PRS_KERNEL and skip the first rewrite
 * on CPU2+ when NUM_CPU > 2.
 */
static struct pmp_cpu_cache g_pmp_cache[ULMK_ARCH_NUM_CPU] = {
	[0 ... ULMK_ARCH_NUM_CPU - 1] = { .prs = 0xFFu },
};

static uint8_t pmp_dyn_count(const ulmk_arch_region_t *regions, uint8_t count)
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
	struct pmp_cpu_cache *c;
	uint32_t              cpu;
	uint8_t               eff;

	cpu = ulmk_arch_cpu_id();
	if (cpu >= (uint32_t)ULMK_ARCH_NUM_CPU)
		cpu = 0u;
	c = &g_pmp_cache[cpu];

	/*
	 * On SMP only skip when this hart already has the exact same layout.
	 * The stack-only fast path was UP-friendly but races badly when another
	 * hart's view of "already programmed" is assumed.
	 */
	if (prs == c->prs && regions == c->regions && count == c->count)
		return;

	eff = (prs == ULMK_ARCH_PRS_KERNEL) ? 0u : pmp_dyn_count(regions, count);

#if !ULMK_CONFIG_ENABLE_SMP
	/* Stack-only AS: static URAM covers stacks — skip full PMP rewrite. */
	if (prs == c->prs && eff == 0u && c->dyn == 0u &&
	    prs != ULMK_ARCH_PRS_KERNEL) {
		c->regions = regions;
		c->count   = count;
		return;
	}
#endif

	if (prs == ULMK_ARCH_PRS_KERNEL)
		pmp_kernel_layout();
	else
		pmp_user_layout(regions, count);

	c->prs     = prs;
	c->regions = regions;
	c->count   = count;
	c->dyn     = eff;
}

bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms)
{
	(void)addr;
	(void)size;
	(void)perms;
	return true;
}

static uint8_t mcause_to_trap_class(uint32_t mcause)
{
	uint32_t code = mcause & MCAUSE_EC_MASK;

	switch (code) {
	case MCAUSE_INST_FAULT:
	case MCAUSE_LOAD_FAULT:
	case MCAUSE_STORE_FAULT:
		return 0u;
	case MCAUSE_ECALL_U:
	case MCAUSE_ECALL_M:
		return 6u;
	default:
		return 4u;
	}
}

void _ulmk_trap_dispatch(struct riscv_trap_frame *frame)
{
	uint32_t mcause = read_mcause();
	uint32_t mstatus;
	uint32_t ret;
	uint32_t args[4];
	uint32_t code;

	ulmk_arch_mpu_switch(NULL, 0, ULMK_ARCH_PRS_KERNEL);

	if (mcause & MCAUSE_INT_BIT) {
		riscv_irq_handle_interrupt(mcause);
		ulmk_kern_trap_mpu_restore();
		return;
	}

	code = mcause & MCAUSE_EC_MASK;
	if (code == MCAUSE_ECALL_U || code == MCAUSE_ECALL_M) {
		mstatus = frame->regs[TF_MSTATUS / 4u];
		frame->regs[TF_MEPC / 4u] += 4u;

		args[0] = frame->regs[TF_A0 / 4u];
		args[1] = frame->regs[TF_A1 / 4u];
		args[2] = frame->regs[TF_A2 / 4u];
		args[3] = frame->regs[TF_A3 / 4u];

		clear_mstatus_mie();
		ret = ulmk_kern_trap_syscall((uint8_t)frame->regs[TF_A7 / 4u], args);
		ulmk_kern_sched_dispatch(false);
		ret = ulmk_kern_syscall_ret_resolve(ret);
		frame->regs[TF_A0 / 4u] = ret;
		/*
		 * Keep MIE clear until mret (MPIE → MIE).  Forcing MIE=1 here
		 * lets a pending MTIP nest in the trap epilogue with
		 * mepc=epilogue and MPP=U after a bad restore → INST_FAULT.
		 */
		frame->regs[TF_MSTATUS / 4u] =
			(mstatus & ~MSTATUS_MIE_BIT) | MSTATUS_MPIE_BIT;
		ulmk_kern_trap_mpu_restore();
		return;
	}

	/*
	 * U-mode fetch of kernel text may raise INST_FAULT (PMP deny) or,
	 * when a NAPOT user RX window overlaps and the first insn is a
	 * privileged CSR (-O1+), ILLEGAL_INST.  Userspace load/store/fetch
	 * faults are recoverable (kill thread).  M-mode faults panic.
	 */
	mstatus = frame->regs[TF_MSTATUS / 4u];
	if (((mstatus >> MSTATUS_MPP_SHIFT) & 3u) == 0u &&
	    (code == MCAUSE_LOAD_FAULT || code == MCAUSE_STORE_FAULT ||
	     code == MCAUSE_INST_FAULT || code == MCAUSE_ILLEGAL_INST))
		ulmk_arch_trap_entry(0u, (uint8_t)code);
	else
		ulmk_arch_trap_entry(mcause_to_trap_class(mcause), (uint8_t)code);
}

void ulmk_arch_syscall_entry(void)
{
}

/* =========================================================================
 * Atomics
 * ========================================================================= */

uint32_t ulmk_arch_atomic_cas(volatile uint32_t *ptr,
			    uint32_t expected, uint32_t desired)
{
	uint32_t old;
	ulmk_arch_irq_key_t key = ulmk_arch_cpu_irq_save();

	old = *ptr;
	if (old == expected)
		*ptr = desired;
	ulmk_arch_cpu_irq_restore(key);
	return old;
}

uint32_t ulmk_arch_atomic_add(volatile uint32_t *ptr, uint32_t val)
{
	uint32_t old;
	uint32_t new_val;

	do {
		old     = *ptr;
		new_val = old + val;
	} while (ulmk_arch_atomic_cas(ptr, old, new_val) != old);

	return old;
}

/* =========================================================================
 * Trap diagnostics
 * ========================================================================= */

static void dump_puts(const char *s)
{
	while (*s)
		ulmk_printk_char_out(*s++);
}

void ulmk_arch_trap_dump(uint8_t trap_class, uint8_t tin)
{
	(void)trap_class;
	dump_puts("  tin=");
	(void)tin;
	dump_puts("\n");
}

static void dump_hex8(uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	char              buf[11];
	int               i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++)
		buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xFu];
	buf[10] = '\0';
	dump_puts(buf);
}

void ulmk_arch_trap_entry(uint8_t trap_class, uint8_t tin)
{
	uint32_t mcause;

	mcause = read_mcause();
	dump_puts("TRAP class=");
	dump_hex8((uint32_t)trap_class);
	dump_puts(" tin=");
	dump_hex8((uint32_t)tin);
	dump_puts(" mcause=");
	dump_hex8(mcause);
	dump_puts(" mtval=");
	{
		uint32_t mtval;

		__asm__ volatile("csrr %0, mtval" : "=r"(mtval));
		dump_hex8(mtval);
	}
	dump_puts("\n");
	ulmk_arch_trap_dump(trap_class, tin);

	if (trap_class == 0u || ulmk_irq_in_attach())
		ulmk_kern_trap_recoverable();
	else
		ulmk_kern_trap_panic();
}

/* =========================================================================
 * Boot
 * ========================================================================= */

extern void _trap_handler(void);

void ulmk_arch_init(ulmk_boot_info_t *info)
{
	extern uint8_t _ulmk_user_ram_start[];
	extern uint8_t _ulmk_user_pool_end[];

	if (info) {
		info->mem_count = 1u;
		info->mem[0].base = (uintptr_t)_ulmk_user_ram_start;
		info->mem[0].size = (uintptr_t)_ulmk_user_pool_end -
				    (uintptr_t)_ulmk_user_ram_start;
		info->csa_pool_base = 0u;
		info->csa_pool_size = 0u;
	}

	ulmk_arch_irq_vectors_init((uintptr_t)_trap_handler, 0u, 0u);
	ulmk_arch_mpu_init();
#if ULMK_CONFIG_ENABLE_SMP
	/* Accept CLINT MSIP reschedule IPIs on every hart. */
	__asm__ volatile("csrs mie, %0" :: "r"(1u << 3));
#endif
	(void)user_mstatus_init;
}

/* =========================================================================
 * Kernel tick — CLINT mtimecmp (per-hart)
 * ========================================================================= */

static uint64_t g_tick_period;

static uint64_t clint_mtime_read(void)
{
	volatile uint32_t *mtime =
		(volatile uint32_t *)(uintptr_t)ULMK_ARCH_CLINT_MTIME;
	uint32_t hi, lo;

	do {
		hi = mtime[1];
		lo = mtime[0];
	} while (hi != mtime[1]);

	return ((uint64_t)hi << 32) | lo;
}

static void clint_mtimecmp_write(uint32_t hart, uint64_t when)
{
	volatile uint32_t *cmp =
		(volatile uint32_t *)(uintptr_t)ULMK_ARCH_CLINT_MTIMECMP(hart);

	cmp[1] = 0xFFFFFFFFu;
	cmp[0] = (uint32_t)when;
	cmp[1] = (uint32_t)(when >> 32);
}

void ulmk_arch_tick_init(uint32_t tick_hz)
{
	uint32_t hart = ulmk_arch_cpu_id();
	uint64_t now;

	if (tick_hz == 0u)
		tick_hz = 1000u;

	g_tick_period = (uint64_t)ULMK_BOARD_TICK_CLOCK_HZ / (uint64_t)tick_hz;
	if (g_tick_period == 0u)
		g_tick_period = 1u;

	now = clint_mtime_read();
	clint_mtimecmp_write(hart, now + g_tick_period);
	__asm__ volatile("csrs mie, %0" :: "r"(1u << 7));
}

void ulmk_arch_tick_ack(void)
{
	uint32_t hart = ulmk_arch_cpu_id();
	uint64_t now = clint_mtime_read();
	uint64_t next = now + g_tick_period;

	clint_mtimecmp_write(hart, next);
}

uint32_t ulmk_arch_timer_wheel_cpu(void)
{
	return ulmk_arch_cpu_id();
}

/* Cache stubs — ULMK_ARCH_HAS_CACHE=0; syscalls return ENOTSUP. */
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
