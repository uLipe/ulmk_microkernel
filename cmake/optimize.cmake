# cmake/optimize.cmake — kernel/arch compile-time optimisation level.
#
# Default: -Ofast -fno-inline (speed).  Pass -DULMK_OPTIMIZE_SIZE=ON (or
# `tools/dev.py build --optimize-size`) for -Os.
#
# -fno-inline is required with -O3/-Ofast on TriCore: GCC inlining across
# CSA / syscall boundaries corrupts PCXI UL bits (class-3 TIN 6 CTYP).
# ARM/RISC-V tolerate it; keeping the flag arch-wide is the safe default.
#
# Applies only to kernel + arch translation units (libulmk_kernel.a and
# arch startup/vectors).  Board and component/userspace code keep the
# toolchain defaults.

option(ULMK_OPTIMIZE_SIZE
    "Optimize kernel/arch for code size (-Os) instead of speed (-Ofast)"
    OFF)

if(ULMK_OPTIMIZE_SIZE)
    set(ULMK_KERNEL_OPT_FLAGS -Os)
    message(STATUS "Kernel/arch optimisation: -Os (size)")
elseif("${ULMK_COMPILER_FAMILY}" STREQUAL "ticlang")
    # TIClang: -Ofast not supported; -fno-inline has no effect on inlining
    # across protected-call boundaries (C29 has no CSA-style UL bit hazard).
    set(ULMK_KERNEL_OPT_FLAGS -O3)
    message(STATUS "Kernel/arch optimisation: -O3 (TIClang)")
else()
    set(ULMK_KERNEL_OPT_FLAGS -Ofast -fno-inline)
    message(STATUS "Kernel/arch optimisation: -Ofast -fno-inline (speed)")
endif()

# Apply ULMK_KERNEL_OPT_FLAGS to every file in a source list (arch EXE
# objects linked into ulmk / ulmk_board, not into ulmk_kernel).
macro(ulmk_apply_kernel_opt _srcs)
    foreach(_ulmk_opt_src IN LISTS ${_srcs})
        set_property(SOURCE "${_ulmk_opt_src}" APPEND PROPERTY
            COMPILE_OPTIONS ${ULMK_KERNEL_OPT_FLAGS})
    endforeach()
endmacro()
