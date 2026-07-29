# SPDX-License-Identifier: MIT
#
# Board descriptor for QEMU mps2-an500 (Cortex-M7, ARMv7-M).
# boards/qemu_mps2_an500/board.cmake
#
# SoC addresses: boards/qemu_mps2_an500/board_config.h

set(UL_BOARD_ARCH "arm")
set(ULMK_BOARD_CPU "cortex-m7")

# Cortex-M7 single-precision FPU; ABI follows ULMK_CONFIG_FPU.
include("${_ULMK_REPO_ROOT}/cmake/arm_fpu.cmake")
ulmk_arm_float_flags(cortex-m7 fpv5-sp-d16)

set(ULMK_BOARD_SOURCES
    qemu_console.c
    board_console.c
    board_timer.c
    board_services.c
)

set(UL_BOARD_QEMU_MACHINE "mps2-an500")
set(UL_BOARD_QEMU_CPU "")
set(UL_BOARD_QEMU_EXTRA "")
