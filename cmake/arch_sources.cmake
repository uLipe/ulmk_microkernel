# cmake/arch_sources.cmake — arch-specific source lists (included after arch.cmake).

if(ULMK_ARCH STREQUAL "tricore")
	set(ULMK_ARCH_KERNEL_SOURCES
		${ULMK_ARCH_DIR}/arch.c
		${ULMK_ARCH_DIR}/smp.c
		${ULMK_ARCH_DIR}/ctx_switch.S)
	set(ULMK_ARCH_EXE_SOURCES
		${ULMK_ARCH_DIR}/startup.S
		${ULMK_ARCH_DIR}/board_wdt_early_stub.S
		${ULMK_ARCH_DIR}/secondary.S
		${ULMK_ARCH_DIR}/vectors.S)
elseif(ULMK_ARCH STREQUAL "riscv")
	set(ULMK_ARCH_KERNEL_SOURCES
		${ULMK_ARCH_DIR}/arch.c
		${ULMK_ARCH_DIR}/smp.c
		${ULMK_ARCH_DIR}/irq.c
		${ULMK_ARCH_DIR}/irq_clint.c
		${ULMK_ARCH_DIR}/irq_clic.c
		${ULMK_ARCH_DIR}/irq_plic.c
		${ULMK_ARCH_DIR}/ctx_switch.S
		${ULMK_ARCH_DIR}/trap.S)
	set(ULMK_ARCH_EXE_SOURCES
		${ULMK_ARCH_DIR}/startup.S)
elseif(ULMK_ARCH STREQUAL "arm")
	set(ULMK_ARCH_KERNEL_SOURCES
		${ULMK_ARCH_DIR}/arch.c
		${ULMK_ARCH_DIR}/cache.c
		${ULMK_ARCH_DIR}/irq.c
		${ULMK_ARCH_DIR}/mpu_v7m.c
		${ULMK_ARCH_DIR}/mpu_v8m.c
		${ULMK_ARCH_DIR}/ctx_switch.S
		${ULMK_ARCH_DIR}/trap.S)
	set(ULMK_ARCH_EXE_SOURCES
		${ULMK_ARCH_DIR}/startup.S
		${ULMK_ARCH_DIR}/vectors.S)
else()
	message(FATAL_ERROR "Unsupported ULMK_ARCH=${ULMK_ARCH}")
endif()
