# ulmk — Build System Specification

**Version:** 1.0
**Status:** Reflects the implemented build system as of the component system introduction.

> **Purpose of this document:** authoritative reference for the CMake build model,
> directory layout, component scan rules, chip parameterisation, and kernel
> configuration symbols.  Consult this before changing `CMakeLists.txt`,
> `cmake/*.cmake`, or adding a new board or component.

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Binary Model](#2-binary-model)
3. [Directory Structure](#3-directory-structure)
4. [Component Discovery](#4-component-discovery)
5. [Component CMake API](#5-component-cmake-api)
6. [Build Flow](#6-build-flow)
7. [Chip Parameterisation](#7-chip-parameterisation)
8. [Board Descriptor](#8-board-descriptor)
9. [Root Thread and Stubs](#9-root-thread-and-stubs)
10. [Kernel Configuration](#10-kernel-configuration)
11. [dev.py — Container Frontend](#11-devpy--container-frontend)
12. [Bootloader Boundary](#12-bootloader-boundary)
13. [Phase 2 — CMake Package Export](#13-phase-2--cmake-package-export)

---

## 1. Design Philosophy

The build system follows the same layering principle as the linker and arch specs:
**each layer owns its concerns, nothing bleeds across boundaries**.

```
┌──────────────────────────────────────────────────────────────────────┐
│  ulmk/          Kernel repo — kernel source, arch port,  │
│                             linker fragments, CMake API, boards/,    │
│                             built-in components, and stub files.     │
├──────────────────────────────────────────────────────────────────────┤
│  ../ulmk_apps/              Optional sibling — external components   │
│                             and board-specific root threads.         │
├──────────────────────────────────────────────────────────────────────┤
│  <any path>/my_board/       Board chip input (external, optional) —  │
│                             memory.ld + bmhd.ld.in for real hw.      │
│                             Pointed to by ULMK_CHIP_DIR.               │
├──────────────────────────────────────────────────────────────────────┤
│  build/                     Generated artefacts — generated.ld,      │
│                             object files, final ELF. Never committed.│
└──────────────────────────────────────────────────────────────────────┘
```

**Principles:**

- Output is always **one ELF**.  Isolation is logical (MPU sections), not physical
  (separate ELF files).
- The kernel builds standalone.  If no component declares `ROOT_THREAD`, it links
  with a commented-out stub as documentation — see §9.
- No component name, domain name, or source path is hardcoded in the kernel.  All
  of that comes from component `CMakeLists.txt` files.
- The linker script is **generated at configure time** from fragments, after all
  components have registered themselves.
- Chip-specific inputs (memory map, boot header) are parameterised via `ULMK_CHIP_DIR`.

---

## 2. Binary Model

```
ulmk  (ELF)
│
├── KERNEL_FLASH
│   ├── .startup           ← _start entry (must be first; QEMU starts at 0x80000000)
│   ├── .trap_table        ← trap vector table  (arch/tricore, ALIGN(256))
│   ├── .int_table         ← interrupt vector table (arch/tricore, ALIGN(256))
│   ├── .kernel_text       ← kernel supervisor code + rodata + component code
│   └── .domain_table      ← ulmk_domain_desc_t array (scanned at boot)
│
└── KERNEL_RAM
    ├── .kernel_data       ← kernel .data + .bss (supervisor only)
    ├── .kernel_stack      ← kernel supervisor stack
    ├── .isr_stack         ← ISR stack (TriCore ISP)
    ├── .csa_pool          ← TriCore CSA pool (ALIGN(64), NOLOAD)
    ├── .sdata / .sbss     ← TriCore small-data ABI
    └── .user_pool         ← physical allocator pool (remainder of KERNEL_RAM)
```

Component code currently lands in `.kernel_text` together with kernel code.
Per-component MPU domains are the mechanism for data isolation, not separate
code sections.

---

## 3. Directory Structure

```
ulmk/
├── CMakeLists.txt                        ← top-level orchestrator
├── cmake/
│   ├── toolchain-tricore-gcc.cmake       ← toolchain (-mcpu, sysroot, …)
│   ├── config.cmake                      ← kernel config knobs (cache vars)
│   ├── component_api.cmake               ← ulmk_component_register(), ulmk_components_finalize()
│   ├── linker_api.cmake                  ← ulmk_add_app(), ulmk_add_domain(), ulmk_generate_linker_script()
│   └── generate_ld.py                    ← assembles build/generated/ulmk.ld
├── tools/
│   └── gen_config.py                     ← generates build/generated/ulmk/{config.h,platform.h}
├── kernel/                               ← kernel implementation (sched, ipc, mem, irq, …)
├── arch/
│   └── tricore/
│       ├── arch.c  ctx_switch.S  vectors.S  startup.S
│       ├── include/
│       │   ├── arch_config.h             ← TriCore constants (ULMK_ARCH_NUM_DPR, etc.)
│       │   └── ulmk_arch.h                 ← arch contract header
│       └── linker/
│           ├── prologue.ld.in            ← OUTPUT_FORMAT, OUTPUT_ARCH, ENTRY
│           ├── csa_pool.ld.in            ← CSA pool fragment
│           └── small_data.ld.in          ← small-data ABI anchors
├── linker/
│   └── kernel/                           ← 6 arch-independent kernel fragments
│       ├── vectors.ld.in                 ← .startup + .trap_table + .int_table
│       ├── kernel_text.ld.in
│       ├── kernel_data.ld.in
│       ├── kernel_stacks.ld.in
│       ├── domain_table.ld.in
│       └── user_pool.ld.in
├── include/
│   └── ul/
│       ├── microkernel.h                 ← public kernel API (all syscall wrappers)
│       ├── linker.h                      ← ULMK_DOMAIN_BSS, ULMK_PRIVATE, ULMK_DEFINE_DOMAIN
│       ├── syscall_nr.h                  ← syscall number table
│       └── syscall_abi.h                 ← ULMK_SYSCALL_N macros
├── components/                           ← built-in components (hello_world only)
│   └── hello_world/
│       ├── CMakeLists.txt
│       ├── include/hello_world.h
│       └── src/root_thread.c  hello_world.c
├── boards/
│   └── qemu_tc3xx/                       ← ONLY built-in board (CI / QEMU)
│       ├── board.cmake                   ← sets ULMK_BOARD_CPU, ULMK_BOARD_CFLAGS, ULMK_BOARD_SOURCES
│       ├── memory.ld                     ← MEMORY block + linker flags (no BMHD for QEMU)
│       ├── qemu_console.c                ← ulmk_printk_char_out() via MMIO
│       ├── board_console.c / .h          ← IPC-backed console service
│       └── board_services.c / .h         ← ulmk_board_init() + board_services_init()
├── stub/
│   ├── board_init_stub.c                 ← non-weak no-op ulmk_board_init(); used by test Makefiles
│   ├── board_services_stub.c             ← DOCUMENTATION ONLY (commented out)
│   ├── printk_stub.c                     ← DOCUMENTATION ONLY (commented out)
│   └── root_thread_stub.c                ← DOCUMENTATION ONLY (commented out)
├── tests/                                ← integration tests (standalone Makefiles)
└── tools/
    ├── dev.py                            ← container frontend (cmake configure + build)
    └── docker/
```

External layout:

```
ulmk/            ← kernel repo (this)
../ulmk_apps/                ← optional sibling; auto-discovered if present
    ping_pong/               ← standalone IPC demo (ROOT_THREAD)
    silicon/                 ← aggregator CMakeLists → silicon_* certs
    freertos/                ← FreeRTOS shim

<anywhere>/my_board/         ← real board chip input (external)
    memory.ld                ← MEMORY block with real HW addresses
    bmhd.ld.in               ← chip boot header (if HAVE_BMHD = 1)
```

---

## 4. Component Discovery

### 4.1 Scan locations

The top-level `CMakeLists.txt` scans three locations at configure time:

1. `components/` — built-in components shipped with the kernel repo.
2. `${ULMK_CHIP_DIR}/components/` — board-local demos (kit LEDs, pot, …).
3. `../ulmk_apps/` — optional external components sibling (board-agnostic apps).

For each directory found, if it contains a `CMakeLists.txt`, it is added via
`add_subdirectory()`.  That `CMakeLists.txt` may call `ulmk_component_register()`
once, or may itself `add_subdirectory()` child packages that each register
(aggregator pattern used by `ulmk_apps/silicon/`).

```cmake
# From CMakeLists.txt — simplified
file(GLOB _dirs LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/components/*")
foreach(_dir IN LISTS _dirs)
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
        add_subdirectory("${_dir}" ...)
    endif()
endforeach()
# Same shallow pattern for ${ULMK_CHIP_DIR}/components/ and ../ulmk_apps/
# Nesting (e.g. silicon/*) is handled by the package's own CMakeLists.txt.
```

### 4.2 Discovery log

With all components OFF by default:

```
--   [component] hello_world DISABLED
--   [component] ping_pong DISABLED
--   [component] silicon_baseline DISABLED
```

After `dev.py build --no-components --component hello_world`:

```
--   [component] hello_world ENABLED
--   [component] ROOT_THREAD: hello_world
```

### 4.3 Validation and enable overrides

`ulmk_components_finalize()` validates:

- At most one **enabled** component declares `ROOT_THREAD`.
- Every `REQUIRES` dependency of an **enabled** component is also enabled.
  On failure, CMake prints a hint to run `python3 tools/dev.py components enable …`.

If no enabled component declares `ROOT_THREAD`, `stub/root_thread_stub.c` is
linked (`ulmk_root_thread()` calls `ulmk_thread_exit()` immediately).

### 4.4 Runtime selection (`dev.py components`)

| Command | Effect |
|---------|--------|
| `components list` | Discover under `components/`, `<board>/components/`, `../ulmk_apps/` |
| `components status` | Manifest default vs `.ulmk/components.conf` |
| `components enable NAME …` | Persist ON in `.ulmk/components.conf` (gitignored) |
| `components disable NAME …` | Remove from config |
| `build --component NAME` | One-shot ON for this build (overrides conf) |
| `build --no-components` | Ignore conf; only explicit `--component` flags apply |

CMake cache variable per component: `-DULMK_COMP_<name>_ENABLED=ON|OFF`.
Manifest `ENABLED OFF` is the default when the cache entry is unset on first configure.

---

## 5. Component CMake API

Defined in `cmake/component_api.cmake`, included automatically by the top-level.

### `ulmk_component_register`

```cmake
ulmk_component_register(
    NAME         <name>           # unique identifier
    ENABLED      <ON|OFF>         # whether to include this component
    SOURCES      <files…>         # source files (paths relative to component dir)
    INCLUDE_DIRS <dirs…>          # public include dirs added globally
    REQUIRES     <components…>    # (optional) dependency names
    LINKER_FRAGMENT <file>        # (optional) .ld.in appended to generated script
    ROOT_THREAD                   # (flag) this component provides ulmk_root_thread()
)
```

When the cache variable `ULMK_COMP_<name>_ENABLED` is ON (via manifest default
on first configure, or `dev.py` override):

- A static library `ulmk_comp_<name>` is created and linked into `ulmk`.
- `INCLUDE_DIRS` are `PUBLIC` on that target.
- `REQUIRES` deps are validated in `ulmk_components_finalize()` and linked
  via `target_link_libraries`.
- A per-component MPU domain snippet is generated.

When OFF, the component emits a `DISABLED` log line and contributes nothing.

### `ulmk_components_finalize`

```cmake
ulmk_components_finalize()
```

Called once after all component scans.  Validates ROOT_THREAD invariant and
prints the summary line.

---

## 6. Build Flow

```
cmake -B /build/ulipe \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-tricore-gcc.cmake \
      -DULMK_CHIP_DIR=boards/qemu_tc3xx
│
├─ cmake/config.cmake          → kernel config knobs (cache vars)
│   tools/gen_config.py → build/generated/ulmk/{config.h,platform.h}
│
├─ cmake/component_api.cmake   → defines ulmk_component_register, ulmk_components_finalize
│
├─ ${ULMK_CHIP_DIR}/board.cmake  → sets ULMK_BOARD_CPU, ULMK_BOARD_CFLAGS, ULMK_BOARD_SOURCES
│
├─ add_library(ulmk_kernel STATIC
│       kernel/**/*.c
│       arch/tricore/arch.c  ctx_switch.S
│       ${ULMK_BOARD_SOURCES})
│
├─ Component scan:
│   for each dir in components/ and ../ulmk_apps/:
│       add_subdirectory → ulmk_component_register()
│         sources → target_sources(ulmk_kernel …)
│         includes → target_include_directories(ulmk_kernel PUBLIC …)
│
├─ ulmk_components_finalize()    → validate ROOT_THREAD invariant
│
├─ cmake/linker_api.cmake      → ulmk_generate_linker_script()
│   PRE_LINK: generate_ld.py → build/generated/ulmk.ld
│
└─ add_executable(ulmk
       arch/tricore/startup.S
       arch/tricore/vectors.S)
   target_link_libraries(ulmk PRIVATE ulmk_kernel)
   target_link_options(-T build/generated/ulmk.ld)

cmake --build /build/ulipe
│
├─ compile all sources in ulmk_kernel (kernel + board + components)
├─ compile startup.S + vectors.S
├─ [PRE_LINK] generate_ld.py → build/generated/ulmk.ld
└─ link → build/ulipe/ulmk  (ELF)

Boot sequence (inside the ELF):
  _start (startup.S)
    ├─ ulmk_board_init()          ← provided by board; no globals, no kernel API
    ├─ .data copy (LMA→VMA) + .bss zero
    ├─ ulmk_arch_init()           ← fills ulmk_boot_info_t
    └─ ulmk_kern_main()         ← does not return; starts scheduler
          └─ ulmk_root_thread()   ← first userspace context
```

---

## 7. Chip Parameterisation

```bash
cmake -B build -DULMK_CHIP_DIR=boards/qemu_tc3xx          # TriCore QEMU (default)
cmake -B build -DULMK_CHIP_DIR=boards/qemu_riscv_virt     # RISC-V QEMU virt
cmake -B build -DULMK_CHIP_DIR=boards/qemu_mps2_an500     # ARMv7-M (Cortex-M7) QEMU
cmake -B build -DULMK_CHIP_DIR=boards/qemu_mps2_an505     # ARMv8-M (Cortex-M33) QEMU
cmake -B build -DULMK_CHIP_DIR=/path/to/my_board          # real hardware
```

`cmake/arch.cmake` reads `UL_BOARD_ARCH` from `${ULMK_CHIP_DIR}/board.cmake`
(via `cmake/board_resolve.cmake`) and sets:

| `UL_BOARD_ARCH` | Toolchain file | Arch sources |
|-----------------|----------------|--------------|
| `tricore` | `cmake/toolchain-tricore-gcc.cmake` | `arch/tricore/` |
| `riscv` | `cmake/toolchain-riscv-gcc.cmake` | `arch/riscv/` |
| `arm` | `cmake/toolchain-arm-gcc.cmake` | `arch/arm/` |

`ULMK_CHIP_DIR` must point to a directory containing:

| File | Required | Description |
|------|----------|-------------|
| `memory.ld` | yes | `MEMORY {}` block + linker flags (`ULMK_MPU_ALIGN`, `HAVE_CSA`, etc.) |
| `bmhd.ld.in` | only if `HAVE_BMHD=1` | Chip boot header section |
| `board.cmake` | yes | Sets `ULMK_BOARD_CPU`, `ULMK_BOARD_SOURCES`, QEMU machine |
| `board_config.h` | yes | SoC MMIO bases (`ULMK_BOARD_SRC_BASE`, `ULMK_BOARD_PLIC_BASE`, …) |

Full chip input contract: `docs/linker_spec.md §9`.

---

## 8. Board Descriptor

Each board directory must contain a `board.cmake` with:

```cmake
set(UL_BOARD_ARCH "tricore")   # or "riscv" / "arm"
set(ULMK_BOARD_CPU   "tc39xx")           # passed to -mcpu= (e.g. cortex-m7, cortex-m33)
set(ULMK_BOARD_SOURCES
    qemu_console.c
    board_console.c
    board_services.c
)
```

SoC addresses and platform options belong in `board_config.h` (included via
`${ULMK_CHIP_DIR}` on the compiler path).  `arch/<isa>/arch_config.h` pulls it
in and validates required `ULMK_BOARD_*` symbols.  Optional `-D` overrides in
`ULMK_BOARD_CFLAGS` remain supported for test matrices (e.g. irq_integ CLINT).

`ULMK_BOARD_SOURCES` paths are relative to `${ULMK_CHIP_DIR}`.  They are added to
`ulmk_kernel` and must provide at minimum:

| Symbol | Where | When called |
|--------|-------|-------------|
| `ulmk_board_init(void)` | `board_services.c` (or a dedicated `board_init.c`) | From `startup.S` before `.data` copy |
| `ulmk_printk_char_out(char)` | `qemu_console.c` or equivalent | By kernel printk subsystem |
| `board_services_init(const ulmk_boot_info_t *)` | `board_services.c` | From `ulmk_root_thread()` |

---

## 9. Root Thread and Stubs

### 9.1 Contract

Exactly one component must declare `ROOT_THREAD` and provide:

```c
#include <ulmk/microkernel.h>

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
    board_services_init(info);  /* start board services */
    my_component_init();        /* start app components */
    ulmk_thread_exit();
}
```

### 9.2 Stubs (documentation only)

The files in `stub/` (except `board_init_stub.c`) are **not compiled**.  They
exist solely as documentation showing what each symbol must look like:

| File | Symbol documented |
|------|------------------|
| `stub/root_thread_stub.c` | `ulmk_root_thread()` |
| `stub/printk_stub.c` | `ulmk_printk_char_out()` |
| `stub/board_services_stub.c` | `board_services_init()` |

`stub/board_init_stub.c` **is compiled** — only by integration test Makefiles
that include it directly.  It provides a non-weak no-op `ulmk_board_init()` for
QEMU test builds.  The CMake build never includes it; the board provides
`ulmk_board_init()` from its own sources.

### 9.3 Why no weak symbols

Weak symbols in a static library (`libulmk_kernel.a`) are resolved by the
first archive object that satisfies the reference, preventing the strong
definition in a later archive object from overriding it.  Using weak symbols
caused the test stubs to silently override the real board/component
implementations.  All board- and component-provided symbols are now **strong**,
and a missing symbol produces a link-time error rather than a silent no-op.

---

## 10. Kernel Configuration

### 10.1 Symbols

Only symbols the real (non-`UL_UNIT_TEST`) kernel actually reads are configurable.
TCBs, IPC endpoints and notification objects are heap-allocated (pointer-as-handle),
so there are no static-pool size knobs for them.

```
┌──────────────────────────────────┬──────────┬────────────────────────────────────┐
│ Symbol                           │ Default  │ What it controls                   │
├──────────────────────────────────┼──────────┼────────────────────────────────────┤
│ ULMK_CONFIG_MAX_IRQ_BINDINGS       │ 16       │ irq.c static SRPN → notif table    │
│ ULMK_CONFIG_DEBUG_PRINTK           │ 1        │ Kernel printk (0 = no-op)          │
│ ULMK_CONFIG_IRQ_ATTACH             │ 0        │ ulmk_irq_attach (1=DANGEROUS ISR   │
│                                    │          │ userspace callbacks; else ENOTSUP) │
└──────────────────────────────────┴──────────┴────────────────────────────────────┘
```

Canonical defaults and range validation live in **`tools/gen_config.py`** — the
single generator shared by the CMake build and the integration-test Makefiles.
`cmake/config.cmake` only re-exposes these as user-overridable cache variables.

### 10.2 Override

```bash
cmake -B build \
    -DULMK_CHIP_DIR=boards/qemu_tc3xx \
    -DULMK_CONFIG_MAX_IRQ_BINDINGS=32 \
    -DULMK_CONFIG_DEBUG_PRINTK=0
```

### 10.3 Generated headers

`tools/gen_config.py` emits two headers under `${CMAKE_BINARY_DIR}/generated/ulmk/`:

| Header | Content | Included by |
|--------|---------|-------------|
| `config.h` | kernel sizing/policy (table above) | `kernel/*.c` |
| `platform.h` | snapshot of `boards/<soc>/board_config.h` | arch layer (`arch_config.h`) |

Both are **kernel/arch-internal**.  User code must not include them.  A future DTS
pipeline replaces the `board_config.h` snapshot step without touching the kernel.

Board-specific timer clock rates (e.g. STM0 frequency) belong in board source
(`board_timer.c`) or `board_config.h`, not in `config.cmake`.

---

## 11. dev.py — Container Frontend

`tools/dev.py` is a thin CMake frontend that runs inside the dev container.

```bash
python3 tools/dev.py              # enter container (builds image on first run)
python3 tools/dev.py --rebuild    # force image rebuild

# Inside the container:
python3 tools/dev.py build                      # configure + build (QEMU board)
python3 tools/dev.py build --clean              # rm -rf build + configure + build
python3 tools/dev.py build --board /path/board  # custom ULMK_CHIP_DIR
python3 tools/dev.py build --optimize-size      # kernel/arch -Os (default: -Ofast)
python3 tools/dev.py run                        # build (if stale) + run on QEMU
python3 tools/dev.py build --kernel --board /path/board   # distributable SDK
```

Kernel and arch objects default to **`-Ofast -fno-inline`**.  Pass
`--optimize-size` (CMake `-DULMK_OPTIMIZE_SIZE=ON`) for **`-Os`**.  Board and
component/userspace sources are unaffected.  Integration-test kernel objects
follow the same default via `tests/integ_kernel.mk`.

`-fno-inline` is required with `-Ofast` on TriCore: GCC inlining across CSA /
syscall boundaries triggers class-3 TIN 6 (CTYP).  The flag is applied on every
arch so the kernel opt surface stays uniform.

`dev.py` does not maintain a parallel source list.  CMake is the single source
of truth for what gets compiled.

### 11.1 Distributable kernel SDK (`build --kernel`)

For integrating ulmk into an external toolchain (Eclipse, STM32Cube, the
Infineon IDE, an existing firmware tree replacing FreeRTOS), `build --kernel`
emits a self-contained SDK for one board.  `--board` is **mandatory** with
`--kernel`; the board may live anywhere (a private out-of-tree directory is the
expected case).

```bash
python3 tools/dev.py build --kernel --board /path/to/my_board
```

Output (under `build/ulipe-<arch>-sdk/dist/`):

```
ulmk/
  lib/libulmk_kernel_<arch>_<board>_gcc.a  kernel + arch (kernel text/data, CPR0)
  lib/libulmk_board_<arch>_<board>_gcc.a   startup + vectors + board + user entry
                                           (userspace text/BSS, CPR1)
  linker/linker_<arch>_<board>_gcc.ld      fully-processed linker script
  include/ulmk/*.h                         public microkernel API
  include/ulmk_syscall_abi.h               arch SYSCALL ABI (ULMK_SYSCALL_N macros;
                                           redirected to by <ulmk/syscall_abi.h>)
  include/board/*.h                        public board services / board init API
```

`include/ulmk_syscall_abi.h` is the architecture-specific ABI header.
`<ulmk/syscall_abi.h>` is only a redirector to it, and `<ulmk/microkernel.h>`
pulls it in transitively, so it **must** ship at the include root or every
consumer of the public API fails to compile.

Two archives instead of one: the kernel runs at supervisor privilege (CPR0)
while board services run as driver-privilege userspace threads (CPR1), and the
linker separates the two by archive name.  The archives carry the lib prefix so
that a consumer can name them with plain `-l`, which is all a vendor IDE's
library field emits.  Link **both**, grouped to resolve the kernel⇄board
cross-references:

```bash
<arch>-gcc -T linker_<arch>_<board>_gcc.ld -nostartfiles -Wl,--gc-sections \
    my_root_thread.o my_app.o \
    -L<sdk>/lib \
    -Wl,--start-group -lulmk_board_<...> -lulmk_kernel_<...> -Wl,--end-group \
    -o firmware.elf
```

The consumer provides `ulmk_root_thread()` (and any driver/app threads).
`ulmk_board_init()` and `board_services_init()` ship inside the board archive.
The reset entry (`_start`) and vector tables are force-linked from the archive
via `EXTERN(...)` in the arch linker prologue, so no whole-archive flag is
needed.  Components and apps are userspace policy and are **never** bundled in
the SDK.

Under the hood, SDK mode (`-DULMK_SDK=ON`) skips component discovery, builds the
two archives plus the processed `ulmk.ld`, and links a throwaway ELF (stub root
thread) purely to validate that the shipped archives and linker script link.

The build/assemble steps live in `tools/sdk_build.sh` so that `dev.py` and the
SDK consumer test share one implementation.  `tests/sdk_e2e/` is a standalone
end-to-end test: it builds the SDK, then compiles a root thread that exercises
every public syscall against the shipped artefacts only (no kernel sources), and
checks a PASS/FAIL sentinel in QEMU.  Run it with `python3 tools/dev.py tests
e2e` (add `--board boards/qemu_riscv_virt` for RV32, or
`--board boards/qemu_mps2_an500` / `boards/qemu_mps2_an505` for Cortex-M).
Board bring-up must go
through the portable `board_services_init(info)` entry point; `board_console_start()`
is board-internal (a no-op stub on some boards) and must not be called directly.

---

## 12. Bootloader Boundary

```
WITHOUT external bootloader (common case):
  power-on / reset vector
       └─ _start (asm prologue) → ulmk_kern_start()
              → ulmk_board_init() → .data/.bss → ulmk_arch_init() → ulmk_kern_main()

WITH external bootloader (when needed):
  ROM / Stage-0 (PLL, clocks already done)
       └─ jump to _start → ulmk_kern_start() → ulmk_board_init() [no-op] → … → ulmk_kern_main()
```

For QEMU: `-kernel` loads to `0x80000000` and starts there, not at the ELF
entry point.  The `.startup` linker section in `linker/kernel/vectors.ld.in`
ensures `_start` is placed at `0x80000000`.

---

## 13. Phase 2 — CMake Package Export

Future: the kernel will be installable as a CMake package:

```bash
cmake --install build --prefix /opt/ulmk
```

An external app:

```cmake
find_package(UlipeMicroKernel REQUIRED HINTS /opt/ulmk)
ulmk_component_register(NAME my_app SOURCES my_app.c INCLUDE_DIRS include ROOT_THREAD ENABLED ON)
ulmk_components_finalize()
ulmk_generate_linker_script()
```

Phase 2 requires only CMake packaging infrastructure — no kernel source changes.

---

*Implementation lives in `cmake/`, `boards/`, `stub/`, `components/`, and the
top-level `CMakeLists.txt`.  Generated artefacts are in `build/`.*
