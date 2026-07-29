#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# tools/sdk_build.sh — build + assemble the distributable ULMK SDK.
#
# Single source of truth shared by:
#   - tools/dev.py  build --kernel --board <dir>
#   - tests/sdk_e2e (standalone SDK consumer integration test)
#
# Produces a self-contained tree under <out-dir>:
#   lib/ulmk_kernel_<arch>_<board>_gcc.a   kernel + arch (CPR0 / supervisor)
#   lib/ulmk_board_<arch>_<board>_gcc.a    board + startup + vectors + user_entry
#   linker/linker_<arch>_<board>_gcc.ld    processed linker script
#   include/ulmk/*.h                       public microkernel API
#   include/ulmk_syscall_abi.h             arch-provided ABI (redirected to by
#                                          <ulmk/syscall_abi.h>)
#   include/board/*.h                      public board API: chip headers plus
#                                          every drivers/*/include header
#                                          (no *internal* headers)
#
# Must run inside the dev container (needs cmake, ninja and the cross toolchains).

set -euo pipefail

usage() {
	echo "usage: $0 --toolchain FILE --chip-dir DIR --arch ARCH \\" >&2
	echo "          --board-name NAME --build-dir DIR --out-dir DIR \\" >&2
	echo "          [--clean] [--optimize-size] [--enable-smp] [--enable-irq-attach]" >&2
	exit 2
}

TOOLCHAIN=""
CHIP_DIR=""
ARCH=""
BOARD_NAME=""
BUILD_DIR=""
OUT_DIR=""
CLEAN=0
OPTIMIZE_SIZE=0
ENABLE_SMP=0
ENABLE_IRQ_ATTACH=0

while [ $# -gt 0 ]; do
	case "$1" in
	--toolchain)  TOOLCHAIN="$2"; shift 2;;
	--chip-dir)   CHIP_DIR="$2";  shift 2;;
	--arch)       ARCH="$2";      shift 2;;
	--board-name) BOARD_NAME="$2"; shift 2;;
	--build-dir)  BUILD_DIR="$2"; shift 2;;
	--out-dir)    OUT_DIR="$2";   shift 2;;
	--clean)      CLEAN=1;        shift;;
	--optimize-size) OPTIMIZE_SIZE=1; shift;;
	--enable-smp) ENABLE_SMP=1;   shift;;
	--enable-irq-attach) ENABLE_IRQ_ATTACH=1; shift;;
	*) echo "error: unknown argument '$1'" >&2; usage;;
	esac
done

[ -n "$TOOLCHAIN" ] && [ -n "$CHIP_DIR" ] && [ -n "$ARCH" ] && \
	[ -n "$BOARD_NAME" ] && [ -n "$BUILD_DIR" ] && [ -n "$OUT_DIR" ] || usage

# The kernel repo root is this script's parent directory.
WORKSPACE="$(cd "$(dirname "$0")/.." && pwd)"

export PATH="/opt/qemu-tricore/bin:/opt/tricore-gcc-bin:/opt/riscv-gcc-bin:/opt/arm-gcc-bin:${PATH}"

TAG="${ARCH}_${BOARD_NAME}_gcc"
if [ "$ENABLE_SMP" -eq 1 ]; then
	TAG="${TAG}_smp"
fi
if [ "$ENABLE_IRQ_ATTACH" -eq 1 ]; then
	TAG="${TAG}_irqattach"
fi
KERNEL_A="ulmk_kernel_${TAG}.a"
BOARD_A="ulmk_board_${TAG}.a"
LD="linker_${TAG}.ld"

if [ "$CLEAN" -eq 1 ]; then
	echo "--- clean ---"
	rm -rf "$BUILD_DIR"
fi

echo "--- configure (SDK) ---"
OPT_SIZE_FLAG=""
if [ "$OPTIMIZE_SIZE" -eq 1 ]; then
	OPT_SIZE_FLAG="-DULMK_OPTIMIZE_SIZE=ON"
fi
SMP_FLAG=""
if [ "$ENABLE_SMP" -eq 1 ]; then
	SMP_FLAG="-DULMK_CONFIG_ENABLE_SMP=1"
