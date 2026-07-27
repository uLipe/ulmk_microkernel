# cmake/arch.cmake — resolve ULMK_ARCH from board.cmake (always follows ULMK_CHIP_DIR).

if(NOT DEFINED UL_BOARD_ARCH OR UL_BOARD_ARCH STREQUAL "")
	set(UL_BOARD_ARCH "tricore")
endif()

set(ULMK_ARCH "${UL_BOARD_ARCH}" CACHE STRING
	"Target architecture (tricore|riscv|arm|c29)" FORCE)

get_filename_component(_ULMK_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ULMK_ARCH_DIR "${_ULMK_REPO_ROOT}/arch/${ULMK_ARCH}")

if(NOT EXISTS "${ULMK_ARCH_DIR}/include/ulmk_arch.h")
	message(FATAL_ERROR "Unknown ULMK_ARCH=${ULMK_ARCH} (missing ${ULMK_ARCH_DIR})")
endif()

set(ULMK_ARCH_LINKER_DIR "${ULMK_ARCH_DIR}/linker")

if(ULMK_ARCH STREQUAL "tricore")
	set(ULMK_DEFAULT_TOOLCHAIN "${_ULMK_REPO_ROOT}/cmake/toolchain-tricore-gcc.cmake")
elseif(ULMK_ARCH STREQUAL "riscv")
	set(ULMK_DEFAULT_TOOLCHAIN "${_ULMK_REPO_ROOT}/cmake/toolchain-riscv-gcc.cmake")
elseif(ULMK_ARCH STREQUAL "arm")
	set(ULMK_DEFAULT_TOOLCHAIN "${_ULMK_REPO_ROOT}/cmake/toolchain-arm-gcc.cmake")
elseif(ULMK_ARCH STREQUAL "c29")
	set(ULMK_DEFAULT_TOOLCHAIN "${_ULMK_REPO_ROOT}/cmake/toolchain-c29-ticlang.cmake")
else()
	message(FATAL_ERROR "Unsupported ULMK_ARCH=${ULMK_ARCH}")
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
	set(CMAKE_TOOLCHAIN_FILE "${ULMK_DEFAULT_TOOLCHAIN}" CACHE FILEPATH
		"CMake toolchain file for ${ULMK_ARCH}" FORCE)
endif()
