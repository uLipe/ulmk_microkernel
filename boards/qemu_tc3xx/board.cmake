# SPDX-License-Identifier: MIT
#
# Board descriptor for the QEMU AURIX TC3xx emulation target.
# boards/qemu_tc3xx/board.cmake
#
# SoC addresses: boards/qemu_tc3xx/board_config.h (on the include path via
# ULMK_CHIP_DIR).  board.cmake carries CPU, sources, and QEMU machine only.

set(UL_BOARD_ARCH "tricore")
set(ULMK_BOARD_CPU  "tc39xx")

if(DEFINED CMAKE_C_FLAGS)
    string(REGEX REPLACE " -mcpu=[^ ]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE " -mcpu=[^ ]+" "" CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS}")
    string(REGEX REPLACE " -mcpu=[^ ]+" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    string(APPEND CMAKE_C_FLAGS          " -mcpu=${ULMK_BOARD_CPU}")
    string(APPEND CMAKE_ASM_FLAGS        " -mcpu=${ULMK_BOARD_CPU}")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -mcpu=${ULMK_BOARD_CPU}")
endif()

set(ULMK_BOARD_SOURCES
    qemu_console.c
    board_console.c
    board_timer.c
    board_services.c
    board_sim_exit.c
)

set(UL_BOARD_QEMU_MACHINE "KIT_AURIX_TC397B_TRB")

# Demos call ulmk_board_sim_exit() to end the run (board_sim_exit.c).
set(ULMK_CONFIG_SIM_EXIT 1 CACHE STRING
	"Board can stop the simulator; demos end instead of idling")
