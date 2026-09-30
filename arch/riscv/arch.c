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
#include <ulmk/board.h>
#include "irq_internal.h"

#define TF_SIZE		148u
#define TF_RA		0u
#define TF_SP		4u
#define TF_S0		28u
#define TF_S1		32u
#define TF_A0		36u
#define TF_A1		40u
#define TF_A2		44u
#define TF_A3		48u
#define TF_A7		64u
#define TF_MEPC		108u
#define TF_MSTATUS	112u
#define TF_MCAUSE	132u

#define MCAUSE_INT_BIT	(1u << 31)
/* CLIC ports may set sticky high bits; exception code is mcause[11:0]. */
#define MCAUSE_EC_MASK	0xFFFu

#define MCAUSE_ECALL_U	8u
#define MCAUSE_ECALL_M	11u
#define MCAUSE_LOAD_FAULT	5u
#define MCAUSE_STORE_FAULT	7u
#define MCAUSE_INST_FAULT	1u
#define MCAUSE_ILLEGAL_INST	2u

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
	uint32_t access;
	uint32_t mtval;

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
	     code == MCAUSE_INST_FAULT)) {
		access = (code == MCAUSE_LOAD_FAULT)  ? ULMK_PERM_READ :
			 (code == MCAUSE_STORE_FAULT) ? ULMK_PERM_WRITE :
							ULMK_PERM_EXEC;
		__asm__ volatile("csrr %0, mtval" : "=r"(mtval));
		if (ulmk_kern_mem_fault((uintptr_t)mtval, access))
			return;
	}
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
	{
		const uint32_t *tf;

		tf = (const uint32_t *)g_trap_sp[0];
		if (tf) {
			dump_puts(" ra=");
			dump_hex8(tf[TF_RA / 4u]);
			dump_puts(" sp=");
			dump_hex8(tf[TF_SP / 4u]);
			dump_puts(" mepc=");
			dump_hex8(tf[TF_MEPC / 4u]);
		}
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
#if ULMK_CONFIG_ENABLE_SMP && ULMK_ARCH_HAVE_CLINT
	/* Accept CLINT MSIP reschedule IPIs on every hart. */
	__asm__ volatile("csrs mie, %0" :: "r"(1u << 3));
#endif
	(void)user_mstatus_init;
}

/* =========================================================================
 * Kernel tick — CLINT mtimecmp (per-hart), or board SYSTIMER when !CLINT
 * ========================================================================= */

#if ULMK_ARCH_HAVE_CLINT

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

#else /* !ULMK_ARCH_HAVE_CLINT — board provides SYSTIMER / similar */

void ulmk_arch_tick_init(uint32_t tick_hz)
{
	ulmk_board_tick_init(tick_hz);
}

void ulmk_arch_tick_ack(void)
{
	/*
	 * HW only.  CLIC mintstatus.MIL is dropped once at the end of
	 * riscv_clic_dispatch after the pending drain — not here — so a
	 * mid-drain tick pulse cannot leave peers masked or double-mret.
	 */
	ulmk_board_tick_ack();
}

#endif /* ULMK_ARCH_HAVE_CLINT */

uint32_t ulmk_arch_timer_wheel_cpu(void)
{
#if ULMK_ARCH_HAVE_CLINT
	/* Per-hart mtimecmp — each CPU advances its own wheel. */
	return ulmk_arch_cpu_id();
#else
	/*
	 * Shared board tick (e.g. SYSTIMER → one CLIC line).  Only CPU0
	 * calls ulmk_timer_tick; secondaries must still arm timeouts on
	 * wheel 0 so sleep / notif_wait_timeout can expire, then IPI wakes
	 * the remote thread (same path as a local expire + enqueue).
	 */
	return 0u;
#endif
}

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


/*
 * Range maintenance is delegated to the board: RISC-V has no architectural
 * cache-op CSRs, so the SoC (custom sync engine, ROM routine, …) owns the
 * recipe.  Boards that declare no cache get no-ops here.
 */
void ulmk_arch_dcache_clean(void *addr, size_t len)
{
#if ULMK_ARCH_HAS_CACHE
	ulmk_board_dcache_clean(addr, len);
#else
	(void)addr;
	(void)len;
#endif
}

void ulmk_arch_dcache_invalidate(void *addr, size_t len)
{
#if ULMK_ARCH_HAS_CACHE
	ulmk_board_dcache_invalidate(addr, len);
#else
	(void)addr;
	(void)len;
#endif
}

void ulmk_arch_dcache_clean_invalidate(void *addr, size_t len)
{
#if ULMK_ARCH_HAS_CACHE
	ulmk_board_dcache_clean(addr, len);
	ulmk_board_dcache_invalidate(addr, len);
#else
	(void)addr;
	(void)len;
#endif
}
