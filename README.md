# ulmk

A small, seL4-inspired microkernel for automotive-grade embedded systems.
Primary targets: **AURIX TriCore TC2xx/TC3xx** (QEMU CI), **RISC-V RV32IMAC**
(QEMU `virt` CI), and **ARM Cortex-M** — **ARMv7-M** (Cortex-M7, `mps2-an500`)
and **ARMv8-M** (Cortex-M33, `mps2-an505`) on QEMU CI.  Isolation is enforced
by the hardware MPU/PMP at thread granularity; policy lives entirely in
userspace.  Single ELF output, O(1) bitmap scheduler, synchronous IPC with
priority inheritance.

---

## Architecture overview

```
┌─────────────────────────────────────────────────┐
│  ulmk_root_thread()                               │
│  board_services (console, clocks, …)            │  userspace
│  components (hello_world, drivers, apps, …)     │  (ULMK_PRIV_DRIVER / USER)
├─────────────────────────────────────────────────┤
│  kernel/  — scheduler, IPC, memory, IRQ table   │  supervisor
│  arch/    — context switch, MPU, tick, atomics  │
├─────────────────────────────────────────────────┤
│  board/   — ulmk_board_init, ulmk_printk_char_out   │  hardware
│  chip     — MEMORY block (external, ULMK_CHIP_DIR)│
└─────────────────────────────────────────────────┘
```

Key properties:

- **One ELF, MPU isolation.**  All components link into a single binary.
  The hardware MPU enforces data domain boundaries between threads.
- **Synchronous IPC.**  Caller blocks until server replies.  Priority
  inheritance prevents priority inversion.
- **Component model.**  Each feature is a component: a directory with a
  `CMakeLists.txt` calling `ulmk_component_register()`.  Components default to
  **OFF**; enable them with `python3 tools/dev.py components enable`.
- **No weak symbols.**  Board and component sources provide strong definitions.
  A missing symbol is a link error, not a silent no-op.

### Microkernel philosophy

ulmk follows the classic microkernel split: the **kernel is mechanism**, userspace
is **policy**.  Threads never call each other directly — they exchange messages
through kernel-managed IPC endpoints and notifications.  Every privileged
operation (create a thread, map memory, bind an IRQ) crosses a single **syscall
gateway**; the kernel validates the request, enforces MPU boundaries, and
returns.  Driver logic, service topology, and startup order all live in
userspace (`ulmk_root_thread`, board services, components).

```mermaid
flowchart TB
    subgraph userspace["Userspace — policy"]
        direction LR
        A["App thread<br/>(ULMK_PRIV_USER)"]
        D["Driver / server<br/>(ULMK_PRIV_DRIVER)"]
        S["Another service"]
    end

    subgraph kernel["ulmk microkernel — mechanism"]
        direction TB
        GW["Syscall gateway<br/>(trap / SVC / ecall)"]
        IPC["IPC endpoints &amp; notifications"]
        SCH["Scheduler"]
        MPU["MPU / PMP isolation"]
    end

    A -->|"ulmk_ep_send / reply"| GW
    D -->|"ulmk_thread_* / ulmk_mem_*"| GW
    S -->|"ulmk_notif_* / ulmk_irq_*"| GW
    GW --> IPC
    GW --> SCH
    GW --> MPU
    A <-->|"sync IPC"| D
    D <-->|"sync IPC"| S
```

The diagram is schematic: all cross-boundary traffic funnels through the kernel.
Applications hold only the capabilities they were granted; the MPU keeps each
thread inside its own data domain even though everything links into one ELF.

---

## Application model

```
ulmk_root_thread()                 ← provided by the ROOT_THREAD component
    board_services_init(info)    ← provided by the board (console, etc.)
    my_component_init()          ← provided by each enabled component
    ulmk_thread_exit()
```

See `docs/application_development_guide.md` for a complete walkthrough.

---

## Getting started

