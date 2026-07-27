# cmake/toolchain-c29-ticlang.cmake
# TI C29 Clang (TIClang) freestanding toolchain for ULMK.
#
# Discovery (first match wins):
#   TI_C29_CGT_ROOT / TI_CCS_ROOT / TI_INSTALL_ROOT
# Container mount convention: /ti  (see tools/dev.py)
#
# Native CMake TIClang ID needs CMake >= 3.29.  This file works with the
# project's 3.20+ minimum by forcing STATIC_LIBRARY try_compile and an
# explicit Clang ID.

include("${CMAKE_CURRENT_LIST_DIR}/board_resolve.cmake")

if(CMAKE_VERSION VERSION_LESS "3.20")
	message(FATAL_ERROR "C29 port requires CMake >= 3.20")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR c29)

# ---------------------------------------------------------------------------
# Locate C29 Clang package
# ---------------------------------------------------------------------------

function(_ulmk_c29_find_cgt out_var)
	set(_candidates "")
	if(DEFINED ENV{TI_C29_CGT_ROOT} AND NOT "$ENV{TI_C29_CGT_ROOT}" STREQUAL "")
		list(APPEND _candidates "$ENV{TI_C29_CGT_ROOT}")
	endif()
	if(DEFINED TI_C29_CGT_ROOT AND NOT "${TI_C29_CGT_ROOT}" STREQUAL "")
		list(APPEND _candidates "${TI_C29_CGT_ROOT}")
	endif()
	if(DEFINED ENV{TI_CCS_ROOT} AND NOT "$ENV{TI_CCS_ROOT}" STREQUAL "")
		file(GLOB _ccs_cgts "$ENV{TI_CCS_ROOT}/tools/compiler/ti-cgt-c29*")
		list(APPEND _candidates ${_ccs_cgts})
	endif()
	if(DEFINED TI_CCS_ROOT AND NOT "${TI_CCS_ROOT}" STREQUAL "")
		file(GLOB _ccs_cgts "${TI_CCS_ROOT}/tools/compiler/ti-cgt-c29*")
		list(APPEND _candidates ${_ccs_cgts})
	endif()
	if(DEFINED ENV{TI_INSTALL_ROOT} AND NOT "$ENV{TI_INSTALL_ROOT}" STREQUAL "")
		file(GLOB _inst_cgts
			"$ENV{TI_INSTALL_ROOT}/ccs*/ccs/tools/compiler/ti-cgt-c29*"
			"$ENV{TI_INSTALL_ROOT}/tools/compiler/ti-cgt-c29*")
		list(APPEND _candidates ${_inst_cgts})
	endif()
	if(EXISTS "/ti")
		file(GLOB _ti_cgts
			"/ti/ccs*/ccs/tools/compiler/ti-cgt-c29*"
			"/ti/tools/compiler/ti-cgt-c29*")
		list(APPEND _candidates ${_ti_cgts})
	endif()

	list(REMOVE_DUPLICATES _candidates)
	foreach(_root IN LISTS _candidates)
		if(EXISTS "${_root}/bin/c29clang")
			set(${out_var} "${_root}" PARENT_SCOPE)
			return()
		endif()
	endforeach()
	set(${out_var} "" PARENT_SCOPE)
endfunction()

_ulmk_c29_find_cgt(_ULMK_C29_CGT)
if("${_ULMK_C29_CGT}" STREQUAL "")
	message(FATAL_ERROR
		"C29 Clang not found. Set TI_C29_CGT_ROOT or TI_CCS_ROOT "
		"(or mount the TI install at /ti).")
endif()

set(TI_C29_CGT_ROOT "${_ULMK_C29_CGT}" CACHE PATH "TI C29 Clang package root" FORCE)
set(_ULMK_C29_BIN "${TI_C29_CGT_ROOT}/bin")

set(CMAKE_C_COMPILER   "${_ULMK_C29_BIN}/c29clang"   CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_ULMK_C29_BIN}/c29clang"   CACHE FILEPATH "" FORCE)
set(CMAKE_AR           "${_ULMK_C29_BIN}/c29ar"      CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${_ULMK_C29_BIN}/c29ar"      CACHE FILEPATH "" FORCE)
# LLVM-style ar: ranlib is `c29ar s <archive>`, not bare `c29ar <archive>`.
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_AR> s <TARGET>")
set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_AR> s <TARGET>")
find_program(CMAKE_OBJCOPY NAMES c29objcopy PATHS "${_ULMK_C29_BIN}" NO_DEFAULT_PATH REQUIRED)
find_program(CMAKE_OBJDUMP NAMES c29objdump PATHS "${_ULMK_C29_BIN}" NO_DEFAULT_PATH REQUIRED)
find_program(CMAKE_SIZE    NAMES c29size    PATHS "${_ULMK_C29_BIN}" NO_DEFAULT_PATH REQUIRED)
find_program(CMAKE_NM      NAMES c29nm      PATHS "${_ULMK_C29_BIN}" NO_DEFAULT_PATH)
find_program(CMAKE_READELF NAMES c29readelf PATHS "${_ULMK_C29_BIN}" NO_DEFAULT_PATH)

set(CMAKE_C_COMPILER_ID "Clang" CACHE STRING "" FORCE)
set(CMAKE_C_COMPILER_VERSION "21.0.0" CACHE STRING "" FORCE)
set(CMAKE_ASM_COMPILER_ID "Clang" CACHE STRING "" FORCE)
set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_ASM_COMPILER_FORCED TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT
	"-mcpu=c29.c0 -mfpu=f32 -ffunction-sections -fdata-sections -ffreestanding")
set(CMAKE_ASM_FLAGS_INIT
	"-mcpu=c29.c0 -mfpu=f32 -x assembler-with-cpp")
# -nostartfiles is ignored by this driver; freestanding uses -nostdlib.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib")

set(ULMK_COMPILER_FAMILY "ticlang" CACHE STRING "" FORCE)
set(ULMK_LINKER_DIALECT "ti-c29" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

message(STATUS "C29 toolchain: ${TI_C29_CGT_ROOT}")
