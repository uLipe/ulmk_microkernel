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

#define PMP_R	0x01u
#define PMP_W	0x02u
#define PMP_X	0x04u
#define PMP_A_TOR	0x08u
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

static inline uint32_t read_pmpcfg2(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, pmpcfg2" : "=r"(val));
	return val;
}

static inline void write_pmpcfg2(uint32_t val)
{
	__asm__ volatile("csrw pmpcfg2, %0" :: "r"(val));
}

static inline uint32_t read_pmpcfg3(void)
{
	uint32_t val;

	__asm__ volatile("csrr %0, pmpcfg3" : "=r"(val));
	return val;
}

static inline void write_pmpcfg3(uint32_t val)
{
	__asm__ volatile("csrw pmpcfg3, %0" :: "r"(val));
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
	case 8: __asm__ volatile("csrw pmpaddr8, %0" :: "r"(val)); break;
	case 9: __asm__ volatile("csrw pmpaddr9, %0" :: "r"(val)); break;
	case 10: __asm__ volatile("csrw pmpaddr10, %0" :: "r"(val)); break;
	case 11: __asm__ volatile("csrw pmpaddr11, %0" :: "r"(val)); break;
	case 12: __asm__ volatile("csrw pmpaddr12, %0" :: "r"(val)); break;
	case 13: __asm__ volatile("csrw pmpaddr13, %0" :: "r"(val)); break;
	case 14: __asm__ volatile("csrw pmpaddr14, %0" :: "r"(val)); break;
	case 15: __asm__ volatile("csrw pmpaddr15, %0" :: "r"(val)); break;
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
	uint8_t  byte;

	if (idx >= ULMK_ARCH_PMP_NUM && ULMK_ARCH_PMP_NUM != 0u)
		return;
	if (idx >= 16u)
		return;

	byte = idx & 3u;
	if (idx < 4u) {
		v = read_pmpcfg0();
		v = (v & ~(0xFFu << (byte * 8u))) |
		    ((uint32_t)cfg << (byte * 8u));
		write_pmpcfg0(v);
	} else if (idx < 8u) {
		v = read_pmpcfg1();
		v = (v & ~(0xFFu << (byte * 8u))) |
		    ((uint32_t)cfg << (byte * 8u));
		write_pmpcfg1(v);
	} else if (idx < 12u) {
		v = read_pmpcfg2();
		v = (v & ~(0xFFu << (byte * 8u))) |
		    ((uint32_t)cfg << (byte * 8u));
		write_pmpcfg2(v);
	} else {
		v = read_pmpcfg3();
		v = (v & ~(0xFFu << (byte * 8u))) |
		    ((uint32_t)cfg << (byte * 8u));
		write_pmpcfg3(v);
	}
}

