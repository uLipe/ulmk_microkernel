/* Stub ulmk_arch.h for host-side unit tests */
#ifndef ULMK_ARCH_H
#define ULMK_ARCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct { uint32_t pcxi; } ulmk_arch_ctx_t;

/* IRQ control — no-op stubs for single-threaded host tests */
typedef uint32_t ulmk_arch_irq_key_t;
static inline ulmk_arch_irq_key_t ulmk_arch_cpu_irq_save(void)       { return 0u; }
static inline void ulmk_arch_cpu_irq_restore(ulmk_arch_irq_key_t k)  { (void)k; }
static inline void ulmk_arch_cpu_irq_enable(void)                   { }
static inline void ulmk_arch_cpu_irq_disable(void)                  { }

#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU	1
#endif

typedef struct { volatile uint32_t locked; } ulmk_spinlock_t;
#define ULMK_SPINLOCK_INIT	{ 0u }

static inline uint32_t ulmk_arch_cpu_id(void) { return 0u; }
static inline void ulmk_arch_spin_lock(ulmk_spinlock_t *l)
{ while (__sync_lock_test_and_set(&l->locked, 1u)) ; }
static inline void ulmk_arch_spin_unlock(ulmk_spinlock_t *l)
{ __sync_lock_release(&l->locked); }
static inline ulmk_arch_irq_key_t ulmk_arch_spin_lock_irqsave(ulmk_spinlock_t *l)
{ ulmk_arch_irq_key_t k = ulmk_arch_cpu_irq_save(); ulmk_arch_spin_lock(l); return k; }
static inline void ulmk_arch_spin_unlock_irqrestore(ulmk_spinlock_t *l, ulmk_arch_irq_key_t k)
{ ulmk_arch_spin_unlock(l); ulmk_arch_cpu_irq_restore(k); }
static inline void ulmk_arch_send_ipi(uint32_t c) { (void)c; }

/* MPU region descriptor */
#define ULMK_ARCH_MAX_REGIONS	12

typedef struct {
	uintptr_t base;
	size_t    size;
	uint32_t  perms;
	uint8_t   type;
} ulmk_arch_region_t;

#define ULMK_REGION_CODE		0
#define ULMK_REGION_DATA		1
#define ULMK_REGION_STACK		2
#define ULMK_REGION_HEAP		3
#define ULMK_REGION_PERIPH	4
#define ULMK_REGION_SHARED	5
#define ULMK_REGION_GRANT	6

/* Forward declarations — test file provides implementations */
void ulmk_arch_ctx_switch(ulmk_arch_ctx_t *from, ulmk_arch_ctx_t *to);
void ulmk_arch_ctx_init(ulmk_arch_ctx_t *ctx,
		      void (*entry)(void *), void *arg,
		      uintptr_t stack_top, int priv);
void ulmk_arch_ctx_free(ulmk_arch_ctx_t *ctx);
void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			uint8_t prs);

#endif /* ULMK_ARCH_H */
