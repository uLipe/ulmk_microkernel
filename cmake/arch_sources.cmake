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
		${ULMK_ARCH_DIR}/irq.c
		${ULMK_ARCH_DIR}/mpu_v7m.c
		${ULMK_ARCH_DIR}/mpu_v8m.c
		${ULMK_ARCH_DIR}/ctx_switch.S
		${ULMK_ARCH_DIR}/trap.S)
	set(ULMK_ARCH_EXE_SOURCES
		${ULMK_ARCH_DIR}/startup.S
		${ULMK_ARCH_DIR}/vectors.S)
elseif(ULMK_ARCH STREQUAL "c29")
	set(ULMK_ARCH_KERNEL_SOURCES
		${ULMK_ARCH_DIR}/arch.c
		${ULMK_ARCH_DIR}/irq.c
		${ULMK_ARCH_DIR}/mpu.c
		${ULMK_ARCH_DIR}/smp.c
		${ULMK_ARCH_DIR}/ctx_switch.S)
	set(ULMK_ARCH_EXE_SOURCES
		${ULMK_ARCH_DIR}/startup.S
		${ULMK_ARCH_DIR}/trap.S
		${ULMK_ARCH_DIR}/cert_placeholder.c)
	# Secondary reset stubs only when SMP — otherwise orphan .cpu2_* sections
	# after resetvector punch a cert-BIN hole (objcopy strip leaves stale
	# PT_LOADs) and flash POR goes silent.
	if("${ULMK_CONFIG_ENABLE_SMP}" STREQUAL "1")
		list(APPEND ULMK_ARCH_EXE_SOURCES
			${ULMK_ARCH_DIR}/startup_cpu2.S
			${ULMK_ARCH_DIR}/startup_cpu3.S)
	endif()
else()
	message(FATAL_ERROR "Unsupported ULMK_ARCH=${ULMK_ARCH}")
endif()
