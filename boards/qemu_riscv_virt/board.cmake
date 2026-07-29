# SPDX-License-Identifier: MIT
#
# Board descriptor for QEMU rv32 virt platform.
# boards/qemu_riscv_virt/board.cmake
#
# SoC addresses: boards/qemu_riscv_virt/board_config.h

set(UL_BOARD_ARCH "riscv")
set(ULMK_BOARD_CPU  "rv32imac")

if(DEFINED CMAKE_C_FLAGS)
    string(APPEND CMAKE_C_FLAGS " -march=rv32imac_zicsr_zifencei -mabi=ilp32")
    string(APPEND CMAKE_ASM_FLAGS " -march=rv32imac_zicsr_zifencei -mabi=ilp32")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -march=rv32imac_zicsr_zifencei -mabi=ilp32")
endif()

set(ULMK_BOARD_SOURCES
    qemu_console.c
    board_console.c
    board_timer.c
    board_services.c
    board_sim_exit.c
)

set(UL_BOARD_QEMU_MACHINE "virt")
set(UL_BOARD_QEMU_CPU "rv32")
set(UL_BOARD_QEMU_EXTRA "-bios" "none" "-m" "16M")

# Demos call ulmk_board_sim_exit() to end the run (board_sim_exit.c).
set(ULMK_CONFIG_SIM_EXIT 1 CACHE STRING
	"Board can stop the simulator; demos end instead of idling")
