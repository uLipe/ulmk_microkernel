# SPDX-License-Identifier: MIT
#
# Board descriptor for QEMU mps2-an505 (Cortex-M33, ARMv8-M mainline).
# boards/qemu_mps2_an505/board.cmake
#
# SoC addresses: boards/qemu_mps2_an505/board_config.h

set(UL_BOARD_ARCH "arm")
set(ULMK_BOARD_CPU "cortex-m33")

# Cortex-M33 single-precision FPU; ABI follows ULMK_CONFIG_FPU.
include("${_ULMK_REPO_ROOT}/cmake/arm_fpu.cmake")
ulmk_arm_float_flags(cortex-m33 fpv5-sp-d16)

set(ULMK_BOARD_SOURCES
    qemu_console.c
    board_console.c
    board_timer.c
    board_services.c
    board_init.c
    board_sim_exit.c
)

set(UL_BOARD_QEMU_MACHINE "mps2-an505")
set(UL_BOARD_QEMU_CPU "")
# userspace=on: the demo calls SYS_EXIT at driver privilege, and QEMU refuses
# semihosting from unprivileged code unless told otherwise.
set(UL_BOARD_QEMU_EXTRA "-semihosting-config" "enable=on,target=native,userspace=on")

# Demos call ulmk_board_sim_exit() to end the run (board_sim_exit.c).
set(ULMK_CONFIG_SIM_EXIT 1 CACHE STRING
	"Board can stop the simulator; demos end instead of idling")
