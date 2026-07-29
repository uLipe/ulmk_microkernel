# cmake/config.cmake
# Kernel static configuration — user-tunable knobs exposed as cache variables.
# Canonical defaults and validation live in tools/gen_config.py (the single
# generator used by both this build and the integration-test Makefiles).
# Full specification: docs/build_system_spec.md §10

set(ULMK_CONFIG_MAX_IRQ_BINDINGS 16 CACHE STRING "Max IRQ-to-notification bindings")
set(ULMK_CONFIG_ROOT_STACK_SIZE 4096 CACHE STRING "Root thread stack size in bytes")
set(ULMK_CONFIG_DEBUG_PRINTK     1  CACHE STRING "Enable kernel printk (0 = production no-op)")
set(ULMK_CONFIG_SYSCALL_WCET     0  CACHE STRING
	"Syscall cycle-counter slot (0=off, 1=WCET HIL / silicon_wcet)")
set(ULMK_CONFIG_ENABLE_SMP       0  CACHE STRING
	"Enable SMP (0=UP, 1=multi-CPU; requires ULMK_ARCH_NUM_CPU>1)")
set(ULMK_CONFIG_TICK_HZ          1000 CACHE STRING
	"Kernel timing-wheel tick rate in Hz (default 1000)")
set(ULMK_CONFIG_IRQ_ATTACH       0  CACHE STRING
	"Enable ulmk_irq_attach (0=off/ENOTSUP, 1=DANGEROUS ISR userspace callbacks)")
set(ULMK_CONFIG_BOARD_IRQ_CTRL   0  CACHE STRING
	"Call the board interrupt-controller hooks on bind/unbind (0=off, 1=on)")
set(ULMK_CONFIG_BOARD_IRQ_CLAIM  0  CACHE STRING
	"Offer each IRQ to the board before generic dispatch (0=off, 1=on)")
set(ULMK_CONFIG_BOARD_PMP_EXTRA  0  CACHE STRING
	"Board adds its own memory-protection entries (0=off, 1=on)")

if("${ULMK_CONFIG_ENABLE_SMP}" STREQUAL "1")
	if("${ULMK_ARCH}" STREQUAL "arm")
		message(FATAL_ERROR
			"ULMK_CONFIG_ENABLE_SMP=1 is not supported on ARM Cortex-M")
	endif()
endif()
