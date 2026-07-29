cmake_minimum_required(VERSION 3.20)

# Toolchain + board/arch resolution: pass -DCMAKE_TOOLCHAIN_FILE and
# -DULMK_CHIP_DIR on the cmake command line (see tools/dev.py).

project(ulmk
    VERSION 0.1.0
    DESCRIPTION "Minimal capability-based microkernel"
    LANGUAGES C ASM)

# Re-sync arch from ULMK_CHIP_DIR (FORCE overwrites stale cache entries).
include(cmake/board_resolve.cmake)

include(cmake/arch_sources.cmake)

# Distribution SDK mode: emit the kernel/arch and the board as two static
# libraries (+ processed linker script) for integration into external
# toolchains, instead of a self-contained demo ELF.  Driven by
# `tools/dev.py build --kernel`.
option(ULMK_SDK "Emit distributable static libraries instead of a demo ELF" OFF)

# ---------------------------------------------------------------------------
# Build configuration
# ---------------------------------------------------------------------------

include(cmake/config.cmake)
include(cmake/optimize.cmake)

# ---------------------------------------------------------------------------
# Linker API (ulmk_add_app, ulmk_add_domain, ulmk_set_root_thread)
# ---------------------------------------------------------------------------

include(cmake/linker_api.cmake)

# ---------------------------------------------------------------------------
# Component API (ulmk_component_register, ulmk_components_finalize)
# ---------------------------------------------------------------------------

include(cmake/component_api.cmake)

# ---------------------------------------------------------------------------
# Chip / board selection (board.cmake already included by board_resolve.cmake)
# ---------------------------------------------------------------------------

if(NOT DEFINED ULMK_CHIP_DIR)
    set(ULMK_CHIP_DIR "${CMAKE_SOURCE_DIR}/boards/qemu_tc3xx"
        CACHE PATH "Path to the board directory containing memory.ld")
    get_filename_component(ULMK_CHIP_DIR "${ULMK_CHIP_DIR}" ABSOLUTE)
endif()

# Prefix board sources with the board directory path.
# Absolute paths (e.g. iLLD under deps/) are kept as-is.
set(_ULMK_BOARD_SRCS "")
foreach(_src IN LISTS ULMK_BOARD_SOURCES)
    if(IS_ABSOLUTE "${_src}")
        list(APPEND _ULMK_BOARD_SRCS "${_src}")
    else()
        list(APPEND _ULMK_BOARD_SRCS "${ULMK_CHIP_DIR}/${_src}")
    endif()
endforeach()

if(NOT DEFINED ULMK_BOARD_INCLUDES)
    set(ULMK_BOARD_INCLUDES "")
endif()

# ---------------------------------------------------------------------------
# Generated config headers — single generator shared with the test Makefiles.
#   generated/ulmk/config.h    kernel sizing/policy (kernel includes this)
#   generated/ulmk/platform.h  SoC snapshot for the arch layer (from board_config.h)
# ---------------------------------------------------------------------------

find_program(ULMK_PYTHON NAMES python3 python REQUIRED)

execute_process(
    COMMAND "${ULMK_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/gen_config.py"
        --out-dir "${CMAKE_BINARY_DIR}/generated"
        --board-config "${ULMK_CHIP_DIR}/board_config.h"
        --set "ULMK_CONFIG_MAX_IRQ_BINDINGS=${ULMK_CONFIG_MAX_IRQ_BINDINGS}"
        --set "ULMK_CONFIG_DEBUG_PRINTK=${ULMK_CONFIG_DEBUG_PRINTK}"
        --set "ULMK_CONFIG_SYSCALL_WCET=${ULMK_CONFIG_SYSCALL_WCET}"
        --set "ULMK_CONFIG_ENABLE_SMP=${ULMK_CONFIG_ENABLE_SMP}"
        --set "ULMK_CONFIG_TICK_HZ=${ULMK_CONFIG_TICK_HZ}"
        --set "ULMK_CONFIG_IRQ_ATTACH=${ULMK_CONFIG_IRQ_ATTACH}"
        --set "ULMK_CONFIG_ROOT_STACK_SIZE=${ULMK_CONFIG_ROOT_STACK_SIZE}"
        --set "ULMK_CONFIG_BOARD_IRQ_CTRL=${ULMK_CONFIG_BOARD_IRQ_CTRL}"
        --set "ULMK_CONFIG_BOARD_IRQ_CLAIM=${ULMK_CONFIG_BOARD_IRQ_CLAIM}"
        --set "ULMK_CONFIG_BOARD_PMP_EXTRA=${ULMK_CONFIG_BOARD_PMP_EXTRA}"
        --set "ULMK_CONFIG_FPU=${ULMK_CONFIG_FPU}"
    RESULT_VARIABLE _ulmk_gen_rc)
