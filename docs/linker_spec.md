# ulmk — Linker Script Specification

**Version:** 0.2
**Target:** Three-layer linker model — kernel-common / arch / chip (chip input is external)

> **Purpose of this document:** authoritative reference for the three-layer
> linker script model, section layout, domain generation pipeline, and chip
> input contract.  Consult this when adding a new memory region, changing
> section alignment, creating a domain, or porting to a new chip.

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Fragment Model](#2-fragment-model)
3. [Arch Input Contract](#3-arch-input-contract)
4. [Kernel Fragment Catalog](#4-kernel-fragment-catalog)
   - 4.1 [vectors](#41-vectors)
   - 4.2 [kernel_text](#42-kernel_text)
   - 4.3 [kernel_data](#43-kernel_data)
   - 4.4 [kernel_stacks](#44-kernel_stacks)
   - 4.5 [csa_pool (arch-optional)](#45-csa_pool-arch-optional)
   - 4.6 [small_data (arch-optional)](#46-small_data-arch-optional)
   - 4.7 [domain_table](#47-domain_table)
   - 4.8 [user_pool](#48-user_pool)
5. [App and Domain Snippet Templates](#5-app-and-domain-snippet-templates)
   - 5.1 [app_code snippet](#51-app_code-snippet)
   - 5.2 [domain_data snippet](#52-domain_data-snippet)
6. [Exported Symbol Table](#6-exported-symbol-table)
7. [Assembly Model](#7-assembly-model)
8. [C Macro API — linker.h](#8-c-macro-api--linkerh)
9. [TC27x Arch Input Example](#9-tc27x-arch-input-example)
10. [QEMU Arch Input Example](#10-qemu-arch-input-example)
11. [Alignment Constants](#11-alignment-constants)

---

## 1. Design Philosophy

The linker infrastructure has **three independent layers**. Each layer owns its
concerns and only passes a defined contract to the next:

![Linker layers: kernel-common, arch port, chip input and CMake snippets assembled by generate_ld.py into the final linker script](diagrams/linker_layers.png)

**Principles:**

- Kernel fragments reference **abstract region names** only (`KERNEL_FLASH`,
  `KERNEL_RAM`, `SHARED_RAM`). No concrete addresses in kernel or arch layers.
- Chip-specific sections that vary between chip revisions (e.g. BMHD, whose
  layout differs between TC2xx and TC3xx) live in the chip input, not in the
  arch layer.
- Arch-specific features that are uniform across all chips of the same arch
  (e.g. CSA pool, TriCore small-data ABI anchors) live in the arch layer.
- App code sections and memory domain sections are **not hardcoded** anywhere.
  They are generated as snippets from CMake declarations and concatenated by
  `generate_ld.py` before link.
- All symbols exported to C code use the `_ulmk_` prefix.
- MPU alignment is parameterised via `ULMK_MPU_ALIGN`, defined in the chip input.

---

## 2. Fragment Model

### 2.1 Directory structure

```
linker/                              # LAYER 1: kernel-owned, arch-independent
├── kernel/
│   ├── vectors.ld.in                # trap + interrupt vector sections
│   ├── kernel_text.ld.in            # kernel .text and .rodata
│   ├── kernel_data.ld.in            # kernel .data and .bss
│   ├── kernel_stacks.ld.in          # kernel stack + ISR stack
│   ├── domain_table.ld.in           # domain descriptor table
│   └── user_pool.ld.in              # physical allocator pool
└── snippets/
    ├── app_code.ld.in               # template: per-app code section
    └── domain_data.ld.in            # template: per-domain data section

arch/tricore/linker/                 # LAYER 2: arch-owned, TriCore-specific
├── prologue.ld.in                   # OUTPUT_FORMAT, OUTPUT_ARCH, ENTRY
├── csa_pool.ld.in                   # CSA pool (all TriCore chips)
└── small_data.ld.in                 # small-data ABI anchors (A0/A1/A8/A9)

<chip_dir>/                          # LAYER 3: chip input (EXTERNAL, not in repo)
├── memory.ld                        # MEMORY block + ULMK_MPU_ALIGN + HAVE_* flags
└── bmhd.ld.in                       # optional: chip boot header section
```

### 2.2 Assembly order

`cmake/generate_ld.py` concatenates the layers in this fixed order:

```
[chip]  memory.ld               → MEMORY block + linker variables
[arch]  prologue.ld.in          → OUTPUT_FORMAT / OUTPUT_ARCH / ENTRY
        SECTIONS {
[chip]    bmhd.ld.in            → boot header (only if present in chip dir)
[kernel]  vectors.ld.in         → trap table + interrupt table
[kernel]  kernel_text.ld.in     → kernel code
[snip]    app_code/<name>.ld    → one per registered app (code in KERNEL_FLASH)
[kernel]  domain_table.ld.in    → domain descriptor table (in KERNEL_FLASH)
[kernel]  kernel_data.ld.in     → kernel data/bss
[kernel]  kernel_stacks.ld.in   → kernel stack + ISR stack
[arch]    csa_pool.ld.in        → CSA pool (if HAVE_CSA in memory.ld)
[snip]    domain_data/<name>.ld → one per registered domain (data in KERNEL_RAM)
[arch]    small_data.ld.in      → sdata/sbss + GP anchors (if HAVE_SMALL_DATA)
[kernel]  user_pool.ld.in       → remainder of KERNEL_RAM → allocator pool
        }
```

The output `${CMAKE_BINARY_DIR}/generated.ld` is passed to the linker via
`-T ${CMAKE_BINARY_DIR}/generated.ld`.

---

## 3. Input Contracts

### 3.1 Chip input contract (Layer 3 — external)

The chip provides a directory (path passed to `generate_ld.py --chip-dir`) with:

**Required file: `memory.ld`**

Must define the `MEMORY {}` block with these abstract region names:

| Region name | Attrs | Required | Purpose |
|-------------|-------|----------|---------|
| `KERNEL_FLASH` | `rx` | yes | Cached/executable flash for code |
| `KERNEL_RAM` | `rwx` | yes | Primary data RAM (data, bss, stacks, domains) |
| `SHARED_RAM` | `rwx` | yes | RAM shared across cores; IPC buffers |
| `PERIPH` | `rw` | yes | Peripheral SFR space (no execute) |
| `KERNEL_FLASH_NC` | `rx` | only if `HAVE_BMHD = 1` | Non-cached flash alias for boot header |
| `KERNEL_RAM_AUX` | `rwx` | no | Secondary core RAM (multi-core only) |

Must also define these linker variables:

```ld
ULMK_MPU_ALIGN         = <value>;   /* MPU region boundary alignment */
ULMK_KERNEL_STACK_SIZE = <value>;   /* kernel supervisor stack, bytes */
ULMK_ISR_STACK_SIZE    = <value>;   /* ISR stack (ISP), bytes */
ULMK_CSA_POOL_SIZE     = <value>;   /* CSA pool size, bytes (only if HAVE_CSA) */
```

And optionally activate arch fragments via feature flags:

```ld
HAVE_CSA        = 1;   /* arch: include arch/tricore/linker/csa_pool.ld.in    */
HAVE_SMALL_DATA = 1;   /* arch: include arch/tricore/linker/small_data.ld.in  */
HAVE_BMHD       = 1;   /* chip: include <chip_dir>/bmhd.ld.in                 */
```

**Optional file: `bmhd.ld.in`**

If present and `HAVE_BMHD = 1`, this file is inserted first in `SECTIONS {}`. It
defines a chip-specific boot header section (e.g. TriCore BMHD placed in
`KERNEL_FLASH_NC`). Its format is chip-family specific — TC2xx and TC3xx have
different BMHD layouts — so it belongs in the chip input, not the arch layer.

---

### 3.2 Arch layer contract (Layer 2 — in repo)

Every arch port provides fragments in `arch/<ARCH>/linker/`:

**`prologue.ld.in`** — mandatory. Defines the ELF output metadata:

```ld
OUTPUT_FORMAT("elf32-tricore")
OUTPUT_ARCH(tricore)
ENTRY(_start)
```

This is the only place `OUTPUT_FORMAT` and `OUTPUT_ARCH` appear. The chip input
must not redefine them.

**`csa_pool.ld.in`** — included by the generator when `HAVE_CSA = 1` in the chip
`memory.ld`. Defines the CSA pool section using `ULMK_CSA_POOL_SIZE` (see §4.5).

**`small_data.ld.in`** — included by the generator when `HAVE_SMALL_DATA = 1`.
Defines `.sdata`/`.sbss` and exports the four GP anchor symbols (see §4.6).

The arch layer must **not** contain any concrete memory addresses. It may reference
`KERNEL_RAM` and other abstract region names from the chip `memory.ld`.

---

## 4. Kernel Fragment Catalog

### 4.1 vectors

**File:** `linker/kernel/vectors.ld.in`

Reserves flash space for the CPU trap/exception vector table and the interrupt vector
table. The kernel writes the addresses of `BTV` and `BIV` into hardware registers
during `ulmk_arch_init()` — the linker symbols tell it where those tables are.

```ld
/* --- Arch boot header (only if HAVE_BMHD) --- */

.bmhd : ALIGN(256) {
    KEEP(*(.bmhd))
} > KERNEL_FLASH_NC

/* --- Trap vector table --- */

.trap_table : ALIGN(256) {
    _ulmk_trap_table = .;
    KEEP(*(.trap_table))
} > KERNEL_FLASH

/* --- Interrupt vector table --- */

.int_table : ALIGN(256) {
    _ulmk_int_table = .;
    KEEP(*(.int_table))
} > KERNEL_FLASH
```

**Exported symbols:**

| Symbol | Type | Description |
|--------|------|-------------|
| `_ulmk_trap_table` | address | Base address of the trap vector table (→ BTV register) |
| `_ulmk_int_table`  | address | Base address of the interrupt vector table (→ BIV register) |

**Notes:**

- `.bmhd` is placed in `KERNEL_FLASH_NC` because most TriCore variants require the boot
  header at the non-cached alias. If `HAVE_BMHD` is not defined, this sub-section is
  omitted and `KERNEL_FLASH_NC` may be left undeclared.
- Both vector tables require 256-byte alignment so the hardware-encoded base address
  fits in the register's address field without bit manipulation.
- The `.trap_table` and `.int_table` input sections are provided by the arch port
  (`arch/tc27x/vectors.S`). They must be marked `KEEP` to survive `--gc-sections`.

---

### 4.2 kernel_text

**File:** `linker/kernel/kernel_text.ld.in`

Kernel code and read-only data. Not individually MPU-aligned because the kernel runs
in supervisor mode; no MPU region is needed to allow the kernel to access its own code.

```ld
.kernel_text : ALIGN(32) {
    _ulmk_kernel_text_start = .;
    *(.text.ulmk_arch_*)
    *(.text.ulmk_kernel_*)
    *(.text.ulmk_sched_*)
    *(.text.ulmk_ipc_*)
    *(.text.ulmk_irq_*)
    *(.text.ulmk_mem_*)
    *(.text.ulmk_notif_*)
    *(.rodata .rodata.*)
    _ulmk_kernel_text_end = .;
} > KERNEL_FLASH
```

**Exported symbols:**

| Symbol | Description |
|--------|-------------|
| `_ulmk_kernel_text_start` | Start of kernel executable code |
| `_ulmk_kernel_text_end`   | End of kernel executable code |

**Notes:**

- The section input patterns match the function name prefixes enforced by the
  arch and kernel coding conventions (all internal functions use `ulmk_arch_*`,
  `ulmk_kernel_*`, etc.).
- Rodata is merged here rather than in a separate section; this simplifies the
  MPU config for supervisor mode (one range covers code + constants).
- The `ALIGN(32)` satisfies TriCore's 32-byte instruction fetch granularity.

---

### 4.3 kernel_data

**File:** `linker/kernel/kernel_data.ld.in`

Kernel mutable state. Supervisor-only; no MPU region is exposed to userspace.

```ld
.kernel_data : ALIGN(ULMK_MPU_ALIGN) {
    _ulmk_kernel_data_start = .;
    *(.data.ulmk_arch_*)
    *(.data.ulmk_kernel_*)
    *(.data.ulmk_sched_*)
    *(.data.ulmk_ipc_*)
    *(.data.ulmk_irq_*)
    *(.data.ulmk_mem_*)
    *(.data.ulmk_notif_*)
    *(.bss.ulmk_arch_*)
    *(.bss.ulmk_kernel_*)
    *(.bss.ulmk_sched_*)
    *(.bss.ulmk_ipc_*)
    *(.bss.ulmk_irq_*)
    *(.bss.ulmk_mem_*)
    *(.bss.ulmk_notif_*)
    COMMON
    . = ALIGN(ULMK_MPU_ALIGN);
    _ulmk_kernel_data_end = .;
} > KERNEL_RAM
```

**Exported symbols:**

| Symbol | Description |
|--------|-------------|
| `_ulmk_kernel_data_start` | Start of kernel data/bss region |
| `_ulmk_kernel_data_end`   | End of kernel data/bss region |

**Notes:**

- `startup.S` uses `_ulmk_kernel_data_start` / `_ulmk_kernel_data_end` to zero the bss
  and copy initialised data from flash LMA to RAM VMA.
- Aligned to `ULMK_MPU_ALIGN` on both ends so the arch can optionally configure
  a single read-only MPU range that locks kernel RAM from userspace.

---

### 4.4 kernel_stacks

**File:** `linker/kernel/kernel_stacks.ld.in`

Two stacks for kernel privilege modes. NOLOAD: they are not initialised at boot.

```ld
.kernel_stack (NOLOAD) : ALIGN(8) {
    _ulmk_kernel_stack_bottom = .;
    . += ULMK_KERNEL_STACK_SIZE;
    _ulmk_kernel_stack_top = .;
} > KERNEL_RAM

.isr_stack (NOLOAD) : ALIGN(8) {
    _ulmk_isr_stack_bottom = .;
    . += ULMK_ISR_STACK_SIZE;
    _ulmk_isr_stack_top = .;
} > KERNEL_RAM
```

`ULMK_KERNEL_STACK_SIZE` and `ULMK_ISR_STACK_SIZE` must be defined by the arch `memory.ld`
or by the top-level CMakeLists before the linker is assembled. Defaults:

```
ULMK_KERNEL_STACK_SIZE = 4096;
ULMK_ISR_STACK_SIZE    = 2048;
```

**Exported symbols:**

| Symbol | Description |
|--------|-------------|
| `_ulmk_kernel_stack_bottom` | Bottom of the kernel (supervisor) stack |
| `_ulmk_kernel_stack_top`    | Top of the kernel stack; initial SP |
| `_ulmk_isr_stack_bottom`    | Bottom of the ISR stack (TriCore ISP) |
| `_ulmk_isr_stack_top`       | Top of the ISR stack; initial ISP |

**Notes:**

- `startup.S` loads `_ulmk_kernel_stack_top` into the stack pointer (SP / A10)
  and `_ulmk_isr_stack_top` into the interrupt stack pointer (ISP / A11) before
  calling any C code.
- Keeping them separate allows stack-overrun detection: if the ISR stack
  overflows into the kernel stack, the kernel stack sentinel catches it.

---

### 4.5 csa_pool (arch-optional)

**File:** `arch/tricore/linker/csa_pool.ld.in`

**Included only when `HAVE_CSA = 1` is set in the chip `memory.ld`.**

TriCore hardware requires a pool of Context Save Areas (CSAs) before the first
`CALL` or interrupt. Each CSA is exactly 64 bytes. The pool must be contiguous
and 64-byte aligned.

```ld
.csa_pool (NOLOAD) : ALIGN(64) {
    _ulmk_csa_pool_start = .;
    . += ULMK_CSA_POOL_SIZE;
    _ulmk_csa_pool_end = .;
} > KERNEL_RAM
```

Default:

```
ULMK_CSA_POOL_SIZE = 16384;   /* 256 CSAs × 64 bytes */
```

**Exported symbols:**

| Symbol | Description |
|--------|-------------|
| `_ulmk_csa_pool_start` | First CSA frame address (written into FCX by startup.S) |
| `_ulmk_csa_pool_end`   | One-past-last CSA frame (used to compute LCX) |

**Notes:**

- 256 CSAs supports 256 simultaneous nested call/interrupt contexts. Each thread
  needs at minimum 2 CSAs (one for its own frame, one for the CALL into the kernel
  during a syscall). A system with 32 threads should have at least 64 CSAs; 256 is
  safe for nearly all embedded workloads.
- `startup.S` must initialise the CSA free list (FCX/LCX/PCX chain) **before**
  enabling interrupts or calling any C function. The linker symbols provide the
  bounds.
- `_ulmk_csa_pool_start` must lie in a physical SRAM segment whose segment number
  can be encoded in the 4-bit PCXI.PCXS field. DSPR of Core 0 (`0x70000000`) is
  always segment 7, which is valid.

---

### 4.6 small_data (arch-optional)

**File:** `arch/tricore/linker/small_data.ld.in`

**Included only when `HAVE_SMALL_DATA = 1` is set in the chip `memory.ld`.**

TriCore ABI reserves four global address registers (A0, A1, A8, A9) as base
pointers for small-data areas. Accesses to variables in these areas use short
base+offset encodings. `startup.S` must load the four anchor symbols into those
registers before any C code runs.

```ld
.sdata  : { *(.sdata  .sdata.*)  } > KERNEL_RAM
.sbss   : { *(.sbss   .sbss.*)   } > KERNEL_RAM
.sdata2 : { *(.sdata2 .sdata2.*) } > KERNEL_RAM
.sbss2  : { *(.sbss2  .sbss2.*)  } > KERNEL_RAM

_small_data_  = ADDR(.sdata)  + 0x8000;
_small_data2_ = ADDR(.sdata2) + 0x8000;
_small_data3_ = ADDR(.sdata)  + 0x8000;   /* alias; A8 mirrors A0 range */
_small_data4_ = ADDR(.sdata2) + 0x8000;   /* alias; A9 mirrors A1 range */
```

**Exported symbols:**

| Symbol | Loaded into | Description |
|--------|-------------|-------------|
| `_small_data_`  | A0 | Centre of the primary small-data area |
| `_small_data2_` | A1 | Centre of the secondary small-data area |
| `_small_data3_` | A8 | Alias (may differ on multi-SDA toolchains) |
| `_small_data4_` | A9 | Alias (may differ on multi-SDA toolchains) |

**Notes:**

- The `+0x8000` offset places the anchor in the middle of the 64 KiB window
  reachable by the signed 16-bit offset in TriCore `LD`/`ST` short encodings.
- If the toolchain generates `.sdata`/`.sbss` but the arch does not define
  `HAVE_SMALL_DATA`, the linker will fail with "no rule for .sdata". The arch
  must always include this fragment when targeting TriCore.
- Variables are placed here automatically by the compiler when they are smaller
  than a threshold (typically 8 bytes) and when `-G <n>` is passed to the compiler.
  This is transparent to user code.

---

### 4.7 domain_table

**File:** `linker/kernel/domain_table.ld.in`

Collects all `ULMK_DEFINE_DOMAIN(name, perms)` descriptors into a contiguous read-only
table in flash. The kernel iterates this table at boot to register every declared
memory domain.

```ld
.domain_table : ALIGN(4) {
    _ulmk_domain_table_start = .;
    KEEP(*(.domain_table))
    _ulmk_domain_table_end = .;
} > KERNEL_FLASH
```

**Exported symbols:**

| Symbol | Description |
|--------|-------------|
| `_ulmk_domain_table_start` | Start of the domain descriptor array |
| `_ulmk_domain_table_end`   | One-past-end of the domain descriptor array |

**Notes:**

- `KEEP` is mandatory: descriptors have no direct references from other code and
  would be eliminated by `--gc-sections` otherwise.
- The kernel computes the number of registered domains at boot as
  `(_ulmk_domain_table_end - _ulmk_domain_table_start) / sizeof(ulmk_domain_desc_t)`.
- This table is strictly read-only after boot. Writing to it from userspace is
  prevented by the MPU configuration of `.kernel_text` (which covers flash).

---

### 4.8 user_pool

**File:** `linker/kernel/user_pool.ld.in`

Marks the remaining KERNEL_RAM after all static allocations as the physical
memory pool managed by the kernel's physical allocator.

```ld
.user_pool (NOLOAD) : ALIGN(ULMK_MPU_ALIGN) {
    _ulmk_user_pool_start = .;
    . = ORIGIN(KERNEL_RAM) + LENGTH(KERNEL_RAM);
    _ulmk_user_pool_end = .;
} > KERNEL_RAM
```

**Exported symbols:**

| Symbol | Description |
|--------|-------------|
| `_ulmk_user_pool_start` | First byte of the physical allocator pool |
| `_ulmk_user_pool_end`   | One-past-end of the physical allocator pool |

**Notes:**

- The physical allocator (`ulmk_arch_phys_alloc`) uses these symbols as its entire
  managed range. It does not know about any other memory region.
- Thread stacks, `ulmk_mem_map(ANON)` buffers, and IPC endpoint structures are all
  carved from this pool.
- Dynamic domain data sections (`.domain_NAME`) placed in `KERNEL_RAM` by app
  snippets are laid out **before** `.user_pool`, so the pool naturally starts
  after all statically declared domains.
- For multi-core chips (e.g., TC27x), each core's DSPR can have its own
  `user_pool` section. The arch decides which DSPR backs `KERNEL_RAM`.

---

## 5. App and Domain Snippet Templates

These are parameterised templates. The build system (`generate_ld.py`) renders
one instance per registered app or domain and concatenates them between the kernel
fragments and `user_pool`.

### 5.1 app_code snippet

**Template:** `linker/snippets/app_code.ld.in`

One instance per app registered with `ulmk_add_app()` in CMake.

```ld
/* Generated for app: @APP_NAME@ */
.app_@APP_NAME@_text : ALIGN(ULMK_MPU_ALIGN) {
    _ulmk_app_@APP_NAME@_text_start = .;
    *(.text.@APP_NAME@.*)
    *(.rodata.@APP_NAME@.*)
    . = ALIGN(ULMK_MPU_ALIGN);
    _ulmk_app_@APP_NAME@_text_end = .;
} > KERNEL_FLASH
```

Parameters: `@APP_NAME@` — replaced by the app name string.

**Exported symbols (per app):**

| Symbol | Description |
|--------|-------------|
| `_ulmk_app_NAME_text_start` | Start of the app's executable code region |
| `_ulmk_app_NAME_text_end`   | End of the app's executable code region |

**Notes:**

- The input section patterns (`.text.NAME.*`, `.rodata.NAME.*`) are produced when
  the compiler flag `-ffunction-sections -fdata-sections` is combined with the
  `UL_APP_SECTION(name)` macro (see §8). Each TU in the app is compiled with
  `-DULMK_APP_NAME=asclin_driver`, and the macro redirects `.text` into
  `.text.asclin_driver.<function>`.
- The app's code section boundaries are registered with the kernel's MPU manager
  at boot by scanning `_ulmk_domain_table_*` and matching names.

---

### 5.2 domain_data snippet

**Template:** `linker/snippets/domain_data.ld.in`

One instance per domain registered with `ulmk_add_domain()` in CMake.

```ld
/* Generated for domain: @DOMAIN_NAME@ */
.domain_@DOMAIN_NAME@ (NOLOAD) : ALIGN(ULMK_MPU_ALIGN) {
    _ulmk_domain_@DOMAIN_NAME@_start = .;
    *(.domain_@DOMAIN_NAME@.data .domain_@DOMAIN_NAME@.data.*)
    *(.domain_@DOMAIN_NAME@.bss  .domain_@DOMAIN_NAME@.bss.*)
    . = ALIGN(ULMK_MPU_ALIGN);
    _ulmk_domain_@DOMAIN_NAME@_end = .;
} > KERNEL_RAM
```

For domains that need to be visible from multiple cores, the target region can be
overridden to `SHARED_RAM`:

```ld
/* Generated for shared domain: @DOMAIN_NAME@ */
.domain_@DOMAIN_NAME@ (NOLOAD) : ALIGN(ULMK_MPU_ALIGN) {
    _ulmk_domain_@DOMAIN_NAME@_start = .;
    *(.domain_@DOMAIN_NAME@.data .domain_@DOMAIN_NAME@.data.*)
    *(.domain_@DOMAIN_NAME@.bss  .domain_@DOMAIN_NAME@.bss.*)
    . = ALIGN(ULMK_MPU_ALIGN);
    _ulmk_domain_@DOMAIN_NAME@_end = .;
} > SHARED_RAM
```

Parameters: `@DOMAIN_NAME@` — replaced by the domain name string.

**Exported symbols (per domain):**

| Symbol | Description |
|--------|-------------|
| `_ulmk_domain_NAME_start` | Start of the domain's data region (base for MPU range) |
| `_ulmk_domain_NAME_end`   | End of the domain's data region (limit for MPU range) |

---

## 6. Exported Symbol Table

Complete list of linker symbols consumed by C code in the kernel and arch port:

| Symbol | Consumer | Description |
|--------|----------|-------------|
| `_ulmk_trap_table` | `ulmk_arch_init()` | Written into BTV / equivalent trap vector register |
| `_ulmk_int_table` | `ulmk_arch_init()` | Written into BIV / equivalent interrupt base register |
| `_ulmk_kernel_text_start` | MPU setup | Code range for supervisor CPR |
| `_ulmk_kernel_text_end` | MPU setup | Code range for supervisor CPR |
| `_ulmk_kernel_data_start` | `startup.S`, MPU setup | Data/bss init range; supervisor DPR |
| `_ulmk_kernel_data_end` | `startup.S`, MPU setup | Data/bss init range; supervisor DPR |
| `_ulmk_kernel_stack_top` | `startup.S` | Initial SP value |
| `_ulmk_kernel_stack_bottom` | Stack sentinel | Underflow detection |
| `_ulmk_isr_stack_top` | `startup.S` | Initial ISP value (TriCore A11) |
| `_ulmk_isr_stack_bottom` | Stack sentinel | Underflow detection |
| `_ulmk_csa_pool_start` | `startup.S` | FCX initialisation; passed in `ulmk_boot_info_t` |
| `_ulmk_csa_pool_end` | `startup.S` | LCX upper bound |
| `_small_data_` | `startup.S` | Loaded into A0 |
| `_small_data2_` | `startup.S` | Loaded into A1 |
| `_small_data3_` | `startup.S` | Loaded into A8 |
| `_small_data4_` | `startup.S` | Loaded into A9 |
| `_ulmk_domain_table_start` | `ulmk_kern_main()` | Start of domain descriptor scan |
| `_ulmk_domain_table_end` | `ulmk_kern_main()` | End of domain descriptor scan |
| `_ulmk_user_pool_start` | `ulmk_arch_phys_alloc_init()` | Physical allocator base |
| `_ulmk_user_pool_end` | `ulmk_arch_phys_alloc_init()` | Physical allocator limit |
| `_ulmk_app_NAME_text_start` | MPU setup per thread | Per-app code range for CPR |
| `_ulmk_app_NAME_text_end` | MPU setup per thread | Per-app code range for CPR |
| `_ulmk_domain_NAME_start` | MPU setup per thread | Per-domain data range for DPR |
| `_ulmk_domain_NAME_end` | MPU setup per thread | Per-domain data range for DPR |

---

## 7. Assembly Model

### 7.1 generate_ld.py invocation

```
python3 cmake/generate_ld.py
    --chip-dir  <path>                # Layer 3: chip dir (memory.ld + optional bmhd.ld.in)
    --arch-dir  arch/tricore/linker/  # Layer 2: arch fragments (prologue, csa_pool, small_data)
    --kernel-dir linker/kernel/       # Layer 1: kernel-common fragments
    --snippets  linker/snippets/      # app_code.ld.in + domain_data.ld.in templates
    --app-list  ${ULMK_APP_LIST}        # space-separated list of registered app names
    --dom-list  ${ULMK_DOMAIN_LIST}     # space-separated list of registered domain names
    --output    ${CMAKE_BINARY_DIR}/generated.ld
```

The generator reads `HAVE_*` flags directly from the chip `memory.ld` using a
lightweight parser (no linker needed at configure time). It uses these flags to
decide which arch fragments and which chip fragments to include.

### 7.2 CMake API

Drivers and applications register themselves via two CMake functions:

```cmake
# Declare an application (driver or userspace task)
ulmk_add_app(
    NAME    asclin_driver      # unique name; becomes section prefix
    SOURCES src/asclin.c
    DOMAIN  asclin             # associated data domain (optional)
    STACK   2048               # in bytes; allocated from user_pool at boot
    PRIV    DRIVER             # ULMK_PRIV_DRIVER or ULMK_PRIV_USER
)

# Declare a memory domain
ulmk_add_domain(
    NAME   asclin
    PERMS  "ULMK_PERM_READ | ULMK_PERM_WRITE | ULMK_PERM_USER"
    SHARED false               # true → placed in SHARED_RAM
)
```

`ulmk_add_app` and `ulmk_add_domain` are defined in `cmake/linker_api.cmake`. They
append to internal CMake lists (`ULMK_APP_LIST`, `ULMK_DOMAIN_LIST`) and set associated
properties. They do not directly generate output.

### 7.3 Section ordering rationale

```
KERNEL_FLASH layout (low → high address):
  [BMHD]           ← must be at flash origin on some archs
  [trap_table]     ← hardware register points here; ALIGN(256)
  [int_table]      ← hardware register points here; ALIGN(256)
  [kernel_text]    ← kernel code; no MPU alignment needed (supervisor)
  [app_NAME_text]  ← one per app; each ALIGN(ULMK_MPU_ALIGN) front and back
  [domain_table]   ← read-only; no MPU alignment needed

KERNEL_RAM layout (low → high address):
  [kernel_data]    ← supervisor only; ALIGN(ULMK_MPU_ALIGN) both ends
  [kernel_stack]   ← grows downward; ALIGN(8)
  [isr_stack]      ← grows downward; ALIGN(8)
  [csa_pool]       ← NOLOAD; ALIGN(64) — arch-optional
  [domain_NAME]    ← one per domain; each ALIGN(ULMK_MPU_ALIGN) front and back
  [small_data]     ← sdata/sbss; ALIGN(4) — arch-optional
  [user_pool]      ← remainder of RAM; expands to end of KERNEL_RAM

SHARED_RAM layout:
  [domain_NAME]    ← only domains declared SHARED
```

---

## 8. C Macro API — linker.h

**Header:** `include/ulmk/linker.h`

Used by drivers and apps to place variables into the correct linker sections and to
register domain descriptors.

```c
/*
 * Place a zero-initialised variable in the named domain's bss section.
 * The linker collects all ULMK_DOMAIN_BSS(foo) vars into .domain_foo.bss.
 */
#define ULMK_DOMAIN_BSS(name) \
	__attribute__((section(".domain_" #name ".bss")))

/*
 * Place an initialised variable in the named domain's data section.
 * The linker collects all ULMK_DOMAIN_DATA(foo) vars into .domain_foo.data.
 */
#define ULMK_DOMAIN_DATA(name) \
	__attribute__((section(".domain_" #name ".data")))

/*
 * Shorthand: place in the current module's domain.
 * Requires #define ULMK_MODULE_NAME <name> before including this header.
 */
#define ULMK_PRIVATE      ULMK_DOMAIN_BSS(ULMK_MODULE_NAME)
#define ULMK_PRIVATE_INIT ULMK_DOMAIN_DATA(ULMK_MODULE_NAME)

/*
 * Register a domain descriptor in .domain_table.
 * The kernel reads this table at boot and configures MPU ranges.
 *
 * perms: bitwise OR of ULMK_PERM_READ, ULMK_PERM_WRITE, ULMK_PERM_EXEC, ULMK_PERM_USER
 */
#define ULMK_DEFINE_DOMAIN(dname, perms)                                  \
	extern uint8_t _ulmk_domain_##dname##_start[];                    \
	extern uint8_t _ulmk_domain_##dname##_end[];                      \
	static const ulmk_domain_desc_t __ulmk_domain_desc_##dname          \
		__attribute__((section(".domain_table"), used)) = {     \
		.name  = #dname,                                        \
		.start = (uintptr_t)_ulmk_domain_##dname##_start,        \
		.end   = (uintptr_t)_ulmk_domain_##dname##_end,          \
		.perms = (perms),                                       \
	}

/*
 * Place a function in the app-specific code section so the linker script's
 * app_code snippet can isolate it.
 * Used internally by ulmk_add_app() CMake function via -DULMK_APP_NAME=<name>.
 */
#ifdef ULMK_APP_NAME
#define UL_APP_TEXT \
	__attribute__((section(".text." UL_STR(ULMK_APP_NAME) "." __func__)))
#define UL_APP_RODATA \
	__attribute__((section(".rodata." UL_STR(ULMK_APP_NAME))))
#else
#define UL_APP_TEXT
#define UL_APP_RODATA
#endif

#define UL_STR_(x) #x
#define UL_STR(x)  UL_STR_(x)
```

### 8.1 Driver usage example

```c
/* drivers/asclin/asclin.c */

#define ULMK_MODULE_NAME asclin
#include <ulmk/linker.h>
#include <ulmk/linker_defs.h>   /* ulmk_domain_desc_t, UL_PERM_* */

ULMK_PRIVATE static uint8_t  rx_buf[512];
ULMK_PRIVATE static uint16_t rx_head;
ULMK_PRIVATE static uint16_t rx_tail;
ULMK_PRIVATE_INIT static uint32_t baud_rate = 115200;

ULMK_DEFINE_DOMAIN(asclin, ULMK_PERM_READ | ULMK_PERM_WRITE | ULMK_PERM_USER);

#undef ULMK_MODULE_NAME
```

---

## 9. TC27x Chip Input Example

The TC27x chip directory (`--chip-dir boards/tc27x/` or passed from the board CMake):

**`boards/tc27x/memory.ld`** — chip-specific MEMORY block:

```ld
/* TC27x memory map — Core 0 centric                          */
/* No OUTPUT_FORMAT/ARCH/ENTRY here; those are in arch layer. */

ULMK_MPU_ALIGN         = 64;
ULMK_KERNEL_STACK_SIZE = 4096;
ULMK_ISR_STACK_SIZE    = 2048;
ULMK_CSA_POOL_SIZE     = 16384;   /* 256 CSAs × 64 bytes */

HAVE_CSA        = 1;
HAVE_SMALL_DATA = 1;
HAVE_BMHD       = 1;

MEMORY
{
    KERNEL_FLASH_NC (rx)  : ORIGIN = 0x80000000, LENGTH = 4M
    KERNEL_FLASH    (rx)  : ORIGIN = 0xA0000000, LENGTH = 4M
    KERNEL_RAM      (rwx) : ORIGIN = 0x70000000, LENGTH = 240K
    SHARED_RAM      (rwx) : ORIGIN = 0x90000000, LENGTH = 32K
    PERIPH          (rw)  : ORIGIN = 0xF0000000, LENGTH = 256M
}
```

**`boards/tc27x/bmhd.ld.in`** — TC27x-specific boot header section:

```ld
/*
 * Boot Mode Header — TC2xx specific layout.
 * Must be at the start of KERNEL_FLASH_NC (0x80000000).
 * TC3xx has a different BMHD structure; that belongs in boards/tc3xx/.
 */
.bmhd : ALIGN(256) {
    KEEP(*(.bmhd))
} > KERNEL_FLASH_NC
```

**`arch/tricore/linker/prologue.ld.in`** — arch layer (in repo):

```ld
OUTPUT_FORMAT("elf32-tricore")
OUTPUT_ARCH(tricore)
ENTRY(_start)
```

---

## 10. QEMU Chip Input Example

**`boards/qemu_tc27x/memory.ld`** — simplified layout for the Linumiz QEMU-AURIX target:

```ld
ULMK_MPU_ALIGN         = 64;
ULMK_KERNEL_STACK_SIZE = 4096;
ULMK_ISR_STACK_SIZE    = 2048;
ULMK_CSA_POOL_SIZE     = 16384;

HAVE_CSA        = 1;
HAVE_SMALL_DATA = 1;
/* HAVE_BMHD deliberately absent: QEMU does not validate the boot header */

MEMORY
{
    KERNEL_FLASH (rx)  : ORIGIN = 0xA0000000, LENGTH = 4M
    KERNEL_RAM   (rwx) : ORIGIN = 0x70000000, LENGTH = 240K
    SHARED_RAM   (rwx) : ORIGIN = 0x90000000, LENGTH = 32K
    PERIPH       (rw)  : ORIGIN = 0xF0000000, LENGTH = 256M
}
```

There is no `bmhd.ld.in` in this chip directory. The generator detects the absence of
`HAVE_BMHD` and skips the BMHD step; `KERNEL_FLASH_NC` is also absent, so no linker
error occurs.

---

## 11. Alignment Constants

| Constant | Value (TC27x) | Reason |
|----------|---------------|--------|
| `ULMK_MPU_ALIGN` | 64 | CSA frame boundary; TriCore DPR minimum is 8 bytes but 64 is natural |
| Trap vector alignment | 256 | 8 trap classes × 32 bytes each; must fit in BTV address field |
| Interrupt vector alignment | 256 | BIV address field; each entry is 32 bytes |
| CSA frame size | 64 | Fixed by TriCore architecture; not configurable |
| Kernel text alignment | 32 | TriCore fetch unit granularity |
| Stack alignment | 8 | TriCore ABI requires 8-byte aligned SP on function entry |

---

*This document defines the linker contract. Implementation is in `linker/`, `cmake/`,
and `include/ulmk/linker.h`. Arch inputs live in `arch/<ARCH>/memory.ld`.*
