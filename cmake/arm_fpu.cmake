# SPDX-License-Identifier: MIT
#
# cmake/arm_fpu.cmake — float ABI selection for Cortex-M boards.
#
# The FPU type is a fact about the chip and stays in board.cmake; whether the
# build uses it is policy and comes from ULMK_CONFIG_FPU.  A board calls:
#
#	ulmk_arm_float_flags(cortex-m7 fpv5-sp-d16)
#
# ULMK_CONFIG_FPU=1 (default) selects the hard-float ABI, where FP arguments
# travel in FP registers.  The ABI has to match across every object linked into
# the image, including ones an SDK consumer compiles in its own IDE, and ld
# refuses the mix rather than miscompiling quietly:
#
#	error: <app>.elf uses VFP register arguments, <sdk>.a(x.o) does not
#
# Vendor IDEs default to hard-float on parts with an FPU, so a softfp SDK
# cannot be consumed without reconfiguring the whole project.  Hard is the
# default here for that reason.
#
# ULMK_CONFIG_FPU=0 builds soft-float and drops the FP register file from the
# context switch — see ULMK_ARCH_HAVE_FPU in arch/arm/include/arch_config.h.

# board.cmake is included from the toolchain file, ahead of cmake/config.cmake,
# so the default has to be established here too.  A -D on the command line is
# already in the cache and wins over both; the duplicate set() is a no-op.
set(ULMK_CONFIG_FPU 1 CACHE STRING
	"Use the hardware FPU: hard-float ABI + FP context switch (0 = soft)")

macro(ulmk_arm_float_flags cpu fpu)
	if("${ULMK_CONFIG_FPU}" STREQUAL "1")
		set(_ulmk_arm_mflags "-mcpu=${cpu} -mfloat-abi=hard -mfpu=${fpu}")
	else()
		set(_ulmk_arm_mflags "-mcpu=${cpu} -mfloat-abi=soft")
	endif()

	if(DEFINED CMAKE_C_FLAGS)
		string(APPEND CMAKE_C_FLAGS " ${_ulmk_arm_mflags}")
		string(APPEND CMAKE_ASM_FLAGS " ${_ulmk_arm_mflags}")
		string(APPEND CMAKE_EXE_LINKER_FLAGS " ${_ulmk_arm_mflags}")
	endif()
endmacro()