static void pmp_clear_all(void)
{
	uint8_t i;
	uint8_t n = ULMK_ARCH_PMP_NUM;
	uint32_t cfg;

	if (n == 0u)
		n = 16u;

	/*
	 * Do not touch mseccfg (may be unimplemented on some SoCs).
	 * Skip Locked entries — boot firmware owns those.
	 */
	for (i = 0u; i < n && i < 16u; i++) {
		cfg = 0u;
		if (i < 4u) {
			uint32_t v;
			__asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
			cfg = (v >> ((i & 3u) * 8u)) & 0xFFu;
		} else if (i < 8u) {
			uint32_t v;
			__asm__ volatile("csrr %0, pmpcfg1" : "=r"(v));
			cfg = (v >> ((i & 3u) * 8u)) & 0xFFu;
		} else if (i < 12u) {
			uint32_t v;
			__asm__ volatile("csrr %0, pmpcfg2" : "=r"(v));
			cfg = (v >> ((i & 3u) * 8u)) & 0xFFu;
		} else {
			uint32_t v;
			__asm__ volatile("csrr %0, pmpcfg3" : "=r"(v));
			cfg = (v >> ((i & 3u) * 8u)) & 0xFFu;
		}
		if ((cfg & 0x80u) != 0u) /* PMP_L */
			continue;
		pmp_write_cfg(i, 0u);
		pmp_write_addr(i, 0u);
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
	uintptr_t hi;
	uintptr_t aligned;
	uint32_t  addr;

	if (idx >= 16u || size == 0u)
		return;
	if (ULMK_ARCH_PMP_NUM != 0u && idx >= ULMK_ARCH_PMP_NUM)
		return;

	hi = base + size;
	napot = napot_round_size(size);
	/*
	 * Aligning base down can push the top of the NAPOT window below
	 * @p hi — grow until [aligned, aligned+napot) covers the range.
	 */
	for (;;) {
		aligned = base & ~(napot - 1u);
		if (aligned + napot >= hi)
			break;
		if (napot >= (uintptr_t)0x80000000u)
			return;
		napot <<= 1u;
	}
	base = aligned;
	/*
	 * NAPOT encodes size as a run of low ones: pmpaddr = base/4 with the
	 * bottom log2(size/8) bits set.  Using size/4 sets one bit too many,
	 * which doubles the window — and where base/4 already ends in ones the
	 * two runs merge and the region grows by orders of magnitude.
	 */
	addr = pmp_addr_encode(base) | ((uint32_t)(napot >> 3u) - 1u);
	pmp_write_cfg(idx, 0u);
	pmp_write_addr(idx, addr);
	pmp_write_cfg(idx, perm | PMP_A_NAPOT);
}

/*
 * TOR on slot @p idx: range [lo, hi).  Slot idx-1 holds the low bound
 * (OFF or previous TOR).  For idx==0, low bound is 0.
 */
static void __attribute__((unused))
pmp_set_tor(uint8_t idx, uintptr_t lo, uintptr_t hi, uint8_t perm)
{
	if (idx >= 16u || hi <= lo)
		return;
	if (ULMK_ARCH_PMP_NUM != 0u && idx >= ULMK_ARCH_PMP_NUM)
		return;

	if (idx == 0u) {
		pmp_write_cfg(0u, 0u);
		pmp_write_addr(0u, pmp_addr_encode(hi));
		pmp_write_cfg(0u, perm | PMP_A_TOR);
		return;
	}

	pmp_write_cfg(idx - 1u, 0u);
	pmp_write_addr(idx - 1u, pmp_addr_encode(lo));
	pmp_write_cfg(idx, 0u);
	pmp_write_addr(idx, pmp_addr_encode(hi));
	pmp_write_cfg(idx, perm | PMP_A_TOR);
}

/*
 * User RAM is the one window where rounding is not a rounding error: it sits
 * directly above kernel RAM, so a NAPOT window grown downwards to reach a
 * power-of-two boundary hands U-mode read/write over the kernel's own data.
 * Boards whose layout is not naturally aligned spend the slot below
 * ULMK_ARCH_PMP_URAM on a lower bound and describe the range exactly.
 */
static void pmp_set_uram(uintptr_t lo, uintptr_t hi)
{
#if ULMK_ARCH_PMP_URAM_TOR
	pmp_set_tor(ULMK_ARCH_PMP_URAM, lo, hi, PMP_R | PMP_W);
#else
	pmp_set_napot(ULMK_ARCH_PMP_URAM, lo, hi - lo, PMP_R | PMP_W);
#endif
}

static uint8_t perms_to_pmp(uint32_t perms)
{
	uint8_t p = 0u;

	if (perms & ULMK_PERM_READ)
		p |= PMP_R;
	if (perms & ULMK_PERM_WRITE)
		p |= PMP_W;
	if (perms & ULMK_PERM_EXEC)
		p |= PMP_X;
	return p;
}

int ulmk_arch_pmp_set_napot(uint8_t slot, uintptr_t base, size_t size,
			    uint32_t perms)
{
	if (ULMK_ARCH_PMP_NUM == 0u)
		return ULMK_ENOTSUP;
	if (slot >= ULMK_ARCH_PMP_NUM || size == 0u)
		return ULMK_EINVAL;
	pmp_set_napot(slot, base, size, perms_to_pmp(perms));
	return ULMK_OK;
}

int ulmk_arch_pmp_map_temp(uintptr_t base, size_t size, uint32_t perms)
{
	static uint8_t next;

	if (ULMK_ARCH_PMP_NUM == 0u)
		return -1;
	if (size == 0u)
		return -1;

	if (next == 0u)
		next = (uint8_t)ULMK_ARCH_PMP_TEMP0;
	else if (next == (uint8_t)ULMK_ARCH_PMP_TEMP0)
		next = (uint8_t)ULMK_ARCH_PMP_TEMP1;
	else
		next = (uint8_t)ULMK_ARCH_PMP_TEMP0;

	pmp_set_napot(next, base, size, perms_to_pmp(perms));
	return (int)next;
}

void ulmk_arch_pmp_unmap_temp(int slot)
{
	if (slot < 0 || ULMK_ARCH_PMP_NUM == 0u)
		return;
	if ((uint8_t)slot >= ULMK_ARCH_PMP_NUM)
		return;
	pmp_write_cfg((uint8_t)slot, 0u);
	pmp_write_addr((uint8_t)slot, 0u);
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

/*
 * Board-supplied protection entries.  Reapplied on every rebuild of the
 * protection state, so the board's implementation has to be idempotent.
 */
static inline void pmp_board_extra(void)
{
#if ULMK_CONFIG_BOARD_PMP_EXTRA
	ulmk_board_pmp_extra();
#endif
}

/*
 * M-mode is not subject to unlocked PMP entries, so the kernel needs no
 * layout of its own: every entry describes what the current U-mode thread
 * may touch.  Static windows (user text, user .data/.bss, MMIO) and the
 * pinned stack are written on a switch; everything else the thread maps is
 * loaded into the dynamic slots on its first access fault.
 */
#define PMP_BIT(n)	(1u << (n))

#if ULMK_ARCH_PMP_URAM_TOR
#define PMP_URAM_SLOTS	(PMP_BIT(ULMK_ARCH_PMP_URAM) | \
			 PMP_BIT(ULMK_ARCH_PMP_URAM - 1))
#else
#define PMP_URAM_SLOTS	PMP_BIT(ULMK_ARCH_PMP_URAM)
#endif

#define PMP_FREE_SLOTS	((uint32_t)(((1ull << ULMK_ARCH_PMP_NUM) - 1u) & \
			 ~(uint64_t)(ULMK_ARCH_PMP_RESERVED_MASK | \
				     PMP_BIT(ULMK_ARCH_PMP_UTEXT) | PMP_URAM_SLOTS | \
				     PMP_BIT(ULMK_ARCH_PMP_MMIO) | \
				     PMP_BIT(ULMK_ARCH_PMP_TEMP0) | \
				     PMP_BIT(ULMK_ARCH_PMP_TEMP1))))