if(NOT _ulmk_gen_rc EQUAL 0)
    message(FATAL_ERROR "gen_config.py failed (rc=${_ulmk_gen_rc})")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/gen_config.py"
    "${ULMK_CHIP_DIR}/board_config.h")

# ---------------------------------------------------------------------------
# Kernel static library
# ---------------------------------------------------------------------------

add_library(ulmk_kernel STATIC
    kernel/init/init.c
    kernel/kernel_main.c
    kernel/syscall/syscall_router.c
    kernel/printk/ulmk_printk.c
    kernel/thread/thread.c
    kernel/time/timer_wheel.c
    kernel/time/sleep.c
    kernel/percpu/percpu.c
    kernel/percpu/klock.c
    kernel/sched/sched.c
    kernel/sched/fifo_rt.c
    kernel/sched/bitmap_rt.c
    kernel/ipc/ep.c
    kernel/notif/notif.c
    kernel/mem/mem.c
    kernel/mem/tlsf.c
    kernel/irq/irq.c
    kernel/syscall/syscall_wcet.c
    ${ULMK_ARCH_KERNEL_SOURCES})

if(EXISTS "${ULMK_CHIP_DIR}/qemu_printk_hook.c")
    target_sources(ulmk_kernel PRIVATE "${ULMK_CHIP_DIR}/qemu_printk_hook.c")
endif()

