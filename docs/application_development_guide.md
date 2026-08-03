# ulmk — Application Development Guide

> **Purpose of this document:** step-by-step guide for building an application
> on top of ulmk, targeting custom hardware.  Covers the component
> model, what files to create, how to wire the board, and how to run on QEMU
> or real hardware.

---

## Table of Contents

1. [Concepts](#1-concepts)
2. [Repository Layout for an Application](#2-repository-layout-for-an-application)
3. [Creating a Component](#3-creating-a-component)
4. [Creating the Root Thread](#4-creating-the-root-thread)
5. [Per-Thread Heap (slabAO Model)](#5-per-thread-heap-slabao-model)
6. [Providing Board Services](#6-providing-board-services)
7. [Creating a Custom Board (chip input)](#7-creating-a-custom-board-chip-input)
8. [Configuration Model (per layer)](#8-configuration-model-per-layer)
9. [Build](#9-build)
10. [Output Artefacts](#10-output-artefacts)
11. [Running on QEMU](#11-running-on-qemu)
12. [Running on Real Hardware](#12-running-on-real-hardware)
13. [Device Manager (pathname I/O)](#13-device-manager-pathname-io)
14. [Worked Example](#14-worked-example)
15. [SDK Mode — Integrating ulmk into an External Toolchain](#15-sdk-mode--integrating-ulmk-into-an-external-toolchain)

---

## 1. Concepts

### Single ELF, MPU isolation

The entire system — kernel, board services, all application components — links
into **one ELF file**.  Isolation is enforced at runtime by the hardware MPU,
not by separate binaries.

### Component

A directory containing a `CMakeLists.txt` that calls `ulmk_component_register()`.
Its sources are compiled into the `ulmk_kernel` static library.  Its public
header is added to the global include path.

Exactly one component must declare `ROOT_THREAD`.  That component provides
`ulmk_root_thread()`, which is the first userspace function the kernel calls.

### Board services

Hardware-dependent services (console, clocks, peripherals) live in the board
directory, not in components.  The board provides three mandatory symbols:

| Symbol | Called from | What it does |
|--------|-------------|-------------|
| `ulmk_board_init(void)` | `startup.S` before `.data` copy | PLL, flash WS, ext RAM |
| `ulmk_printk_char_out(char)` | kernel printk | single-character debug output |
| `board_services_init(const ulmk_boot_info_t *)` | `ulmk_root_thread()` | spawn background service threads |

---

## 2. Repository Layout for an Application

```
my_project/
├── ulmk_apps/                ← auto-discovered sibling of ulmk/
│   ├── my_app/               ← application component (ROOT_THREAD)
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── my_app.h
│   │   └── src/
│   │       ├── root_thread.c
│   │       └── my_task.c
│   └── my_driver/            ← driver component
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── my_driver.h
│       └── src/
│           ├── server.c
│           └── client.c
│
├── ulmk/         ← kernel repo (git submodule or clone)
│
└── my_board/                 ← chip input for real hardware
    ├── board.cmake
    ├── board_config.h        ← SoC MMIO bases, IRQ ctrl, platform quirks
    ├── memory.ld
    ├── board_console.c       ← ulmk_printk_char_out via UART
    └── board_services.c      ← ulmk_board_init + board_services_init
```

Place `ulmk_apps/` as a sibling of `ulmk/`.  The build
auto-discovers it with a **shallow** scan of immediate children:

```cmake
# From CMakeLists.txt — happens automatically
if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/../ulmk_apps")
    file(GLOB dirs LIST_DIRECTORIES true "../ulmk_apps/*")
    foreach(dir IN LISTS dirs)
        add_subdirectory(${dir} ...)
    endforeach()
endif()
```

A child may be a single component **or** an aggregator package.  Example —
silicon cert suite:

```
ulmk_apps/silicon/
├── CMakeLists.txt          ← GLOB children + add_subdirectory
├── silicon_baseline/
├── silicon_e2e/
└── …
```

Recursion is the aggregator's responsibility; the kernel scan stays shallow.

---

## 3. Creating a Component

### Minimum file set

```
my_component/
├── CMakeLists.txt
├── include/
│   └── my_component.h    ← public C API
└── src/
    ├── server.c           ← IPC receive loop (if this is a service)
    └── client.c           ← public API wrappers calling IPC
```

### CMakeLists.txt

```cmake
ulmk_component_register(
    NAME         my_component
    ENABLED      ON
    SOURCES      src/server.c
                 src/client.c
    INCLUDE_DIRS include
)
```

Flags:
- `ROOT_THREAD` — set on exactly one component; that component provides
  `ulmk_root_thread()`.
- `REQUIRES other_component` — build fails if `other_component` is DISABLED.
- `LINKER_FRAGMENT my_component.ld.in` — optional memory domain fragment.
- `ENABLED OFF` — exclude from build without removing the directory.

### Public header convention

```c
/* include/my_component.h */
#ifndef MY_COMPONENT_H
#define MY_COMPONENT_H

#include <ulmk/microkernel.h>

/*
 * my_component_init — spawn the service thread and create the IPC endpoint.
 * The endpoint is ready before this function returns; callers can invoke the
 * service API immediately.  Returns ULMK_TID_INVALID on double-init.
 */
ulmk_tid_t my_component_init(void);

/* Service API — IPC details are an internal implementation detail. */
int my_component_do_thing(int arg);

#endif
```

### Service pattern (server + client)

```c
/* src/server.c */
#include <my_component.h>
#include <ulmk/microkernel.h>

#define MSG_DO_THING  1u

static ulmk_ep_t g_ep;

static void server_thread(void *arg)
{
    ulmk_msg_t msg;
    ulmk_tid_t sender;

    (void)arg;
    for (;;) {
        ulmk_ep_recv(g_ep, &msg, &sender);
        /* process msg.label, msg.words[] */
        ulmk_msg_t reply = { .label = ULMK_OK };
        ulmk_ep_reply(sender, &reply);
    }
}

ulmk_tid_t my_component_init(void)
{
    static int done;
    if (done)
        return ULMK_TID_INVALID;
    done = 1;

    g_ep = ulmk_ep_create();   /* endpoint ready before thread starts */

    ulmk_thread_attr_t attr = {
        .name       = "my_comp",
        .entry      = server_thread,
        .arg        = NULL,
        .priority   = 10,
        .stack_size = 1024,
        .privilege  = ULMK_PRIV_DRIVER,
    };
    return ulmk_thread_create(&attr);
}
```

```c
/* src/client.c */
#include <my_component.h>
#include <ulmk/microkernel.h>

extern ulmk_ep_t g_ep;  /* defined in server.c */

int my_component_do_thing(int arg)
{
    ulmk_msg_t msg = { .label = MSG_DO_THING, .words = { (uint32_t)arg } };
    int ret = ulmk_ep_call(g_ep, &msg);
    return ret == ULMK_OK ? (int)msg.words[0] : ret;
}
```

---

## 4. Creating the Root Thread

The root thread component declares `ROOT_THREAD` and provides `ulmk_root_thread()`.

```cmake
# ulmk_apps/my_app/CMakeLists.txt
ulmk_component_register(
    NAME         my_app
    ENABLED      ON
    SOURCES      src/root_thread.c
                 src/main_task.c
    INCLUDE_DIRS include
    ROOT_THREAD
)
```

```c
/* src/root_thread.c */
#include <ulmk/microkernel.h>
#include <board_services.h>   /* provided by the board */
#include <my_driver.h>
#include <my_app.h>

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
    /*
     * 1. Board services: console, clocks, etc.
     *    board_services_init() creates endpoints and spawns board threads
     *    before returning.  Service APIs are safe to call immediately after.
     */
    board_services_init(info);

    /*
     * 2. Components, in dependency order.
     *    Each *_init() creates its endpoint and spawns its service thread.
     */
    my_driver_init();
    my_app_init(info);

    /*
     * 3. Optionally delegate capabilities before exiting.
     */
    /* ulmk_cap_grant(driver_tid, ULMK_CAP_IRQ | ULMK_CAP_MAP_PERIPH); */

    ulmk_thread_exit();
}
```

---

## 5. Per-Thread Heap (slabAO Model)

Each thread may carry a private heap allocated at creation time.  Set
`attr.heap_size` to the number of bytes the thread needs beyond its stack:

```c
ulmk_thread_attr_t attr = {0};
attr.name       = "worker";
attr.entry      = worker_entry;
attr.priority   = 5;
attr.stack_size = 2048;
attr.privilege  = ULMK_PRIV_DRIVER;
attr.heap_size  = 8192;   /* 8 KiB private heap */
ulmk_tid_t tid = ulmk_thread_create(&attr);
```

The kernel allocates a contiguous *slabAO* (`stack_size + heap_size` bytes)
from `user_pool` and covers it with a single MPU DPR.  The TCB is kept in
a separate allocation so userspace cannot reach kernel metadata through the DPR.

Inside the thread, retrieve the heap area:

```c
ulmk_heap_info_t info;
ulmk_get_thread_heap(&info);   /* info.base, info.size */

uint8_t *heap = (uint8_t *)(uintptr_t)info.base;
/* use heap[0 .. info.size-1] directly */
```

To expand the heap at runtime (requires `ULMK_PRIV_DRIVER`):

```c
int rc = ulmk_heap_extend(4096);   /* add 4 KiB; new DPR entry added */
```

**Notes:**

- `attr.heap_size = 0` means no heap; `ulmk_get_thread_heap()` returns
  `ULMK_EPERM` in that case.
- Always declare `ulmk_thread_attr_t attr = {0};` before setting fields so that
  `heap_size` defaults to zero safely.
- There is no userspace allocator included in the kernel.  The heap is a raw
  contiguous block; use `ulmk_heap_extend()` when more memory is needed.

---

## 6. Providing Board Services

Create a board directory (can be inside `ulmk_apps/` or anywhere on disk):

```
my_board/
├── board.cmake          ← board descriptor
├── board_config.h       ← SoC MMIO bases, IRQ ctrl, platform quirks
├── memory.ld            ← MEMORY block
├── board_console.c      ← ulmk_printk_char_out via UART or semihosting
├── board_services.c     ← ulmk_board_init + board_services_init
└── board_services.h     ← declares board_services_init()
```

### board.cmake

```cmake
set(ULMK_BOARD_CPU   "tc39xx")   # passed to -mcpu=
set(ULMK_BOARD_SOURCES
    board_console.c
    board_services.c
)
```

### board_config.h

SoC MMIO bases and platform quirks (not arch ISA constants):

```c
#define ULMK_BOARD_SRC_BASE      0xF0038000u
#define ULMK_BOARD_SRC_SRE_BIT   10u
#define ULMK_BOARD_STM0_BASE     0xF0001000u
#define ULMK_BOARD_FLASH_BASE    0x80000000u
#define ULMK_BOARD_IDLE_IS_WAIT  0
```

See `boards/board_config.h.template` and `boards/qemu_tc3xx/board_config.h`.

### board_services.c

```c
#include <ulmk/microkernel.h>
#include "board_services.h"

void ulmk_board_init(void)
{
    /*
     * Called before .data copy — no globals, no kernel API.
     * Configure PLL, flash wait states, external RAM here.
     * Leave empty if a bootloader already did this.
     */
}

void ulmk_printk_char_out(char c)
{
    /* Write c to UART TX register or semihosting interface. */
    volatile uint32_t *tx = (volatile uint32_t *)0xF0000020u;
    *tx = (uint32_t)c;
}

void board_services_init(const ulmk_boot_info_t *info)
{
    (void)info;
    /* Spawn any board-level service threads (console IPC server, etc.). */
}
```

### board_services.h

```c
#ifndef BOARD_SERVICES_H
#define BOARD_SERVICES_H

#include <ulmk/microkernel.h>

void board_services_init(const ulmk_boot_info_t *info);

#endif
```

---

## 7. Creating a Custom Board (chip input)

The chip input provides the MEMORY block and linker flags.  See
`docs/linker_spec.md §9` for the full contract.  Minimum:

### memory.ld

```ld
/* memory.ld — chip input for My TC277 board */

ULMK_MPU_ALIGN    = 64;
ULMK_KERNEL_STACK_SIZE = 4096;
ULMK_ISR_STACK_SIZE    = 2048;
ULMK_CSA_POOL_SIZE     = 16384;  /* 256 × 64-byte CSA frames */

HAVE_CSA        = 1;
HAVE_SMALL_DATA = 1;
HAVE_BMHD       = 1;

MEMORY {
    KERNEL_FLASH    (rx)  : ORIGIN = 0x80000000, LENGTH = 512K
    KERNEL_FLASH_NC (rx)  : ORIGIN = 0xA0000000, LENGTH = 512K  /* BMHD lives here */
    KERNEL_RAM      (rwx) : ORIGIN = 0x70000000, LENGTH = 240K
    SHARED_RAM      (rwx) : ORIGIN = 0x90000000, LENGTH = 32K
    PERIPH          (rw)  : ORIGIN = 0xF0000000, LENGTH = 256M
}
```

---

## 8. Configuration Model (per layer)

Configuration is split by **who owns the value**, and a single generator
(`tools/gen_config.py`) folds it into the `generated/ulmk/` headers the build
consumes.  You never edit the generated headers — you edit the layer that owns
each value.

```
Layer        You edit                                Generated / consumed
──────────────────────────────────────────────────────────────────────────────
Kernel       cmake/config.cmake  (cache vars)   ──▶  generated/ulmk/config.h
policy       -DULMK_CONFIG_* on the cmake line        (kernel/*.c include this)

Board /      boards/<soc>/board_config.h        ──▶  generated/ulmk/platform.h
SoC          (MMIO bases, IRQ ctrl, quirks)           (arch_config.h includes this)

Arch /       arch/<isa>/arch_config.h                 (included via ulmk_arch.h;
ISA          (CSFR/CSR addrs, CSA/PMP layout)          ships with the kernel — you
                                                        do not normally touch it)

Memory       <chip_dir>/memory.ld               ──▶  generated/ulmk.ld
sizing       (MEMORY block, stack/CSA sizes)          (linked with -T)
```

**Rules of thumb**

- A number that changes per **product** (IRQ table size, printk on/off) →
  `config.cmake` or `-DULMK_CONFIG_*`.  Canonical defaults and range checks live
  in `tools/gen_config.py`.
- A number that changes per **chip/board** (a peripheral base, an IRQ line, a
  clock, a build quirk) → `board_config.h`.
- A number fixed by the **ISA** (a control-register address, CSA/PMP slot
  layout) → `arch_config.h`.  If you find yourself editing this for a new board,
  the value probably belongs in `board_config.h` instead.
- A memory address/region size → `memory.ld` (see §7).

### Kernel policy knobs

Only symbols the real kernel reads are configurable (TCBs, endpoints and
notifications are heap-allocated, so they have no static-pool caps):

| Symbol | Default | Controls |
|--------|---------|----------|
| `ULMK_CONFIG_MAX_IRQ_BINDINGS` | 16 | `irq.c` SRPN → notif binding table |
| `ULMK_CONFIG_DEBUG_PRINTK` | 1 | kernel `printk` (0 = compile to no-op) |
| `ULMK_CONFIG_IRQ_ATTACH` | 0 | `ulmk_irq_attach` fast-path (1 = DANGEROUS; else `ULMK_ENOTSUP`) |

```bash
cmake -B build -DULMK_CHIP_DIR=... \
      -DULMK_CONFIG_MAX_IRQ_BINDINGS=32 \
      -DULMK_CONFIG_DEBUG_PRINTK=0 \
      -DULMK_CONFIG_IRQ_ATTACH=1
```

`ULMK_CONFIG_IRQ_ATTACH` is **off by default**.  Enabling it is an intentional
kernel aperture: userspace ISR callbacks run on the interrupt path.  Some sample
boards (e.g. `tc275_lite`) set the cache default to `1` for demos; override with
`-DULMK_CONFIG_IRQ_ATTACH=0` if you do not want that surface.

When enabled, the public wrappers are `ulmk_irq_attach` / `ulmk_irq_attach_hw` /
`ulmk_irq_detach` (syscalls **65–67**).  Full contract, return encoding, and the
anti-nested-syscall gate are in [`docs/api_spec.md`](api_spec.md) §10 and §12.
Demo on TC275 Lite: `ulmk_boards/.../components/board_irq_attach_blinky`.

---

## 9. Build

```bash
# Enter the dev container
python3 tools/dev.py

# Inside the container — build with custom board
python3 tools/dev.py build --board /workspace/../my_board

# Or directly with cmake
cmake -B /build/my_project \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-tricore-gcc.cmake \
      -DULMK_CHIP_DIR=/workspace/../my_board
cmake --build /build/my_project
```

Key CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `ULMK_CHIP_DIR` | `boards/qemu_tc3xx` | Path to chip input directory |
| `ULMK_CONFIG_*` | see §8 | Kernel policy knobs (`config.cmake`) |

See [§8 Configuration Model](#8-configuration-model-per-layer) for the full set of
knobs and which layer owns each value.

---

## 10. Output Artefacts

After a successful build:

```
build/
└── ulipe/
    ├── ulmk                     ← final ELF
    ├── libulmk_kernel.a         ← kernel + board + component objects
    └── generated/
        ├── ulmk.ld              ← generated linker script
        └── ulmk/
            ├── config.h         ← kernel policy (from tools/gen_config.py)
            └── platform.h       ← SoC snapshot of board_config.h
```

| Artefact | Use |
|----------|-----|
| `ulmk` | Flash or load onto target |
| `ulmk.ld` | Inspect section layout, debug address issues |
| `libulmk_kernel.a` | Not needed on-target; used by the link step only |

Convert to Intel HEX or binary for flashing:

```bash
tricore-elf-objcopy -O ihex ulmk ulmk.hex
tricore-elf-objcopy -O binary ulmk ulmk.bin
```

Inspect section sizes:

```bash
tricore-elf-size ulmk
tricore-elf-objdump -h ulmk | grep -E "text|data|bss|startup"
```

---

## 11. Running on QEMU

```bash
# Inside the dev container
python3 tools/dev.py run

# Or directly
qemu-system-tricore -machine KIT_AURIX_TC397B_TRB \
    -kernel /build/ulipe/ulmk \
    -nographic
```

Press `Ctrl+A X` to exit QEMU.

QEMU limitations:
- Starts execution at `0x80000000` (reset address), not at the ELF entry point.
  The `.startup` linker section ensures `_start` is placed there.
- MMIO console output at `0xBF000020` (VIRT device) — configured in
  `boards/qemu_tc3xx/qemu_console.c`.
- No flash programming — `ulmk` is loaded directly as an ELF.

---

## 12. Running on Real Hardware

### Flash via Lauterbach Trace32

```
; trace32 script snippet
Data.LOAD.Elf ulmk /NoCODE
FLASH.Program ALL
System.Up
Go
```

### Flash via MemTool / DAS (Infineon)

```bash
# Requires DAS and the appropriate TC2xx/TC3xx device package
infineon-memtool -d TC277 -f ulmk.hex
```

### OpenOCD + GDB (for development)

```bash
openocd -f interface/aurix.cfg -f target/tc277.cfg &
tricore-elf-gdb ulmk \
    -ex "target remote :3333" \
    -ex "monitor reset halt" \
    -ex "load" \
    -ex "continue"
```

### JTAG boot sequence

After flashing:

1. Power cycle or reset the board.
2. The chip ROM loads the BMHD (if `HAVE_BMHD = 1`) and jumps to `_start`.
3. `ulmk_board_init()` runs (PLL, flash WS).
4. `.data` copy and `.bss` zero.
5. `ulmk_arch_init()` — CSA pool, vectors, MPU.
6. `ulmk_kern_main()` — scheduler starts.
7. `ulmk_root_thread()` — your application code.

---

## 13. Device Manager (pathname I/O)

Full specification: [`docs/device_manager_spec.md`](device_manager_spec.md).

### Mechanism vs policy

| In the **kernel** repo (`ulmk`) | Outside (`ulmk_apps` / board) |
|--------------------------------|-------------------------------|
| `ulmk_device_manager` component | `ulmk_device_classes` (display, input, can, …) |
| `ulmk_open` / `read` / `write` / `ioctl` | Class ioctl codes and helpers |
| `ulmk_dev_ops` + `ulmk_dev_serve` | Board `drivers/*_dm/` adapters |

Do **not** add class headers under `ulmk/components/`.

### App sketch

```c
#include <ulmk_device.h>
#include <ulmk_device_display.h>   /* from ulmk_device_classes */

board_devices_register();         /* board: dm_init + ulmk_dev_register */
ulmk_open("/dev/disp0", &disp);
ulmk_disp_write_present(&disp, fb, rects, n);
```

CMake: `REQUIRES ulmk_device_manager ulmk_device_classes`.

### Writing a board adapter

1. Keep legacy `drivers/foo/` for old demos.
2. Add `drivers/foo_dm/` — one thread, `ulmk_dev_serve`, ops call legacy clients.
3. Register `/dev/foo0` from `board_devices_*` or the demo root.
4. Put DM BSS in `.user_bss` on TriCore; never call `foo_init()` from inside an ops handler that is already serving an `ep_call`.

## 14. Worked Example

Create a minimal application that blinks an LED via a driver component.

> **Note:** addresses and peripheral layout in this example are based on the
> AURIX TC2xx/TC3xx GPIO port registers (`0xF003A000`).  Adapt base addresses
> and register offsets for your target.

### File structure

```
ulmk_apps/
├── led_blink/          ← ROOT_THREAD component
│   ├── CMakeLists.txt
│   └── src/root_thread.c
└── gpio_driver/        ← driver component
    ├── CMakeLists.txt
    ├── include/gpio_driver.h
    └── src/gpio_driver.c
```

### gpio_driver/CMakeLists.txt

```cmake
ulmk_component_register(
    NAME         gpio_driver
    ENABLED      ON
    SOURCES      src/gpio_driver.c
    INCLUDE_DIRS include
)
```

### gpio_driver/include/gpio_driver.h

```c
#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <ulmk/microkernel.h>

ulmk_tid_t gpio_driver_init(void);
void     gpio_set(uint8_t pin, int val);

#endif
```

### gpio_driver/src/gpio_driver.c

The server thread must map the GPIO peripheral region before accessing it.
`ulmk_mem_map()` with `ULMK_MMAP_PERIPH` requires `ULMK_CAP_MAP_PERIPH`; the root
thread grants this capability to the driver thread before it starts.

```c
#include <gpio_driver.h>
#include <ulmk/microkernel.h>

/*
 * TC2xx/TC3xx PORT20 output register — AURIX peripheral base 0xF003A000.
 * Size covers the full port register block (one page, 4 KB).
 * Adjust base address and size for your target.
 */
#define GPIO_PERIPH_BASE  0xF003A000u
#define GPIO_PERIPH_SIZE  0x1000u      /* 4 KB */

#define MSG_GPIO_SET  1u

static ulmk_ep_t          g_ep;
static volatile uint32_t *g_port;

static void gpio_server(void *arg)
{
    ulmk_msg_t msg;
    ulmk_tid_t sender;

    /*
     * Map the peripheral region into this thread's address space.
     * Must be done here (inside the thread), not in gpio_driver_init(),
     * because ulmk_mem_map with ULMK_MMAP_PERIPH operates on the calling thread's
     * MPU context.
     */
    g_port = ulmk_mem_map((void *)GPIO_PERIPH_BASE, GPIO_PERIPH_SIZE,
                        ULMK_PERM_READ | ULMK_PERM_WRITE,
                        ULMK_MMAP_PERIPH);

    (void)arg;
    for (;;) {
        ulmk_ep_recv(g_ep, &msg, &sender);
        if (msg.label == MSG_GPIO_SET && g_port) {
            if (msg.words[1])
                *g_port |= (1u << msg.words[0]);
            else
                *g_port &= ~(1u << msg.words[0]);
        }
        ulmk_msg_t reply = { .label = g_port ? ULMK_OK : ULMK_EPERM };
        ulmk_ep_reply(sender, &reply);
    }
}

ulmk_tid_t gpio_driver_init(void)
{
    static int done;
    if (done)
        return ULMK_TID_INVALID;
    done = 1;
    g_ep = ulmk_ep_create();
    ulmk_thread_attr_t attr = {
        .name = "gpio", .entry = gpio_server, .arg = NULL,
        .priority = 5, .stack_size = 512, .privilege = ULMK_PRIV_DRIVER,
    };
    return ulmk_thread_create(&attr);
}

void gpio_set(uint8_t pin, int val)
{
    ulmk_msg_t msg = { .label = MSG_GPIO_SET, .words = { pin, (uint32_t)val } };
    ulmk_ep_call(g_ep, &msg);
}
```

### led_blink/CMakeLists.txt

```cmake
ulmk_component_register(
    NAME         led_blink
    ENABLED      ON
    SOURCES      src/root_thread.c
    INCLUDE_DIRS include
    REQUIRES     gpio_driver
    ROOT_THREAD
)
```

### led_blink/src/root_thread.c

```c
#include <ulmk/microkernel.h>
#include <board_services.h>
#include <board_console.h>
#include <gpio_driver.h>

static void blink_task(void *arg)
{
    (void)arg;
    for (;;) {
        gpio_set(0, 1);
        (void)ulmk_sleep_ms(500u);  /* or board_timer_sleep_us(500000u) */
        gpio_set(0, 0);
        (void)ulmk_sleep_ms(500u);
    }
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
    board_services_init(info);

    /*
     * gpio_driver_init() creates the endpoint and spawns the server thread.
     * The server maps its peripheral region on its own, but it needs
     * ULMK_CAP_MAP_PERIPH to do so — grant it before the thread runs.
     */
    ulmk_tid_t gpio_tid = gpio_driver_init();
    ulmk_cap_grant(gpio_tid, ULMK_CAP_MAP_PERIPH);

    ulmk_thread_attr_t attr = {
        .name = "blink", .entry = blink_task, .arg = NULL,
        .priority = 20, .stack_size = 512, .privilege = ULMK_PRIV_DRIVER,
    };
    ulmk_thread_create(&attr);

    ulmk_thread_exit();
}
```

### Build and run

```bash
# Build with the custom board
python3 tools/dev.py build --board /workspace/../my_board

# Flash or run on QEMU
python3 tools/dev.py run
```

---

## 15. SDK Mode — Integrating ulmk into an External Toolchain

Everything above builds a **single ELF from the kernel sources** using ulmk's
own CMake/dev.py flow.  That is the right model when ulmk owns the build.

SDK mode is the opposite: it packages the kernel as a **closed, prebuilt drop-in**
(two static archives + a processed linker script + public headers) so you can
add ulmk to an *existing* firmware project driven by a third-party build system —
Eclipse, STM32CubeIDE, the Infineon toolchain, a Makefile tree replacing
FreeRTOS, etc.  You never bring the kernel sources into that project.

### 14.1 When to use it

| Use in-tree build (§9)               | Use SDK mode (this section)                     |
|--------------------------------------|-------------------------------------------------|
| ulmk drives the whole build          | An external IDE/build system owns the firmware  |
| Apps live in `ulmk_apps/` components | Apps live in your existing project tree         |
| You want to hack the kernel          | You want a stable, opaque binary to link        |

### 14.2 Producing the SDK

```bash
# --board is MANDATORY with --kernel.  It may point anywhere — an out-of-tree
# private board directory is the expected case (see §7 for the chip input).
python3 tools/dev.py build --kernel --board boards/qemu_tc3xx
python3 tools/dev.py build --kernel --board /path/to/my_board
```

Output tree (`build/ulipe-<arch>-sdk/dist/ulmk/`, with
`<tag> = <arch>_<board>_gcc`):

```
ulmk/
  lib/
    libulmk_kernel_<tag>.a       kernel + arch — supervisor code/data (CPR0)
    libulmk_board_<tag>.a        startup + vectors + board services + user
                                 entry — driver-privilege userspace (CPR1)
  linker/
    linker_<tag>.ld              fully-processed, self-contained linker script
  include/
    ulmk/*.h                     public microkernel API (microkernel.h, …)
    ulmk_syscall_abi.h           arch SYSCALL ABI; <ulmk/syscall_abi.h> only
                                 redirects to it, and <ulmk/microkernel.h>
                                 pulls it in — it MUST be on the include path
    board/*.h                    public board API (board_services.h,
                                 board_console.h, …); no *internal* headers
```

**Two archives, not one.**  The kernel runs at supervisor privilege (CPR0) while
board services run as driver-privilege userspace threads (CPR1); the linker
separates the two by archive name.  Always link **both**, wrapped in a group so
the kernel⇄board cross-references resolve regardless of order.

### 14.3 What you must provide

The consumer supplies exactly one function — the root thread (§4):

```c
#include <ulmk/microkernel.h>
#include <board/board_services.h>

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
    board_services_init(info);          /* start board console/timer, etc. */
    /* spawn your driver/app threads here … */
    ulmk_thread_exit();                 /* must never return */
}
```

Everything else ships in the archives:

- `_start` (reset entry) and the vector tables are force-linked from
  `libulmk_board_<tag>.a` via `EXTERN(...)` in the linker script — **no**
  whole-archive / `--whole-archive` flag is needed.
- `ulmk_board_init()` (early chip setup) and `board_services_init()` (spawns the
  console/timer service threads) live in the board archive.  Use
  `board_services_init(info)` for bring-up — it is the portable entry point.
  Do **not** call board-internal helpers like `board_console_start()` directly;
  they are board-private and may be no-op stubs on some boards.

### 14.4 Linking against the SDK

Compile your sources with both include dirs, then link both archives against the
shipped linker script with `-nostartfiles` (ulmk provides `_start`):

```bash
CC=tricore-elf-gcc          # or riscv-none-elf-gcc
SDK=path/to/ulmk

$CC -mcpu=tc39xx -ffreestanding -O2 -g \
    -I$SDK/include -I$SDK/include/board \
    -T $SDK/linker/linker_<tag>.ld \
    -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
    my_root_thread.c my_app.c \
    -L$SDK/lib \
    -Wl,--start-group -lulmk_board_<tag> -lulmk_kernel_<tag> -Wl,--end-group \
    -lc -lgcc \
    -o firmware.elf
```

In an IDE, translate this to:

- **Include paths:** `$SDK/include` and `$SDK/include/board`
- **Linker script:** `$SDK/linker/linker_<tag>.ld` (replace any default script)
- **Library search path:** `$SDK/lib`
- **Libraries:** `ulmk_board_<tag>` and `ulmk_kernel_<tag>`, as a group.  The
  two archives reference each other, so a single pass in either order leaves
  undefined symbols.  If the IDE emits its library list outside its own
  `--start-group`, listing the board archive a second time after the kernel
  resolves the cycle just as well.
- **Flags:** `-nostartfiles` (do not let the toolchain inject its own crt0)

### 14.5 Reference consumer

`tests/sdk_e2e/` is a complete, CI-run example of consuming the SDK: its
`Makefile` builds the SDK via `tools/sdk_build.sh`, then compiles a standalone
root thread (`sdk_test.c`) that exercises every public syscall and links it
exactly as above.  Run it end to end with:

```bash
python3 tools/dev.py tests e2e                                  # TriCore
python3 tools/dev.py tests e2e --board boards/qemu_riscv_virt   # RISC-V
```

Use it as the canonical template for wiring ulmk into your own project.
