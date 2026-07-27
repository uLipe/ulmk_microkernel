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
#   include/board/*.h                      public board API (no *internal* headers)
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

if [ "$ARCH" = "c29" ]; then
	# Discover TI C29 toolchain when mounted at /ti (see tools/dev.py _ti_mount).
	_TI_CGT=$(ls -d /ti/ccs*/ccs/tools/compiler/ti-cgt-c29* 2>/dev/null \
		|| ls -d /ti/tools/compiler/ti-cgt-c29* 2>/dev/null | head -1 || true)
	if [ -n "$_TI_CGT" ]; then
		export PATH="$_TI_CGT/bin:${PATH}"
	fi
fi

if [ "$ARCH" = "c29" ]; then
	COMPILER_TAG="ticlang"
else
	COMPILER_TAG="gcc"
fi

TAG="${ARCH}_${BOARD_NAME}_${COMPILER_TAG}"
if [ "$ENABLE_SMP" -eq 1 ]; then
	TAG="${TAG}_smp"
fi
if [ "$ENABLE_IRQ_ATTACH" -eq 1 ]; then
	TAG="${TAG}_irqattach"
fi
KERNEL_A="ulmk_kernel_${TAG}.a"
BOARD_A="ulmk_board_${TAG}.a"
# TI C29 uses .cmd; GNU arches use .ld.
if [ "$ARCH" = "c29" ]; then
	LD="linker_${TAG}.cmd"
else
	LD="linker_${TAG}.ld"
fi

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
if [ "$ARCH" = "c29" ]; then
	# TI .cmd: archive references use <libulmk_kernel.a>(...) syntax.
	sed "s/<libulmk_kernel\.a>/<${KERNEL_A}>/g" \
		"$BUILD_DIR/generated/ulmk.ld" > "$OUT_DIR/linker/$LD"
else
	sed "s/libulmk_kernel\.a/$KERNEL_A/g" \
		"$BUILD_DIR/generated/ulmk.ld" > "$OUT_DIR/linker/$LD"
fi

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

echo "--- verify SDK ---"
if [ "$ARCH" = "c29" ]; then
	c29ar t "$OUT_DIR/lib/$KERNEL_A" >/dev/null
	c29ar t "$OUT_DIR/lib/$BOARD_A" >/dev/null
else
	ar t "$OUT_DIR/lib/$KERNEL_A" >/dev/null
	ar t "$OUT_DIR/lib/$BOARD_A" >/dev/null
	grep -q "$KERNEL_A" "$OUT_DIR/linker/$LD"
	! grep -q "libulmk_kernel\.a" "$OUT_DIR/linker/$LD"
fi
test -f "$OUT_DIR/include/ulmk/microkernel.h"
test -f "$OUT_DIR/include/ulmk_syscall_abi.h"

echo "SDK ready → $OUT_DIR"
echo "  lib/     $(ls "$OUT_DIR/lib")"
echo "  linker/  $(ls "$OUT_DIR/linker")"
echo "  include/ ulmk/ + ulmk_syscall_abi.h + board/"