### Requirements

- Linux host (Ubuntu 22.04 or newer recommended)
- Docker (for the dev container — cross-toolchains + QEMU)
- Python 3.8+

Toolchains and QEMU are only available inside the dev container.  Do **not**
try to compile or run target tests on the host.

### Enter the dev container

```bash
# First run builds the Docker image (~20–30 min)
python3 tools/dev.py

# Force rebuild if the image is stale
python3 tools/dev.py --rebuild
```

The workspace is mounted at `/workspace` inside the container.

### Build the hello world demo (inside the container)

All components are **OFF by default**. Enable a demo, build, and run.
`hello_world` is the only builtin component; `ping_pong` lives in the sibling
`ulmk_apps/` repo (auto-discovered when present). Each has its own
`ROOT_THREAD` — enable only one at a time.

```bash
# 1. See what is available
python3 tools/dev.py components list

# 2. Enable hello_world (builtin)
python3 tools/dev.py components enable hello_world

# 3. Build
python3 tools/dev.py build --board boards/qemu_riscv_virt

# TriCore QEMU (default board)
python3 tools/dev.py build

# ARM Cortex-M7 (ARMv7-M)
python3 tools/dev.py build --board boards/qemu_mps2_an500

# ARM Cortex-M33 (ARMv8-M)
python3 tools/dev.py build --board boards/qemu_mps2_an505

# Clean rebuild
python3 tools/dev.py build --clean

# One-shot enable without saving .ulmk/components.conf
python3 tools/dev.py build --no-components --component hello_world

# IPC demo from ulmk_apps (requires sibling checkout)
python3 tools/dev.py build --no-components --component ping_pong

# Kernel-only image (no components)
python3 tools/dev.py build --no-components
```

Local component selection is stored in `.ulmk/components.conf` (gitignored).

### Run on QEMU (inside the container)

```bash
python3 tools/dev.py build qemu --board boards/qemu_riscv_virt
python3 tools/dev.py build qemu
python3 tools/dev.py build qemu --board boards/qemu_mps2_an500
python3 tools/dev.py build qemu --board boards/qemu_mps2_an505
```

Expected output with `hello_world`:

```
ulmk: kernel entry
...
ulmk: switching to root thread
ulmk: hello from userspace — hello world!
```

`hello_world` then stops the simulator, so QEMU exits 0 by itself.

Expected output with `ping_pong`:

```
ping_pong: round 1
```

### Alternative: consume ulmk as a prebuilt SDK

The demo above builds a single ELF from the kernel sources.  If instead you want
to drop ulmk into an **existing firmware tree or a third-party IDE** (Eclipse,
STM32Cube, the Infineon toolchain — e.g. to replace FreeRTOS), build it once as a
distributable SDK: two static archives + a fully-processed linker script + the
public headers.

```bash
# --board is mandatory with --kernel; it may point anywhere (out-of-tree board)
python3 tools/dev.py build --kernel --board boards/qemu_tc3xx
```

This emits a self-contained tree under `build/ulipe-<arch>-sdk/dist/ulmk/`:

```
ulmk/
  lib/libulmk_kernel_<tag>.a  kernel + arch (supervisor)
  lib/libulmk_board_<tag>.a   startup + vectors + board services (driver)
  linker/linker_<tag>.ld      processed linker script
  include/                    public microkernel + board headers
```