extern uint8_t _ulmk_user_text_start[];
extern uint8_t _ulmk_user_text_end[];
extern uint8_t _ulmk_user_ram_start[];
extern uint8_t _ulmk_user_pool_start[];
extern uintptr_t _ulmk_mem_periph_base[];
extern uintptr_t _ulmk_mem_periph_end[];

/*
 * PMP CSRs are per-hart, and so is the layout they hold.  @pinned is the
 * key: it is the TCB's own pinned list, so the same thread coming back
 * keeps its dynamic windows and any other thread gets a fresh layout.
 */
struct pmp_cpu {
	const ulmk_arch_region_t *pinned;
	uint32_t dyn;
	uint8_t  next;
};

static struct pmp_cpu g_pmp_cpu[ULMK_ARCH_NUM_CPU];

static struct pmp_cpu *pmp_cpu(void)
{
	uint32_t cpu = ulmk_arch_cpu_id();

	return &g_pmp_cpu[cpu < (uint32_t)ULMK_ARCH_NUM_CPU ? cpu : 0u];
}

static void pmp_off(uint8_t idx)
{
	pmp_write_cfg(idx, 0u);
	pmp_write_addr(idx, 0u);
}

static void pmp_static_user(void)
{
	uintptr_t lo;
	uintptr_t hi;

	lo = (uintptr_t)_ulmk_user_text_start;
	hi = (uintptr_t)_ulmk_user_text_end;
	if (hi > lo)
		pmp_set_napot(ULMK_ARCH_PMP_UTEXT, lo, hi - lo, PMP_R | PMP_X);

	/* The pool above user .data/.bss is kernel heap: areas only. */
	lo = (uintptr_t)_ulmk_user_ram_start;
	hi = (uintptr_t)_ulmk_user_pool_start;
	if (hi > lo)
		pmp_set_uram(lo, hi);

	lo = (uintptr_t)_ulmk_mem_periph_base;
	hi = (uintptr_t)_ulmk_mem_periph_end;
	if (hi > lo)
		pmp_set_napot(ULMK_ARCH_PMP_MMIO, lo, hi - lo, PMP_R | PMP_W);
}

static bool pmp_covered_by_uram(const ulmk_arch_region_t *r)
{
	return r->base >= (uintptr_t)_ulmk_user_ram_start &&
	       r->base + r->size <= (uintptr_t)_ulmk_user_pool_start;
}

static void pmp_user_layout(struct pmp_cpu *c,
			    const ulmk_arch_region_t *pinned, uint8_t count)
{
	uint32_t free = PMP_FREE_SLOTS;
	uint8_t  slot;
	uint8_t  i;

	pmp_clear_all();
	pmp_static_user();

	for (i = 0u; pinned && i < count && free; i++) {
		if (pmp_covered_by_uram(&pinned[i]))
			continue;
		slot = (uint8_t)__builtin_ctz(free);
		free &= ~PMP_BIT(slot);
		pmp_set_napot(slot, pinned[i].base, pinned[i].size,
			      perms_to_pmp(pinned[i].perms));
	}

	pmp_board_extra();
	c->dyn  = free;
	c->next = 0u;
}

