# cmake/linker_api.cmake
# Public CMake API consumed by the top-level CMakeLists.txt and
# (eventually) by the ulmk_apps sibling directory.
# Full specification: docs/build_system_spec.md §6
#
# Functions:
#   ulmk_add_app(name SOURCES <files...> [DOMAIN <domain>])
#   ulmk_add_domain(name [REGION <KERNEL_RAM|SHARED_RAM>])
#   ulmk_set_root_thread(target)
#
# All three accumulate state in internal CMake lists; the top-level
# CMakeLists.txt invokes _ulmk_finalize_build() after all ulmk_add_* calls
# to wire everything together and invoke generate_ld.py.

# Internal accumulators — do not use directly.
set(_UL_APPS     "" CACHE INTERNAL "")
set(_UL_DOMAINS  "" CACHE INTERNAL "")
set(_UL_ROOT_TGT "" CACHE INTERNAL "")

# ---------------------------------------------------------------------------
# ulmk_add_app
# ---------------------------------------------------------------------------
function(ulmk_add_app name)
    cmake_parse_arguments(ARG "" "DOMAIN" "SOURCES" ${ARGN})

    add_library("app_${name}" OBJECT ${ARG_SOURCES})
    target_compile_definitions("app_${name}" PRIVATE ULMK_APP_NAME=${name})
    target_include_directories("app_${name}" PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated")

    set(tmp "${_UL_APPS}")
    list(APPEND tmp "${name}")
    set(_UL_APPS "${tmp}" CACHE INTERNAL "")
    if(DEFINED ARG_DOMAIN)
        set_target_properties("app_${name}" PROPERTIES UL_APP_DOMAIN "${ARG_DOMAIN}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# ulmk_add_domain
# ---------------------------------------------------------------------------
function(ulmk_add_domain name)
    cmake_parse_arguments(ARG "" "REGION" "" ${ARGN})
    if(NOT DEFINED ARG_REGION)
        set(ARG_REGION "KERNEL_RAM")
    endif()
    set(tmp "${_UL_DOMAINS}")
    list(APPEND tmp "${name}:${ARG_REGION}")
    set(_UL_DOMAINS "${tmp}" CACHE INTERNAL "")
endfunction()

# ---------------------------------------------------------------------------
# ulmk_set_root_thread
# ---------------------------------------------------------------------------
function(ulmk_set_root_thread target)
    set(_UL_ROOT_TGT "${target}" CACHE INTERNAL "")
endfunction()

# ---------------------------------------------------------------------------
# _ulmk_finalize_build  (called by top-level CMakeLists.txt after all ulmk_add_*)
# ---------------------------------------------------------------------------
function(_ulmk_finalize_build kernel_target chip_dir)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(generated_ld "${CMAKE_BINARY_DIR}/generated/ulmk.ld")

    # Build argument lists for generate_ld.py
    set(app_args "")
    foreach(app IN LISTS _UL_APPS)
        list(APPEND app_args "--app" "${app}")
    endforeach()

    set(comp_args "")
    foreach(comp IN LISTS _ULMK_COMPONENTS)
        list(APPEND comp_args "--comp" "${comp}")
    endforeach()

    set(domain_args "")
    foreach(entry IN LISTS _UL_DOMAINS)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 dname)
        list(GET parts 1 dregion)
        list(APPEND domain_args "--domain" "${dname}:${dregion}")
    endforeach()

    if("${ULMK_LINKER_DIALECT}" STREQUAL "ti-c29")
        set(_ld_dialect_args "--dialect" "ti-c29")
        if(ULMK_C29_FLASH)
            list(APPEND _ld_dialect_args "--profile" "flash")
            if("${ULMK_CONFIG_ENABLE_SMP}" STREQUAL "1")
                list(APPEND _ld_dialect_args "--smp")
            endif()
            set(_ld_memory_dep "${chip_dir}/memory_flash.cmd")
        # memory.cmd is the TI board input; fall back to memory.ld if absent.
        elseif(EXISTS "${chip_dir}/memory.cmd")
            set(_ld_memory_dep "${chip_dir}/memory.cmd")
        else()
            set(_ld_memory_dep "${chip_dir}/memory.ld")
        endif()
    else()
        set(_ld_dialect_args "")
        set(_ld_memory_dep "${chip_dir}/memory.ld")
    endif()

    add_custom_command(
        OUTPUT  "${generated_ld}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/cmake/generate_ld.py"
                "--chip-dir"   "${chip_dir}"
                "--arch-dir"   "${ULMK_ARCH_LINKER_DIR}"
                "--kernel-dir" "${CMAKE_SOURCE_DIR}/linker/kernel"
                "--snippets"   "${CMAKE_SOURCE_DIR}/linker/snippets"
                "--output"     "${generated_ld}"
                ${_ld_dialect_args}
                ${app_args}
                ${comp_args}
                ${domain_args}
        DEPENDS
            "${_ld_memory_dep}"
            COMMENT "Generating linker script ${generated_ld}"
    )

    add_custom_target(ulmk_linker_script DEPENDS "${generated_ld}")

    if("${ULMK_LINKER_DIALECT}" STREQUAL "ti-c29")
        # Freestanding: no TI ROM autoinit / RTS.  Pull only compiler-rt
        # builtins (e.g. __udivsi3) from the CGT package.
        set(_c29_builtins
            "${TI_C29_CGT_ROOT}/lib/c29.c0-ti-none-eabi/libclang_rt.builtins.a")
        if(NOT EXISTS "${_c29_builtins}")
            set(_c29_builtins
                "${TI_C29_CGT_ROOT}/lib/libclang_rt.builtins.a")
        endif()
        target_link_options("${kernel_target}" PRIVATE
            "LINKER:--map_file=${CMAKE_BINARY_DIR}/ulmk.map"
            "LINKER:--reread_libs"
            "LINKER:--disable_auto_rts"
            "${generated_ld}"
            "${_c29_builtins}")
    else()
        target_link_options("${kernel_target}" PRIVATE
            "-T${generated_ld}"
            "-nostartfiles"
            "-Wl,--gc-sections"
            "-Wl,-Map=${CMAKE_BINARY_DIR}/ulmk.map")
    endif()

    add_dependencies("${kernel_target}" ulmk_linker_script)

    # Link every registered component library into the final executable.
    foreach(comp IN LISTS _ULMK_COMPONENTS)
        target_link_libraries("${kernel_target}" PRIVATE "ulmk_comp_${comp}")
    endforeach()
endfunction()
