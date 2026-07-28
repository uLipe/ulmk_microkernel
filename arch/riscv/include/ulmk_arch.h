/* SPDX-License-Identifier: MIT */
/*
 * Architecture abstraction layer — RISC-V RV32
 * Contract: docs/arch_api_spec.md
 */

#ifndef ULMK_ARCH_H
#define ULMK_ARCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <arch_config.h>

typedef struct {
	uint32_t sp;
} ulmk_arch_ctx_t;

typedef uint32_t ulmk_arch_irq_key_t;

typedef struct {
	volatile uint32_t locked;
} ulmk_spinlock_t;

#define ULMK_SPINLOCK_INIT	{ 0u }

typedef struct {
	uintptr_t base;
	size_t    size;
	uint32_t  perms;
	uint8_t   type;
} ulmk_arch_region_t;

#define ULMK_REGION_CODE	0
#define ULMK_REGION_DATA	1
#define ULMK_REGION_STACK	2
#define ULMK_REGION_HEAP	3
#define ULMK_REGION_PERIPH	4
#define ULMK_REGION_SHARED	5

ulmk_arch_irq_key_t ulmk_arch_cpu_irq_save(void);
void              ulmk_arch_cpu_irq_restore(ulmk_arch_irq_key_t key);
void              ulmk_arch_cpu_irq_enable(void);
void              ulmk_arch_cpu_irq_disable(void);
void              ulmk_arch_cpu_idle(void);
void              ulmk_arch_cpu_halt(void);
uint32_t          ulmk_arch_cpu_clz(uint32_t val);
uint32_t          ulmk_arch_cpu_id(void);

void              ulmk_arch_spin_lock(ulmk_spinlock_t *lock);
void              ulmk_arch_spin_unlock(ulmk_spinlock_t *lock);
ulmk_arch_irq_key_t ulmk_arch_spin_lock_irqsave(ulmk_spinlock_t *lock);
void              ulmk_arch_spin_unlock_irqrestore(ulmk_spinlock_t *lock,
						    ulmk_arch_irq_key_t key);

void              ulmk_arch_send_ipi(uint32_t cpu_id);
void              ulmk_arch_ipi_clear_self(void);
void              ulmk_arch_ipi_note_enter(void);
void              ulmk_arch_ipi_pulse_self(void);
void              ulmk_arch_secondary_init(void);
void              ulmk_arch_secondary_mark_ready(void);
void              ulmk_arch_start_secondary(uint32_t cpu_id, void (*entry)(void));
void              ulmk_arch_smp_mark_ready(void);
void              ulmk_arch_smp_park(void);

void     ulmk_arch_cycle_enable(void);
uint32_t ulmk_arch_cycle_read(void);

void ulmk_arch_csa_pool_init(uintptr_t pool_base, size_t pool_size);

void ulmk_arch_ctx_init(ulmk_arch_ctx_t *ctx,
		      void (*entry)(void *arg), void *arg,
		      uintptr_t stack_top, ulmk_privilege_t priv);

void ulmk_arch_ctx_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to);
void ulmk_arch_ctx_free(ulmk_arch_ctx_t *ctx);

#define ULMK_SCHED_SWITCH_COOP		0u
#define ULMK_SCHED_SWITCH_PREEMPT_ISR	1u

bool ulmk_arch_sched_isr_preempt_deferred(void);
void ulmk_arch_sched_switch(ulmk_arch_ctx_t *from, const ulmk_arch_ctx_t *to,
			    unsigned int flags);

void ulmk_arch_mpu_init(void);

/* Cache maintenance — no-op stubs (API parity with ARM). */
void ulmk_arch_cache_enable(void);
void ulmk_arch_dcache_clean_all(void);
void ulmk_arch_dcache_invalidate_all(void);
void ulmk_arch_dcache_clean_invalidate_all(void);
void ulmk_arch_icache_invalidate_all(void);
void ulmk_arch_dcache_clean(void *addr, size_t len);
void ulmk_arch_dcache_invalidate(void *addr, size_t len);
void ulmk_arch_dcache_clean_invalidate(void *addr, size_t len);
void ulmk_arch_mpu_enable(void);
void ulmk_arch_mpu_disable(void);
void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			   uint8_t count);
void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			uint8_t prs);
bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms);

void ulmk_arch_irq_vectors_init(uintptr_t btv, uintptr_t biv, uintptr_t isp_top);
void ulmk_arch_irq_src_configure(uint8_t srpn, uint8_t priority, uint8_t cpu_id);
void ulmk_arch_irq_src_register(uint8_t srpn, uint32_t src_reg_addr);
void ulmk_arch_irq_src_enable(uint8_t srpn);
void ulmk_arch_irq_src_disable(uint8_t srpn);
void ulmk_arch_irq_src_ack(uint8_t srpn);
bool ulmk_arch_irq_src_is_pending(uint8_t srpn);
void ulmk_arch_irq_src_trigger(uint8_t srpn);

bool ulmk_arch_irq_attach_call(ulmk_irq_attach_fn_t fn, void *data,
			       const ulmk_arch_region_t *regions,
			       uint8_t count);

uint32_t ulmk_arch_atomic_cas(volatile uint32_t *ptr,
			    uint32_t expected, uint32_t desired);
uint32_t ulmk_arch_atomic_add(volatile uint32_t *ptr, uint32_t val);

/*
 * ulmk_kern_start - common C runtime bring-up (kernel/init/init.c); no return.
 * Entered from startup.S after the CPU prologue (stack) with interrupts off;
 * relocates .data, clears BSS, then calls board_init/arch_init/kern_main.
 */
void ulmk_kern_start(void);

/*
 * Board-level hardware setup, called by ulmk_kern_start() before the .data
 * copy.  A definition must always be linked (board board_services.c or
 * stub/board_init_stub.c).  Not weak: under SDK static-library packaging a
 * weak reference fails to pull the board definition from the archive and
 * silently resolves to address 0, crashing at boot.
 */
void ulmk_board_init(void);

void ulmk_arch_init(ulmk_boot_info_t *info);

/* Periodic kernel tick (CLINT mtimecmp).  Called by kernel only. */
void ulmk_arch_tick_init(uint32_t tick_hz);
void ulmk_arch_tick_ack(void);
uint32_t ulmk_arch_timer_wheel_cpu(void);

void ulmk_arch_syscall_entry(void);
void ulmk_arch_trap_entry(uint8_t trap_class, uint8_t tin);
void ulmk_arch_trap_dump(uint8_t trap_class, uint8_t tin);

void ulmk_printk_char_out(char c);

void ulmk_kern_irq_dispatch(uint8_t srpn);
void ulmk_kern_ipi_resched(void);
void ulmk_kern_timer_tick(void);
void ulmk_kern_sched_dispatch(bool from_isr);
void ulmk_kern_secondary_main(void);
uint32_t ulmk_kern_syscall_ret_resolve(uint32_t ret);
uint32_t ulmk_kern_trap_syscall(uint8_t tin, uint32_t args[4]);
void ulmk_kern_trap_recoverable(void);
void ulmk_kern_trap_panic(void);
bool ulmk_irq_in_attach(void);
void ulmk_kern_trap_mpu_restore(void);
void ulmk_kern_main(const ulmk_boot_info_t *info);

#endif /* ULMK_ARCH_H */