fi
IRQ_ATTACH_FLAG=""
if [ "$ENABLE_IRQ_ATTACH" -eq 1 ]; then
	IRQ_ATTACH_FLAG="-DULMK_CONFIG_IRQ_ATTACH=1"
fi
cmake -S "$WORKSPACE" -B "$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DULMK_CHIP_DIR="$CHIP_DIR" \
	-DULMK_SDK=ON \
	${OPT_SIZE_FLAG} \
	${SMP_FLAG} \
	${IRQ_ATTACH_FLAG} \
	-GNinja \
	--no-warn-unused-cli

echo "--- build (SDK) ---"
ninja -C "$BUILD_DIR" ulmk

echo "--- assemble SDK tree ---"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/lib" "$OUT_DIR/include/ulmk" "$OUT_DIR/include/board" \
	"$OUT_DIR/linker"

cp "$BUILD_DIR/libulmk_kernel.a" "$OUT_DIR/lib/$KERNEL_A"
cp "$BUILD_DIR/libulmk_board.a"  "$OUT_DIR/lib/$BOARD_A"

# The processed linker script selects kernel sections by archive name;
# rewrite it to match the shipped archive filename.
sed "s/libulmk_kernel\.a/$KERNEL_A/g" \
	"$BUILD_DIR/generated/ulmk.ld" > "$OUT_DIR/linker/$LD"

# Public microkernel API headers.
cp "$WORKSPACE"/include/ulmk/*.h "$OUT_DIR/include/ulmk/"

# Arch-provided public ABI header.  <ulmk/syscall_abi.h> is only a redirector
# to <ulmk_syscall_abi.h>, which lives in the arch include dir and must ship too
# or every consumer of <ulmk/microkernel.h> fails to compile.
cp "$WORKSPACE/arch/$ARCH/include/ulmk_syscall_abi.h" "$OUT_DIR/include/"

# Public board headers (skip board-internal ones).
for h in "$CHIP_DIR"/*.h; do
	case "$(basename "$h")" in *internal*) continue;; esac
	[ -f "$h" ] && cp "$h" "$OUT_DIR/include/board/"
done

# Board driver APIs.  The drivers are compiled into the board archive, so
# without their headers a consumer links against symbols it cannot declare.
# Only drivers/<name>/include ships; drivers/<name>/src is the private side.
DRIVER_HDRS=0
for h in "$CHIP_DIR"/drivers/*/include/*.h; do
	[ -f "$h" ] || continue
	case "$(basename "$h")" in *internal*) continue;; esac
	cp "$h" "$OUT_DIR/include/board/"
	DRIVER_HDRS=$((DRIVER_HDRS + 1))
done

echo "--- verify SDK ---"
ar t "$OUT_DIR/lib/$KERNEL_A" >/dev/null
ar t "$OUT_DIR/lib/$BOARD_A" >/dev/null
grep -q "$KERNEL_A" "$OUT_DIR/linker/$LD"
! grep -q "libulmk_kernel\.a" "$OUT_DIR/linker/$LD"
test -f "$OUT_DIR/include/ulmk/microkernel.h"
test -f "$OUT_DIR/include/ulmk_syscall_abi.h"

# A board that compiles drivers into the archive has to publish their API.
# Guarded rather than piping a failing find: under pipefail its exit status
# would survive 2>/dev/null and abort the script on every driverless board.
DRIVER_DIRS=0
if [ -d "$CHIP_DIR/drivers" ]; then
	DRIVER_DIRS=$(find "$CHIP_DIR/drivers" -maxdepth 2 -type d \
		-name include | wc -l)
fi
if [ "$DRIVER_DIRS" -gt 0 ] && [ "$DRIVER_HDRS" -eq 0 ]; then
	echo "error: board has $DRIVER_DIRS driver include dirs, none shipped" >&2
	exit 1
fi

echo "SDK ready → $OUT_DIR"
echo "  lib/     $(ls "$OUT_DIR/lib")"
echo "  linker/  $(ls "$OUT_DIR/linker")"
echo "  include/ ulmk/ + ulmk_syscall_abi.h + board/ ($DRIVER_HDRS driver APIs)"
