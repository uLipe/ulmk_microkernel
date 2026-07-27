# ulmk — Component System Specification

**Version:** 0.3
**Status:** Implemented — reflects the component system as of the `cmake/component_api.cmake` introduction.

> **Purpose of this document:** design rationale and rules for the component
> system: directory layout, CMake API (`ulmk_component_register`), ROOT_THREAD
> invariant, service pattern (IPC as implementation detail), and the
> `board_console` reference service.  For a step-by-step application walkthrough
> see `docs/application_development_guide.md`.

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Component Directory Structure](#2-component-directory-structure)
3. [CMake Registration API](#3-cmake-registration-api)
4. [Discovery and Scan Rules](#4-discovery-and-scan-rules)
5. [Build Flow](#5-build-flow)
6. [Linker Fragment Contract](#6-linker-fragment-contract)
7. [Root Thread Ownership and Component Init](#7-root-thread-ownership-and-component-init)
8. [Component Service Pattern — IPC as Implementation Detail](#8-component-service-pattern--ipc-as-implementation-detail)
9. [hello_world Component Design](#9-hello_world-component-design)
10. [ping_pong Component Design](#10-ping_pong-component-design)
11. [board_console — Board Source Service](#11-board_console--board-source-service)
12. [Open Questions](#12-open-questions)

---

## 1. Design Philosophy

A **component** is an independently-developed, reusable unit of userspace code that runs inside the microkernel image alongside other components. It is the primary packaging unit for drivers, services, and applications built on top of the microkernel.

Key principles:

- **One image, N components.** The microkernel produces a single ELF. All components are compiled and linked into that ELF. Isolation is enforced at runtime by the MPU, not by separate binaries.
- **Components are opt-in.** Every component declares `ENABLED OFF` in its CMake
  manifest (default). Enable them at configure time via `python3 tools/dev.py
  components enable` or `dev.py build --component`. Disabled components contribute
  zero code to the image.
- **Only one root thread.** The kernel creates exactly one initial userspace context. Exactly one component provides the `ulmk_root_thread()` function. That component is responsible for starting all other components' service threads by calling their public init functions in order.
- **Kernel knows nothing about components.** Component names, endpoints, and initialisation order are purely userspace policy. The kernel only sees threads, endpoints, and memory mappings.
- **IPC is an implementation detail.** Components expose C functions in their public headers, not endpoint handles. Callers never touch IPC directly; the component's client-side library encapsulates it.
- **Board services are board sources, not components.** Hardware-dependent services (console, clocks, etc.) live in `boards/<board>/` and follow the same public C API convention.

---

## 2. Component Directory Structure

```
my_component/
├── CMakeLists.txt          # component descriptor; calls ulmk_component_register()
├── include/
│   └── my_component.h      # public C API (init function + service calls)
├── src/
│   ├── server.c            # service thread entry point + IPC recv loop
│   └── client.c            # client-side wrappers that call into the service via IPC
└── linker.ld.in            # (optional) domain memory fragment
```

### Rules

- The `CMakeLists.txt` **must** contain exactly one `ulmk_component_register()` call.
- `include/` is added to the global include path so other enabled components can include it.
- Sources listed in `SOURCES` are compiled with `-DUL_COMPONENT_NAME=<name>` defined.
- `linker.ld.in` is optional. If present and the component is enabled, it is appended to the generated linker script after all kernel sections.
- Every component's public header **must** export an `<name>_init()` function. The root thread calls this explicitly. The component is responsible for preventing double-init.

---

## 3. CMake Registration API

### `ulmk_component_register`

Declared in `cmake/component_api.cmake`, auto-included by the top-level build.

```cmake
ulmk_component_register(
    NAME          <string>        # unique component identifier
    ENABLED       <ON|OFF>        # if OFF, component contributes nothing to the build
    SOURCES       <files…>        # C/ASM sources relative to the component's directory
    INCLUDE_DIRS  <dirs…>         # added to global include path (all other components see these)
    REQUIRES      <names…>        # declared dependencies on other component names
    ROOT_THREAD                   # flag: this component provides ulmk_root_thread()
    LINKER_FRAGMENT <file>        # optional .ld.in fragment; appended to generated linker script
)
```

### Field semantics

| Field | Required | Notes |
|---|---|---|
| `NAME` | yes | Identifier used in `REQUIRES` and `-DUL_COMPONENT_NAME=<name>` |
| `ENABLED` | yes | Manifest default (`OFF` in repo). Overridden by `ULMK_COMP_<name>_ENABLED` cache var (`dev.py build`) |
| `SOURCES` | yes | Paths relative to the component directory |
| `INCLUDE_DIRS` | no | `PUBLIC` on `ulmk_comp_<name>`; propagated to `REQUIRES` dependents |
| `REQUIRES` | no | Dependency on another component name; see enforcement rules below |
| `ROOT_THREAD` | no (flag) | Marks the component providing `ulmk_root_thread()`; exactly one must be ENABLED |
| `LINKER_FRAGMENT` | no | Appended verbatim after kernel linker sections |

### Dependency enforcement

| Scenario | Result |
|---|---|
| Component DISABLED, no one requires it | Silently excluded — no error |
| Component A (ENABLED) requires component B (DISABLED) | Configure-time `FATAL_ERROR` in `ulmk_components_finalize()` with `dev.py` hint |
| Component A (DISABLED) requires component B (ENABLED) | No error — A is not built |
| Caller references `<dep>_init()` but dep DISABLED | Link error (`undefined reference`) as secondary guard |

### Runtime enable (`dev.py components`)

Components default to **OFF** in every `CMakeLists.txt`. The developer enables
them explicitly:

```bash
python3 tools/dev.py components list
python3 tools/dev.py components enable hello_world
python3 tools/dev.py build --board boards/qemu_riscv_virt
python3 tools/dev.py build qemu --board boards/qemu_riscv_virt
```

Selection is stored in `.ulmk/components.conf` (gitignored). One-shot overrides:

```bash
python3 tools/dev.py build --no-components --component hello_world
python3 tools/dev.py build --no-components --component ping_pong
python3 tools/dev.py build --no-components   # kernel-only; uses root_thread stub
```

CMake receives `-DULMK_COMP_<name>_ENABLED=ON|OFF` for every discovered component.

### Example

```cmake
# components/hello_world/CMakeLists.txt
ulmk_component_register(
    NAME         hello_world
    ENABLED      OFF
    SOURCES
        src/root_thread.c
        src/hello_world.c
    INCLUDE_DIRS include
    ROOT_THREAD
)
```

---

## 4. Discovery and Scan Rules

The build system scans the following locations **in order**:

```
1. ${CMAKE_SOURCE_DIR}/components/     built-in kernel components (this repo)
2. ${CMAKE_SOURCE_DIR}/../ulmk_apps/   sibling app directory (fixed name, not configurable)
3. ${ULMK_CHIP_DIR}/                     board directory (existing board.cmake policy, unchanged)
```

### Fixed sibling name

The sibling directory is always `ulmk_apps` — this is not a CMake variable. The build silently skips the sibling scan if the directory does not exist.

### Scan algorithm

For each scan root (1 and 2 above; the board dir is handled separately via `board.cmake`):

1. Enumerate immediate subdirectories (non-recursive at this level).
2. If a subdirectory contains a `CMakeLists.txt`, call `add_subdirectory()` on it.
3. That `CMakeLists.txt` either calls `ulmk_component_register()` once, **or**
   acts as an aggregator that `add_subdirectory()`s child packages which each
   register (e.g. `ulmk_apps/silicon/CMakeLists.txt` → `silicon_*`).
4. `ENABLED OFF` → component known but contributes nothing.
5. `ENABLED ON` → sources, include dirs, and optional linker fragment accumulated.

Kernel/board scans stay shallow; nesting is an apps-package concern.

### Board directory

`${ULMK_CHIP_DIR}/` is **not** component-scanned. Board sources are compiled unconditionally via `board.cmake`. Board services expose their C API through headers in the board include path; no scan step is needed.

---

## 5. Build Flow

### CMake configure

```
scan kernel/components/     → ulmk_component_register() calls
scan ../ulmk_apps/          → ulmk_component_register() calls  (if directory exists)
include ${ULMK_CHIP_DIR}/board.cmake  → board sources + flags (unchanged)

validate (ulmk_components_finalize):
  • at most one ENABLED component has ROOT_THREAD
  • all REQUIRES of ENABLED components are themselves ENABLED
  • inter-component target_link_libraries for REQUIRES deps

if no ENABLED component has ROOT_THREAD:
  link stub/root_thread_stub.c (minimal ulmk_root_thread → ulmk_thread_exit)

accumulate from ENABLED components:
  • each → static library ulmk_comp_<name>
  • include dirs → PUBLIC on that library
  • domain_data.ld.in snippet via ulmk_add_domain()
```

### CMake build

```
compile: kernel sources + board sources + all enabled component sources
link:    ulipe.elf  -T generated.ld
```

### dev.py as CMake frontend

`dev.py build` invokes CMake configure + Ninja inside the container. It passes
`-DULMK_COMP_<name>_ENABLED=…` for every discovered component (from
`.ulmk/components.conf` and/or `--component` flags). See `tools/dev.py components
--help`.

```
dev.py build             → cmake configure (QEMU board) + cmake build
dev.py build --clean     → rm -rf build/ + cmake configure + cmake build
dev.py run               → build (if stale) + qemu-system-tricore
dev.py build --board /p  → cmake configure with ULMK_CHIP_DIR=/p + build
```

### What changes vs. today

| Today | With components |
|---|---|
| `../ulmk_apps` is a single CMakeLists.txt | Multiple component subdirs, each with its own descriptor |
| Root thread stub always linked | Root thread comes from the `ROOT_THREAD` component |
| No per-component include paths | Each component exposes `include/` globally |
| No per-component linker fragments | Optional `linker.ld.in` per component |
| dev.py maintains its own source list | dev.py delegates entirely to CMake |

---

## 6. Linker Fragment Contract

If a component declares `LINKER_FRAGMENT`, the `.ld.in` file is appended to the generated linker script after all kernel sections.

### Allowed

- `SECTIONS` blocks using `ULMK_DOMAIN_BSS(name)` / `ULMK_DOMAIN_DATA(name)` macros (`include/ulmk/linker.h`)
- `ALIGN(ULMK_MPU_ALIGN)` before each domain boundary

### Not allowed

- `MEMORY` blocks — those live in Layer 3 (`memory.ld`)
- Redefinition of kernel regions (`KERNEL_FLASH`, `KERNEL_RAM`, `SHARED_RAM`, `PERIPH`)
- Absolute address assignments

### Example

```ld
/* boards/qemu_tc3xx/board_console.ld.in */
.domain_board_console (NOLOAD) :
{
    . = ALIGN(ULMK_MPU_ALIGN);
    _ulmk_domain_board_console_start = .;
    *(.domain_board_console.bss)
    *(.domain_board_console.data)
    _ulmk_domain_board_console_end = .;
    . = ALIGN(ULMK_MPU_ALIGN);
} > KERNEL_RAM
```

---

## 7. Root Thread Ownership and Component Init

Exactly **one** enabled component must declare `ROOT_THREAD`. The build fails at configure time if zero or more than one component declares it.

### Component init convention

Every component's public header exports an `<name>_init()` function. This function:

1. Creates the IPC endpoint and stores it in a module-internal static variable.
2. Spawns the service thread, which calls `ulmk_ep_recv` on the pre-created endpoint.
3. Returns the spawned TID (or `ULMK_TID_INVALID` on double-init).

Because the endpoint is created in step 1 — before any other thread runs — client calls to the component's API are safe as soon as `<name>_init()` returns. No spin-wait, no race.

```c
/* include/my_component.h */
ulmk_tid_t my_component_init(void);   /* spawns service thread; endpoint is ready on return */
void     my_component_do_thing(int arg);  /* client API — calls IPC internally */
```

The root thread calls component inits in whatever order is needed:

```c
void ulmk_root_thread(const ulmk_boot_info_t *info)
{
    board_services_init(info);  /* board-provided weak-overridable function */
    my_component_init();

    ulmk_thread_exit();
}
```

`board_services_init()` follows the same convention: it creates board service endpoints and spawns board service threads before returning. Its weak no-op stub lives in `stub/board_services_stub.c`. Real boards provide a strong definition in their board sources.

---

## 8. Component Service Pattern — IPC as Implementation Detail

IPC is an implementation mechanism, not part of a component's public interface. Callers use the component's C API; they never manipulate endpoints directly.

### Structure

```
my_component/
├── include/my_component.h    ← public: init + service calls (no ulmk_ep_t exposed)
└── src/
    ├── server.c               ← private: ulmk_ep_recv loop, business logic
    └── client.c               ← private: IPC wrappers backing the public API
```

### Skeleton

```c
/* src/client.c — compiled into every caller's address space */
#include <my_component.h>
#include <ulmk/microkernel.h>

#define MY_SVC_MSG_DO_THING  1u

static ulmk_ep_t g_ep;

ulmk_tid_t my_component_init(void)
{
    static int done;
    if (done)
        return ULMK_TID_INVALID;
    done = 1;

    g_ep = ulmk_ep_create();   /* endpoint ready before server thread starts */

    ulmk_thread_attr_t attr = {
        .name       = "mysvc",
        .entry      = my_service_entry,   /* defined in server.c */
        .priority   = 5u,
        .stack_size = 1024u,
        .privilege  = ULMK_PRIV_DRIVER,
    };
    return ulmk_thread_create(&attr);
}

void my_component_do_thing(int arg)
{
    ulmk_msg_t msg = { .label = MY_SVC_MSG_DO_THING };
    msg.data[0] = (uint32_t)arg;
    ulmk_ep_call(g_ep, &msg, NULL);
}
```

```c
/* src/server.c */
#include <ulmk/microkernel.h>

extern ulmk_ep_t g_ep;   /* defined in client.c */

void my_service_entry(void *arg)
{
    for (;;) {
        ulmk_msg_t msg;
        ulmk_tid_t sender;
        ulmk_ep_recv(g_ep, &msg, &sender);

        switch (msg.label) {
        case MY_SVC_MSG_DO_THING:
            /* handle ... */
            break;
        }
        ulmk_ep_reply(sender, &(ulmk_msg_t){0});
    }
}
```

---

## 9. hello_world Component Design

**Location:** `components/hello_world/`

**Purpose:** Reference builtin `ROOT_THREAD` component. Starts board services
and the hello task. Independent of `ping_pong` (which lives in `ulmk_apps`).

### CMakeLists.txt

```cmake
ulmk_component_register(
    NAME         hello_world
    ENABLED      OFF
    SOURCES
        src/root_thread.c
        src/hello_world.c
    INCLUDE_DIRS include
    ROOT_THREAD
)
```

### Sketches

**root_thread.c:**
```c
#include <hello_world.h>
#include <ulmk/microkernel.h>

void board_services_init(const ulmk_boot_info_t *info);

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
    board_services_init(info);
    hello_world_init(info);
    ulmk_thread_exit();
}
```

**hello_world.c:**
```c
#include <hello_world.h>
#include <board_console.h>    /* board_console_puts(), board_console_putc() */
#include <ulmk/microkernel.h>

static void hello_entry(void *arg)
{
    (void)arg;
    board_console_puts("ulmk: hello from userspace — hello world!\n");
    ulmk_thread_exit();
}

ulmk_tid_t hello_world_init(const ulmk_boot_info_t *info)
{
    static int done;
    if (done)
        return ULMK_TID_INVALID;
    done = 1;

    ulmk_thread_attr_t attr = {
        .name       = "hello",
        .entry      = hello_entry,
        .priority   = 10u,
        .stack_size = 1024u,
        .privilege  = ULMK_PRIV_USER,
    };
    return ulmk_thread_create(&attr);
}
```

---

## 10. ping_pong Component Design

**Location:** `../ulmk_apps/ping_pong/` (sibling apps repo, not builtin)

**Purpose:** Standalone IPC demo. Exercises multi-component linking
(`ulmk_comp_ping_pong.a` + MPU domain), synchronous IPC (`ep_call` /
`ep_recv` / `ep_reply`), and board timer/console without board-specific
headers. Has its own `ROOT_THREAD` — do not enable together with
`hello_world`.

```bash
python3 tools/dev.py build --no-components --component ping_pong
```

### File layout

```
ulmk_apps/ping_pong/
├── CMakeLists.txt
├── include/ping_pong.h
└── src/
    ├── root_thread.c
    └── ping_pong.c
```

### CMakeLists.txt

```cmake
ulmk_component_register(
    NAME         ping_pong
    ENABLED      OFF
    SOURCES
        src/root_thread.c
        src/ping_pong.c
    INCLUDE_DIRS include
    ROOT_THREAD
)
```

---

## 11. board_console — Board Source Service

`board_console` is **not** a portable component. It is a board source compiled via `ULMK_BOARD_SOURCES` in `boards/qemu_tc3xx/board.cmake`.

### Rationale

The console MMIO address, peripheral type, and initialisation sequence are chip-specific. Placing board_console in board sources avoids a cross-board abstraction at this stage.

### Public C API

Callers `#include <board_console.h>` and call functions. No endpoint handle is ever exposed outside the module.

```c
/* boards/qemu_tc3xx/board_console.h */
#ifndef BOARD_CONSOLE_H
#define BOARD_CONSOLE_H

void board_console_putc(char c);
void board_console_puts(const char *s);

#endif
```

### Location

```
boards/qemu_tc3xx/
├── board.cmake             # adds board_console.c + board_services.c to ULMK_BOARD_SOURCES
├── board_console.h         # public C API — included by any component that uses the console
├── board_console.c         # client API + server thread + internal endpoint management
├── board_services.h        # declares board_services_init()
└── board_services.c        # board_services_init(): creates ep, spawns console thread
```

`board_services.h` is also added to the board include path so that `root_thread.c` in `hello_world` can call `board_services_init()` without a compile-time dependency on a specific board's `CMakeLists.txt`.

A weak no-op stub in `stub/board_services_stub.c` allows kernel-only builds (no board services) to link cleanly.

### Implementation sketch

```c
/* boards/qemu_tc3xx/board_console.c */
#include <board_console.h>
#include <ulmk/microkernel.h>

#define CONSOLE_MSG_PUTC  1u

/*
 * Endpoint created by board_services_init() before the server thread runs.
 * Visible to both client API functions and the server entry point.
 */
static ulmk_ep_t g_ep;

/* ---- client API ---- */

void board_console_putc(char c)
{
    ulmk_msg_t msg = { .label = CONSOLE_MSG_PUTC };
    msg.data[0]  = (uint8_t)c;
    ulmk_ep_call(g_ep, &msg, NULL);
}

void board_console_puts(const char *s)
{
    while (*s)
        board_console_putc(*s++);
}

/* ---- server thread ---- */

static void console_server(void *arg)
{
    volatile uint32_t *virt = ulmk_mem_map(
        (void *)ULMK_BOARD_VIRT_CONSOLE_BASE,
        4u,
        ULMK_PERM_READ | ULMK_PERM_WRITE,
        ULMK_MMAP_PERIPH);

    for (;;) {
        ulmk_msg_t msg;
        ulmk_tid_t sender;
        ulmk_ep_recv(g_ep, &msg, &sender);
        if (msg.label == CONSOLE_MSG_PUTC)
            *virt = (uint32_t)msg.data[0];
        ulmk_ep_reply(sender, &(ulmk_msg_t){0});
    }
}
```

```c
/* boards/qemu_tc3xx/board_services.c */
#include <board_services.h>
#include <board_console.h>
#include <ulmk/microkernel.h>

extern ulmk_ep_t g_ep;   /* defined in board_console.c */

void board_services_init(const ulmk_boot_info_t *info)
{
    /*
     * Create endpoint before spawning the thread. Any call to
     * board_console_putc() after this function returns is safe.
     */
    g_ep = ulmk_ep_create();

    ulmk_thread_attr_t attr = {
        .name       = "bcon",
        .entry      = console_server,
        .priority   = 1u,
        .stack_size = 1024u,
        .privilege  = ULMK_PRIV_DRIVER,
    };
    ulmk_thread_create(&attr);
}
```

---

## 12. Open Questions

*(No outstanding decisions. Update this section as new open points arise.)*
