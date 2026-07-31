# ESP32-P4 external board (esp32p4_ev_function)

Board tree: sibling `ulmk_boards/esp32p4_ev_function/` (not an in-tree QEMU
board). Authoritative BSP notes live in that tree's `README.md`.

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

## Linker overrides (chip Layer 3)

`generate_ld.py` accepts optional chip fragments:

- `vectors.ld.in` / `kernel_text.ld.in` → place startup, trap and kernel text
  in `KERNEL_IRAM` (bootloader loads the SRAM segment before entry)
- `kernel_data.ld.in` → flash LMA alignment for `.data`
- `memory.ld` → splits `KERNEL_IRAM` / `KERNEL_RAM` around the rev1 ROM hole

## Arch knobs (board_config.h)

- `ULMK_ARCH_HAVE_CLIC=1`, `ULMK_ARCH_CLIC_VECTORED=1`
- `ULMK_ARCH_HAVE_CLINT=0`, `ULMK_ARCH_HAVE_PLIC=0`
- `ULMK_ARCH_PMP_NUM=16` with `ULMK_ARCH_PMP_PRESERVE_BOOT=1`
- Tick: SYSTIMER → INTMTX → CLIC IRQ 16

See the board `README.md` for demos, PSRAM timing tune and HIL commands.