target_include_directories(ulmk_kernel PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${ULMK_ARCH_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated")

target_include_directories(ulmk_kernel PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${ULMK_CHIP_DIR}")

target_compile_definitions(ulmk_kernel PRIVATE ULMK_KERNEL_BUILD=1)

target_compile_options(ulmk_kernel PRIVATE
	-fno-data-sections
	${ULMK_KERNEL_OPT_FLAGS})

# init.c runs before .data/.bss are initialised — keep GCC from lowering its
# copy/zero loops into memcpy/memset calls.
set_source_files_properties(kernel/init/init.c PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

if(DEFINED ULMK_BOARD_CFLAGS)
    target_compile_options(ulmk_kernel PRIVATE ${ULMK_BOARD_CFLAGS})
endif()

# Arch startup/vectors are kernel-space bring-up; match kernel opt level.
ulmk_apply_kernel_opt(ULMK_ARCH_EXE_SOURCES)

# ---------------------------------------------------------------------------
# Component scan — kernel/components/, board/components/, then ../ulmk_apps/
# ---------------------------------------------------------------------------

message(STATUS "ULMK_ARCH=${ULMK_ARCH}  ULMK_CHIP_DIR=${ULMK_CHIP_DIR}")

# Components and apps are userspace policy — never part of the distributable
# kernel SDK, which ships only kernel + arch + board.
if(NOT ULMK_SDK)
    file(GLOB _kernel_comp_dirs LIST_DIRECTORIES true
        "${CMAKE_SOURCE_DIR}/components/*")
    foreach(_dir IN LISTS _kernel_comp_dirs)
        if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
            get_filename_component(_cname "${_dir}" NAME)
            add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/components/${_cname}")
        endif()
    endforeach()

    # Board-local demos (pins, pot, kit LEDs) live next to the BSP.
    if(DEFINED ULMK_CHIP_DIR AND IS_DIRECTORY "${ULMK_CHIP_DIR}/components")
        file(GLOB _board_comp_dirs LIST_DIRECTORIES true
            "${ULMK_CHIP_DIR}/components/*")
        foreach(_dir IN LISTS _board_comp_dirs)
            if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
                get_filename_component(_cname "${_dir}" NAME)
                add_subdirectory("${_dir}"
                    "${CMAKE_BINARY_DIR}/board_components/${_cname}")
            endif()
        endforeach()
    endif()

    set(_ULMK_APPS_DIR "${CMAKE_SOURCE_DIR}/../ulmk_apps")
    if(IS_DIRECTORY "${_ULMK_APPS_DIR}")
        file(GLOB _app_comp_dirs LIST_DIRECTORIES true "${_ULMK_APPS_DIR}/*")
        foreach(_dir IN LISTS _app_comp_dirs)
            if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
                get_filename_component(_cname "${_dir}" NAME)
                add_subdirectory("${_dir}"
                    "${CMAKE_BINARY_DIR}/ulmk_apps/${_cname}")
            endif()
        endforeach()
    endif()

    ulmk_components_finalize()
endif()

# ---------------------------------------------------------------------------
# Board static library (arch startup/vectors + board services + user entry)
#
# These objects must land in the userspace text/BSS regions (board services run
# at driver privilege), so they are kept OUT of libulmk_kernel.a and grouped in
# their own archive.  In SDK mode this archive is a shipped artefact; in the
# normal build the same objects are compiled straight into the demo ELF.
# ---------------------------------------------------------------------------

if(ULMK_SDK)
    add_library(ulmk_board STATIC
        ${ULMK_ARCH_EXE_SOURCES}
        ${_ULMK_BOARD_SRCS}
        ${CMAKE_SOURCE_DIR}/kernel/init/user_entry.c)

    target_include_directories(ulmk_board PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${ULMK_ARCH_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
        "${ULMK_CHIP_DIR}"
        ${ULMK_BOARD_INCLUDES})

    target_compile_options(ulmk_board PRIVATE -fno-data-sections)

    if(DEFINED ULMK_BOARD_CFLAGS)
        target_compile_options(ulmk_board PRIVATE ${ULMK_BOARD_CFLAGS})
    endif()
endif()

# ---------------------------------------------------------------------------
# Kernel executable
#   normal build : kernel + arch startup + board + apps  → demo ELF
#   SDK build    : stub root thread linked against the two static libs, only
#                  to validate that the shipped archives + linker script link.
# ---------------------------------------------------------------------------

if(ULMK_SDK)
    add_executable(ulmk "${CMAKE_SOURCE_DIR}/stub/root_thread_stub.c")

    target_include_directories(ulmk PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
        "${ULMK_ARCH_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
        "${ULMK_CHIP_DIR}"
        ${ULMK_BOARD_INCLUDES})

    target_compile_options(ulmk PRIVATE -fno-data-sections)

    if(DEFINED ULMK_BOARD_CFLAGS)
        target_compile_options(ulmk PRIVATE ${ULMK_BOARD_CFLAGS})
    endif()

    # kernel and board reference each other's symbols — group to resolve the
    # circular dependency between the two archives.
    target_link_libraries(ulmk PRIVATE
        -Wl,--start-group ulmk_board ulmk_kernel -Wl,--end-group)

    _ulmk_finalize_build(ulmk "${ULMK_CHIP_DIR}")
else()
    # user_entry.c is linked into the image (not libulmk_kernel.a) so it lands
    # in the userspace text region, executable by threads at every privilege.
    set(_ulmk_exe_sources
        ${ULMK_ARCH_EXE_SOURCES}
        ${_ULMK_BOARD_SRCS}
        ${CMAKE_SOURCE_DIR}/kernel/init/user_entry.c)

    if("${_ULMK_ROOT_THREAD_COMP}" STREQUAL "")
        list(APPEND _ulmk_exe_sources
            "${CMAKE_SOURCE_DIR}/stub/root_thread_stub.c")
    endif()

    add_executable(ulmk ${_ulmk_exe_sources})

    target_include_directories(ulmk PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${ULMK_ARCH_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
        "${ULMK_CHIP_DIR}"
        ${ULMK_BOARD_INCLUDES})

    target_compile_options(ulmk PRIVATE -fno-data-sections)

    if(DEFINED ULMK_BOARD_CFLAGS)
        target_compile_options(ulmk PRIVATE ${ULMK_BOARD_CFLAGS})
    endif()

    target_link_libraries(ulmk PRIVATE ulmk_kernel)

    _ulmk_finalize_build(ulmk "${ULMK_CHIP_DIR}")

    add_custom_command(TARGET ulmk POST_BUILD
        COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:ulmk>"
        COMMENT "Memory usage")
endif()
