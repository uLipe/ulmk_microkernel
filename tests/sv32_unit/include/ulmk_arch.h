/* host stub — the arch surface arch/riscv/mmu_sv32.c needs */
#ifndef ULMK_ARCH_H
#define ULMK_ARCH_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch_config.h>

typedef struct {
	uintptr_t base;
	size_t    size;
	uint32_t  perms;
	uint8_t   type;
} ulmk_arch_region_t;

#define ULMK_REGION_STACK	2

#define ULMK_ARCH_PRS_KERNEL	0u
#define ULMK_ARCH_PRS_USER	1u

uint32_t ulmk_arch_cpu_id(void);
void ulmk_arch_cpu_halt(void);
void ulmk_printk_char_out(char c);

void ulmk_arch_mpu_init(void);
void ulmk_arch_mpu_enable(void);
void ulmk_arch_mpu_disable(void);
void ulmk_arch_mpu_configure(uint8_t prs, const ulmk_arch_region_t *regions,
			   uint8_t count);
void ulmk_arch_mpu_switch(const ulmk_arch_region_t *regions, uint8_t count,
			uint8_t prs);
bool ulmk_arch_mpu_addr_permitted(uintptr_t addr, size_t size, uint32_t perms);
bool ulmk_arch_mpu_load(const ulmk_arch_region_t *win);
void ulmk_arch_mpu_flush(void);
#endif
