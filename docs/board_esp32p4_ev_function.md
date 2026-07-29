# ESP32-P4 external board (esp32p4_ev_function)

Board tree: sibling `ulmk_boards/esp32p4_ev_function/` (not in-tree QEMU).

## Boot offsets

| Offset | Image |
|--------|--------|
| `0x2000` | IDF 2nd-stage bootloader (`scripts/prebuilt/bootloader.bin`) |
| `0x8000` | Partition table |
| `0x10000` | ulmk app (`esptool elf2image --chip esp32p4`) |

## Environment

- `ESP_IDF_PATH` / `IDF_PATH` → ESP-IDF with `esp32p4` SOC headers
- Toolchain: `riscv32-esp-elf` (Espressif GCC 14.x preferred)
- Host build: `UL_BOARD_HOST_BUILD=1` in `board.cmake` (not QEMU container)
- Serial HIL: typically `/dev/ttyUSB1` @ 115200

## Arch knobs (board_config.h)

- `ULMK_ARCH_HAVE_CLIC=1`, `ULMK_ARCH_CLIC_VECTORED=1`
- `ULMK_ARCH_HAVE_CLINT=0`, `ULMK_ARCH_HAVE_PLIC=0`
- `ULMK_ARCH_PMP_NUM=16` with `ULMK_ARCH_PMP_PRESERVE_BOOT=1` — keep
  bootloader locked early slots; board overlays LP / PSRAM / HP peri on
  high slots via `board_pmp.c` (`ulmk_board_pmp_extra`)
- Negative smoke: component `board_pmp_neg` → `PMP_NEG: PASS`
- Tick: SYSTIMER → INTMTX → CLIC IRQ 16

See board `README.md` for demos and HIL commands.
