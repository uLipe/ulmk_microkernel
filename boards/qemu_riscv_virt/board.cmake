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
    board_hil.c
)

# board_config.h / board_timer.h for components (silicon_* cases).
set(ULMK_BOARD_INCLUDES "${CMAKE_CURRENT_LIST_DIR}")

set(UL_BOARD_QEMU_MACHINE "virt")
set(UL_BOARD_QEMU_CPU "rv32")
set(UL_BOARD_QEMU_EXTRA "-bios" "none" "-m" "16M")
# Appended by dev.py when the ELF was built with ULMK_CONFIG_ENABLE_SMP=1.
set(UL_BOARD_QEMU_SMP_EXTRA "-smp" "2")

# The virt harts implement S-mode, so satp and Sv32 are available:
# -DULMK_CONFIG_MMU=1 (dev.py --enable-mmu) protects with page tables.
set(ULMK_BOARD_HAVE_SV32 1)

# Demos call ulmk_board_sim_exit() to end the run (board_sim_exit.c).
set(ULMK_CONFIG_SIM_EXIT 1 CACHE STRING
	"Board can stop the simulator; demos end instead of idling")