void ulmk_arch_mpu_init(void)
{
	if (ULMK_ARCH_PMP_NUM == 0u)
		return;
	pmp_clear_all();
	pmp_board_extra();
}

void ulmk_arch_mpu_enable(void)
{
}

void ulmk_arch_mpu_disable(void)
{
	if (ULMK_ARCH_PMP_NUM == 0u)
		return;
	pmp_clear_all();
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
	struct pmp_cpu *c;

	if (ULMK_ARCH_PMP_NUM == 0u)
		return;

	c = pmp_cpu();
	/*
	 * A kernel thread cannot see the entries, so leave them in place but
	 * forget the owner: the next user thread, even the same one, gets a
	 * fresh layout because its areas may have changed meanwhile.
	 */
	if (prs == ULMK_ARCH_PRS_KERNEL) {
		c->pinned = NULL;
		return;
	}
	if (c->pinned == regions && regions)
		return;

	pmp_user_layout(c, regions, count);
	c->pinned = regions;
}

void ulmk_arch_mpu_flush(void)
{
	struct pmp_cpu *c;
	uint32_t m;

	if (ULMK_ARCH_PMP_NUM == 0u)
		return;

	c = pmp_cpu();
	for (m = c->dyn; m; m &= m - 1u)
		pmp_off((uint8_t)__builtin_ctz(m));
}

static bool napot_encode(const ulmk_arch_region_t *w, uint32_t *addr)
{
	if (w->size < 8u || (w->size & (w->size - 1u)) ||
	    (w->base & (w->size - 1u)))
		return false;
	*addr = pmp_addr_encode(w->base) | ((uint32_t)(w->size >> 3u) - 1u);
	return true;
}

static uint32_t pmp_read_addr(uint8_t idx)
{
	uint32_t v = 0u;

	switch (idx) {
	case 0: __asm__ volatile("csrr %0, pmpaddr0" : "=r"(v)); break;
	case 1: __asm__ volatile("csrr %0, pmpaddr1" : "=r"(v)); break;
	case 2: __asm__ volatile("csrr %0, pmpaddr2" : "=r"(v)); break;
	case 3: __asm__ volatile("csrr %0, pmpaddr3" : "=r"(v)); break;
	case 4: __asm__ volatile("csrr %0, pmpaddr4" : "=r"(v)); break;
	case 5: __asm__ volatile("csrr %0, pmpaddr5" : "=r"(v)); break;
	case 6: __asm__ volatile("csrr %0, pmpaddr6" : "=r"(v)); break;
	case 7: __asm__ volatile("csrr %0, pmpaddr7" : "=r"(v)); break;
	case 8: __asm__ volatile("csrr %0, pmpaddr8" : "=r"(v)); break;
	case 9: __asm__ volatile("csrr %0, pmpaddr9" : "=r"(v)); break;
	case 10: __asm__ volatile("csrr %0, pmpaddr10" : "=r"(v)); break;
	case 11: __asm__ volatile("csrr %0, pmpaddr11" : "=r"(v)); break;
	case 12: __asm__ volatile("csrr %0, pmpaddr12" : "=r"(v)); break;
	case 13: __asm__ volatile("csrr %0, pmpaddr13" : "=r"(v)); break;
	case 14: __asm__ volatile("csrr %0, pmpaddr14" : "=r"(v)); break;
	case 15: __asm__ volatile("csrr %0, pmpaddr15" : "=r"(v)); break;
	default: break;
	}
	return v;
}

bool ulmk_arch_mpu_load(const ulmk_arch_region_t *win)
{
	struct pmp_cpu *c;
	uint32_t addr;
	uint32_t m;
	uint8_t  n;
	uint8_t  slot;
	uint8_t  i;

	if (ULMK_ARCH_PMP_NUM == 0u || !napot_encode(win, &addr))
		return false;

	c = pmp_cpu();
	if (!c->dyn)
		return false;

	/* The window is live yet the access faulted: a real violation. */
	for (m = c->dyn; m; m &= m - 1u) {
		if (pmp_read_addr((uint8_t)__builtin_ctz(m)) == addr)
			return false;
	}

	n = (uint8_t)__builtin_popcount(c->dyn);
	m = c->dyn;
	for (i = 0u; i < c->next % n; i++)
		m &= m - 1u;
	slot = (uint8_t)__builtin_ctz(m);
	c->next = (uint8_t)((c->next + 1u) % n);

	pmp_write_cfg(slot, 0u);
	pmp_write_addr(slot, addr);
	pmp_write_cfg(slot, perms_to_pmp(win->perms) | PMP_A_NAPOT);
	return true;
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