Your firmware then provides `ulmk_root_thread()`, includes `<ulmk/microkernel.h>`,
and links both archives.  See the
[SDK integration guide](docs/application_development_guide.md#14-sdk-mode--integrating-ulmk-into-an-external-toolchain)
for the full recipe, and `tests/sdk_e2e/` for a working consumer.

### Build options reference

```bash
# Custom board chip dir
python3 tools/dev.py build --board /path/to/my_board
```

CMake configure variables of interest:

```bash
-DULMK_CHIP_DIR=boards/qemu_tc3xx           # TriCore QEMU (default)
-DULMK_CHIP_DIR=boards/qemu_riscv_virt      # RISC-V QEMU virt
-DULMK_CHIP_DIR=boards/qemu_mps2_an500      # ARMv7-M Cortex-M7 QEMU
-DULMK_CHIP_DIR=boards/qemu_mps2_an505      # ARMv8-M Cortex-M33 QEMU
-DULMK_COMP_hello_world_ENABLED=ON          # component override (dev.py sets these)
-DULMK_CONFIG_MAX_IRQ_BINDINGS=16          # SRPN → notif binding table
-DULMK_CONFIG_DEBUG_PRINTK=1               # kernel debug prints
```

### Run tests (inside the container)

```bash
# Unit tests (host, no QEMU)
python3 tools/dev.py tests unit

# Integration tests — TriCore (default)
python3 tools/dev.py tests integ

# Integration tests — RISC-V
python3 tools/dev.py tests integ --board boards/qemu_riscv_virt

# Integration tests — ARM Cortex-M
python3 tools/dev.py tests integ --board boards/qemu_mps2_an500
python3 tools/dev.py tests integ --board boards/qemu_mps2_an505
```

Individual integration tests also support `ARCH=tricore`, `ARCH=riscv`, or
`ARCH=arm` via `tests/integ_common.mk` (see any `tests/*/Makefile`).  For ARM,
pass `ARM_BOARD=qemu_mps2_an500` or `ARM_BOARD=qemu_mps2_an505` when invoking
`make` directly.

---

## Repository layout

```
CMakeLists.txt               top-level build orchestrator
cmake/
  arch.cmake                   ULMK_ARCH selection from board.cmake
  toolchain-tricore-gcc.cmake
  toolchain-riscv-gcc.cmake
  toolchain-arm-gcc.cmake
  component_api.cmake          ulmk_component_register, ulmk_components_finalize
  config.cmake                 kernel configuration symbols
  linker_api.cmake             ulmk_generate_linker_script
  generate_ld.py               assembles the generated linker script
kernel/                      platform-independent kernel
arch/tricore/                TriCore TC1.6.x port
arch/riscv/                  RISC-V RV32 port
arch/arm/                    ARM Cortex-M port (ARMv7-M / ARMv8-M)
boards/qemu_tc3xx/           TriCore QEMU CI board
boards/qemu_riscv_virt/      RISC-V QEMU virt CI board
boards/qemu_mps2_an500/      ARMv7-M Cortex-M7 QEMU CI board
boards/qemu_mps2_an505/      ARMv8-M Cortex-M33 QEMU CI board
components/hello_world/      reference ROOT_THREAD component (default OFF)
                             # ping_pong lives in sibling ../ulmk_apps/ping_pong/
include/ulmk/microkernel.h     public API (all syscall wrappers)
linker/                      arch-independent linker fragments
stub/                        documentation-only stub templates
tests/                       integration tests (standalone Makefiles)
tools/dev.py                 container frontend
docs/                        specifications and guides
```

---

## Documentation

| Document | What it covers |
|----------|----------------|
| [api\_spec](docs/api_spec.md) | Complete public API reference |
| [arch\_api\_spec](docs/arch_api_spec.md) | Architecture abstraction layer contract |
| [build\_system\_spec](docs/build_system_spec.md) | CMake build model and component discovery |
| [component\_spec](docs/component_spec.md) | Component system design |
| [linker\_spec](docs/linker_spec.md) | Three-layer linker script model |
| [application\_development\_guide](docs/application_development_guide.md) | How to build an application for custom hardware |
| [arch\_porting\_guide](docs/arch_porting_guide.md) | How to add a new architecture |
| [riscv\_implementation](docs/arch/riscv_implementation.md) | RISC-V RV32 port details |

---

## License

MIT — see `SPDX-License-Identifier: MIT` in each source file.
