# RISC-V RV32 port — ulmk

Target: **rv32imac** on QEMU `virt` (`boards/qemu_riscv_virt`).

## Layout

| Layer | Path |
|-------|------|
| Arch | `arch/riscv/` — startup, trap, ctx_switch, PMP / Sv32, IRQ backends |
| Board | `boards/qemu_riscv_virt/` — UART 16550, CLINT `mtime` timestamps, test finisher |
| Toolchain | xpack `riscv-none-elf-gcc` (see `tools/docker/Dockerfile`) |

## Privilege and isolation

- Kernel runs in **M-mode**; user/driver threads enter **U-mode** via `mret` from `_ulmk_thread_trampoline`.
- Memory isolation sits behind the frozen `ulmk_arch_mpu_*` API.  Two
  backends, chosen at configure time by `ULMK_CONFIG_MMU`
  (`cmake/arch_sources.cmake`):

| `ULMK_CONFIG_MMU` | File | Hardware | Boards |
|---|---|---|---|
| `0` (default) | `mpu_pmp.c` | PMP entries | every RISC-V board |
| `1` | `mmu_sv32.c` | Sv32 page tables (`satp`) | boards with `ULMK_BOARD_HAVE_SV32` |

Both are loaded lazily: a switch writes the static user layout plus the
thread's pinned stack, and every other area is loaded one window at a time
by `ulmk_kern_mem_fault()` on the first access fault.  The number of areas
a thread holds is therefore not bounded by the hardware.

### PMP (`mpu_pmp.c`)

M-mode ignores unlocked PMP entries, so the kernel needs no entry of its own:
every entry describes what the current U-mode thread may touch.

| Slot | Macro | Contents |
|---|---|---|
| 2 | `ULMK_ARCH_PMP_UTEXT` | user text, RX, NAPOT |
| 3 (+2 with `ULMK_ARCH_PMP_URAM_TOR`) | `ULMK_ARCH_PMP_URAM` | user `.data`/`.bss` up to `_ulmk_user_pool_start`, RW |
| 4 | `ULMK_ARCH_PMP_MMIO` | PERIPH window, RW, NAPOT |
| remaining | — | pinned stack first, then the dynamic ring (round-robin eviction) |
| `ULMK_ARCH_PMP_TEMP0/1` | — | short-lived kernel mappings (`ulmk_arch_pmp_map_temp`) |

Slots 0/1 are free unless a board reserves them through
`ULMK_ARCH_PMP_RESERVED_MASK` / `ULMK_CONFIG_BOARD_PMP_EXTRA`.  Windows are
NAPOT, so `ULMK_ARCH_MPU_WIN_POW2=1` and an access fault is mcause 1/5/7.

### Sv32 (`mmu_sv32.c`) — protection only

Identity mapping: the virtual address is the physical address, there is no
translation.  `satp` translates only S/U-mode, so the M-mode kernel stays
untranslated and `medeleg=0` routes page faults (mcause 12/13/15, address in
`mtval`) to the same M-mode handler as PMP faults.

- **Per hart:** one root table (1024 PTEs) and `ULMK_ARCH_SV32_L2_NUM` L2
  tables, all static and 4 KiB aligned.  Harts never share a table, so a
  flush is local; the SMP shootdown reuses the PMP path
  (`mem_settle` → `xcall` → `ulmk_arch_mpu_flush`).
- **Static map, built once per hart:** user text RX, user RAM
  (`[_ulmk_user_ram_start, _ulmk_user_pool_start)`) RW, PERIPH RW as 4 MiB
  megapages where aligned.
- **Switch:** maps the pinned stack; a different thread also drops the
  dynamic pages.
- **Fault:** loads one 4 KiB page into a ring of `ULMK_ARCH_SV32_DYN_MAX`
  entries, evicting round-robin with a single `sfence.vma`.  L2 tables used
  only by dynamic pages are released on flush.
- **Leaf PTEs** always carry U|A|D (no A/D fault traffic).  W alone is a
  reserved encoding, so a write-only area maps as R|W.
- **PMP** keeps a single catch-all entry (NAPOT, RWX): with any PMP entry
  implemented, U-mode accesses that match none are denied, page tables or
  not.  `ULMK_CONFIG_BOARD_PMP_EXTRA` is rejected with the MMU.
