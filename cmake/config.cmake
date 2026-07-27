# cmake/config.cmake
# Kernel static configuration — user-tunable knobs exposed as cache variables.
# Canonical defaults and validation live in tools/gen_config.py (the single
# generator used by both this build and the integration-test Makefiles).
# Full specification: docs/build_system_spec.md §10

set(ULMK_CONFIG_MAX_IRQ_BINDINGS 16 CACHE STRING "Max IRQ-to-notification bindings")
set(ULMK_CONFIG_DEBUG_PRINTK     1  CACHE STRING "Enable kernel printk (0 = production no-op)")
set(ULMK_CONFIG_SYSCALL_WCET     0  CACHE STRING
	"Syscall cycle-counter slot (0=off, 1=WCET HIL / silicon_wcet)")
set(ULMK_CONFIG_ENABLE_SMP       0  CACHE STRING
	"Enable SMP (0=UP, 1=multi-CPU; requires ULMK_ARCH_NUM_CPU>1)")
set(ULMK_CONFIG_TICK_HZ          1000 CACHE STRING
	"Kernel timing-wheel tick rate in Hz (default 1000)")
set(ULMK_CONFIG_IRQ_ATTACH       0  CACHE STRING
	"Enable ulmk_irq_attach (0=off/ENOTSUP, 1=DANGEROUS ISR userspace callbacks)")

if("${ULMK_CONFIG_ENABLE_SMP}" STREQUAL "1")
	if("${ULMK_ARCH}" STREQUAL "arm")
		message(FATAL_ERROR
			"ULMK_CONFIG_ENABLE_SMP=1 is not supported on ARM Cortex-M")
	endif()
	if("${ULMK_ARCH}" STREQUAL "c29")
		if(NOT DEFINED ULMK_BOARD_C29_DLB_WORKAROUND OR
				NOT "${ULMK_BOARD_C29_DLB_WORKAROUND}" STREQUAL "1")
			message(FATAL_ERROR
				"ULMK_CONFIG_ENABLE_SMP=1 on C29 requires board to declare "
				"ULMK_BOARD_C29_DLB_WORKAROUND=1 in board.cmake "
				"(errata SPRZ569E: shared-RAM stale reads under SMP)")
		endif()
	endif()
endif()
