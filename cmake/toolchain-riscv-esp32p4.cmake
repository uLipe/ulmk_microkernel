# cmake/toolchain-riscv-esp32p4.cmake — Espressif riscv32-esp-elf for ESP32-P4.
#
# Usage (host + IDF toolchain on PATH):
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv-esp32p4.cmake \
#         -DULMK_CHIP_DIR=../ulmk_boards/esp32p4_ev_function ...

include("${CMAKE_CURRENT_LIST_DIR}/board_resolve.cmake")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

set(_ULMK_RISCV_PREFIX "riscv32-esp-elf"
	CACHE STRING "Espressif RISC-V cross toolchain prefix")

find_program(CMAKE_C_COMPILER   "${_ULMK_RISCV_PREFIX}-gcc"    REQUIRED)
find_program(CMAKE_ASM_COMPILER "${_ULMK_RISCV_PREFIX}-gcc"    REQUIRED)
find_program(CMAKE_OBJCOPY      "${_ULMK_RISCV_PREFIX}-objcopy" REQUIRED)
find_program(CMAKE_SIZE         "${_ULMK_RISCV_PREFIX}-size"    REQUIRED)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT
	"-ffreestanding -fno-builtin -Wall -Wextra -nostdlib")
set(CMAKE_ASM_FLAGS_INIT "-ffreestanding")
set(CMAKE_EXE_LINKER_FLAGS_INIT
	"-nostartfiles -nostdlib -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