- `satp` is written and read back at init; a mismatch (no Sv32 on the hart)
  is a panic rather than silent loss of protection.

Page granularity sets `ULMK_ARCH_REGION_ALIGN = 4096` and
`ULMK_ARCH_MPU_WIN_MIN = WIN_MAX = 4096`: ANON maps and stacks are carved on
page boundaries (`ulmk_heap_aligned_alloc`), so each costs at least 4 KiB of
heap, and the linker aligns the user sections to a page
(`generate_ld.py --page-align`).

```bash
python3 tools/dev.py build --board boards/qemu_riscv_virt --enable-mmu
python3 tools/dev.py tests e2e --board boards/qemu_riscv_virt --enable-mmu
```

## Interrupt controllers

Backends are separate compilation units, selected in `board_config.h`:

| File | `ULMK_ARCH_HAVE_*` | Role |
|------|-------------------|------|
| `irq_clint.c` | `ULMK_ARCH_HAVE_CLINT=1` | MSIP/MTIMER via `mie` (legacy) |
| `irq_clic.c` | `ULMK_ARCH_HAVE_CLIC=1` | Core-local MMIO interrupts |
| `irq_plic.c` | `ULMK_ARCH_HAVE_PLIC=1` | MEIP claim/complete, peripheral IRQs |
| `irq.c` | (glue) | `ulmk_arch_irq_src_*`, trap demux |

SoC base addresses live in `boards/<soc>/board_config.h` (`ULMK_BOARD_PLIC_BASE`,
timer RTC base, etc.).  `arch/riscv/arch_config.h` applies standard CLINT/PLIC
offsets only.

## Timer (kernel tick + board wrapper)

The kernel timing wheel is driven by **CLINT `mtimecmp`** on each hart
(`ulmk_arch_tick_init`).  `board_timer_sleep_us()` is a thin wrapper over
`ulmk_sleep_ms()`.  Peripheral IRQs (UART, etc.) still use PLIC +
`ulmk_notif_wait()` in driver servers.

Goldfish RTC is no longer used for sleep.

## Build

```bash
python3 tools/dev.py build --board boards/qemu_riscv_virt
python3 tools/dev.py build qemu --board boards/qemu_riscv_virt
```

## Tests

```bash
python3 tools/dev.py tests e2e --board boards/qemu_riscv_virt [--include-smp] [--enable-mmu]
python3 tools/dev.py tests silicon --board boards/qemu_riscv_virt [--enable-smp] [--enable-mmu]
```

`tests/sv32_unit` covers the page-table backend on the host (PTE encoding,
the dynamic ring, the L2 pool, per-hart tables) with `satp`/`sfence.vma`
mocked.

`tests silicon` builds each `ulmk_apps/silicon/silicon_*` case alone and runs
it on QEMU until its `SILICON_<NAME>: PASS` / `FAIL` line.  On QEMU:

- `silicon_device_manager` is skipped: virt has no device-manager adapters.
- `silicon_smp_smoke` needs `--enable-smp` (also `boards/qemu_riscv_virt_smp4`).
- `silicon_wcet` runs with `-icount shift=0` so `mcycle` counts
  instructions instead of host time, and only single-hart: under icount the
  counter is one clock shared by every hart, so another hart's instructions
  would land in the sample.

`ctx_early_tricore` is TriCore-only (hardware CSA pool). A RISC-V counterpart
(`ctx_early_riscv`) may be added later to exercise stack-frame init without CSA.

## QEMU map (virt)

| Device | Address |
|--------|---------|
| RAM/code | `0x80000000` |
| CLIC (M-mode) | `0x02000000` (boards with `ULMK_ARCH_HAVE_CLIC=1`; not on stock QEMU virt yet) |
| PLIC | `0x0C000000` |
| Goldfish RTC | `0x00101000` |
| UART0 | `0x10000000` |
| Test finisher | `0x00100000` |

## Future

- Board `sifive_u` as HiFive Unleashed proxy (phase 2).
- FPU: `-DULMK_ARCH_HAVE_FPU=1` + `-march=rv32imafc`.
