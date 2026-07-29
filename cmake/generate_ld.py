#!/usr/bin/env python3
# cmake/generate_ld.py
# Assembles the final linker script from layered fragments.
# Full specification: docs/linker_spec.md §5
#
# Invocation (see linker_api.cmake):
#   generate_ld.py --chip-dir <path> --arch-dir <path> \
#                  --kernel-dir <path> --snippets <path> \
#                  --output <file> \
#                  [--app <name>]... [--comp <name>]... [--domain <name:region>]...
#
# Assembly order:
#   1. arch prologue (arch-dir/prologue.ld.in)
#   2. chip MEMORY block (chip-dir/memory.ld)
#   3. [optional] chip BMHD fragment (chip-dir/bmhd.ld.in, if HAVE_BMHD=1)
#   4. SECTIONS {
#        kernel/vectors.ld.in
#        kernel/kernel_text.ld.in
#        [rendered snippets/comp_text.ld.in  for each --comp]
#        kernel/user_runtime.ld.in
#        [rendered snippets/app_code.ld.in   for each --app]
#        kernel/kernel_data.ld.in
#        kernel/domain_table.ld.in
#        kernel/kernel_stacks.ld.in
#        [arch/tricore/linker/csa_pool.ld.in    if HAVE_CSA=1]
#        [arch/tricore/linker/small_data.ld.in  if HAVE_SMALL_DATA=1]
#        kernel/kernel_ram_end.ld.in
#        kernel/user_ram_start.ld.in
#        [rendered snippets/domain_data.ld.in for each --domain]
#        kernel/isr_stack.ld.in
#        kernel/user_bss.ld.in
#        kernel/user_pool.ld.in
#      }

import argparse
import os
import re
import sys


def read_fragment(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def render_snippet(path: str, substitutions: dict) -> str:
    content = read_fragment(path)
    for key, val in substitutions.items():
        content = content.replace(f"@{key}@", val)
    return content


def parse_flags(chip_memory_ld: str) -> dict:
    """Extract HAVE_* flag values from the chip memory.ld as integers."""
    flags = {}
    for name in ("HAVE_CSA", "HAVE_CSA_CPU1", "HAVE_CSA_CPU2",
                 "HAVE_SMALL_DATA", "HAVE_BMHD"):
        m = re.search(rf"^\s*{name}\s*=\s*(\d+)\s*;", chip_memory_ld, re.MULTILINE)
        flags[name] = int(m.group(1)) if m else 0
    return flags


def main():
    ap = argparse.ArgumentParser(description="ulmk linker script generator")
    ap.add_argument("--chip-dir",   required=True)
    ap.add_argument("--arch-dir",   required=True)
    ap.add_argument("--kernel-dir", required=True)
    ap.add_argument("--snippets",   required=True)
    ap.add_argument("--output",     required=True)
    ap.add_argument("--app",        action="append", default=[],
                    metavar="NAME")
    ap.add_argument("--comp",       action="append", default=[],
                    metavar="NAME")
    ap.add_argument("--domain",     action="append", default=[],
                    metavar="NAME:REGION")
    args = ap.parse_args()

    out = []

    # 1. Arch prologue
    out.append(read_fragment(os.path.join(args.arch_dir, "prologue.ld.in")))

    # 2. Chip MEMORY block
    chip_memory = read_fragment(os.path.join(args.chip_dir, "memory.ld"))
    out.append(chip_memory)

    flags = parse_flags(chip_memory)

    out.append("\nSECTIONS {\n")

    # Optional chip app/header fragment before vectors (ESP32 app_desc, etc.)
    app_desc = os.path.join(args.chip_dir, "app_desc.ld.in")
    if os.path.exists(app_desc):
        out.append(read_fragment(app_desc))

    # 3. [optional] BMHD
    bmhd_path = os.path.join(args.chip_dir, "bmhd.ld.in")
    if flags["HAVE_BMHD"] and os.path.exists(bmhd_path):
        out.append(read_fragment(bmhd_path))

    # 4a. Vectors — arch-specific when present (RISC-V trap section)
    vectors_arch = os.path.join(args.arch_dir, "vectors.ld.in")
    vectors_kernel = os.path.join(args.kernel_dir, "vectors.ld.in")
    vectors_tables = os.path.join(args.kernel_dir, "vectors_tables.ld.in")
    if os.path.exists(vectors_arch):
        out.append(read_fragment(vectors_arch))
    elif flags["HAVE_BMHD"] and os.path.exists(vectors_tables):
        out.append(read_fragment(vectors_tables))
    else:
        out.append(read_fragment(vectors_kernel))

    # 4b. Kernel text (libulmk_kernel.a only)
    out.append(read_fragment(os.path.join(args.kernel_dir, "kernel_text.ld.in")))

    # 4c. Userspace text lower bound (per-component sections follow)
    out.append("    _ulmk_user_text_start = .;\n")

    # 4d. Per-component text snippets — one MPU-aligned section per component,
    #     selecting exclusively from libulmk_comp_<name>.a
    comp_snippet = os.path.join(args.snippets, "comp_text.ld.in")
    for comp in args.comp:
        out.append(render_snippet(comp_snippet, {"COMP_NAME": comp}))

    # 4e. Per-app code snippets (legacy ulmk_add_app path)
    app_snippet = os.path.join(args.snippets, "app_code.ld.in")
    for app in args.app:
        out.append(render_snippet(app_snippet, {"APP_NAME": app}))

    # 4f. Userspace runtime / orphan flash (libgcc, etc.)
    out.append(read_fragment(os.path.join(args.kernel_dir, "user_runtime.ld.in")))

    # Optional chip flash pad (e.g. ESP32 64 KiB MMU page before .data LMA)
    flash_pad = os.path.join(args.chip_dir, "flash_pad.ld.in")
    if os.path.exists(flash_pad):
        out.append(read_fragment(flash_pad))

    # 4g. Domain descriptor table (flash)
    out.append(read_fragment(os.path.join(args.kernel_dir, "domain_table.ld.in")))

    # 4h. Kernel data (libulmk_kernel.a only) — chip may override for flash LMA
    kd = os.path.join(args.chip_dir, "kernel_data.ld.in")
    if os.path.exists(kd):
        out.append(read_fragment(kd))
    else:
        out.append(read_fragment(os.path.join(args.kernel_dir, "kernel_data.ld.in")))


    # 4i. Kernel stacks
    out.append(read_fragment(os.path.join(args.kernel_dir, "kernel_stacks.ld.in")))

    # 4j. CSA pool (arch-specific)
    if flags["HAVE_CSA"]:
        out.append(read_fragment(os.path.join(args.arch_dir, "csa_pool.ld.in")))

    # 4k. Small-data areas (arch-specific, kernel-owned RAM)
    if flags["HAVE_SMALL_DATA"]:
        out.append(read_fragment(os.path.join(args.arch_dir, "small_data.ld.in")))

    # 4l. End of kernel static RAM
    out.append(read_fragment(os.path.join(args.kernel_dir, "kernel_ram_end.ld.in")))

    # 4m. Start of userspace static RAM (domains/stacks/pool must follow)
    out.append(read_fragment(os.path.join(args.kernel_dir, "user_ram_start.ld.in")))

    # 4n. Per-domain data snippets (includes auto-registered component domains)
    domain_snippet = os.path.join(args.snippets, "domain_data.ld.in")
    for entry in args.domain:
        parts = entry.split(":", 1)
        dname = parts[0]
        dregion = parts[1] if len(parts) > 1 else "KERNEL_RAM"
        out.append(render_snippet(domain_snippet,
                                  {"DOMAIN_NAME": dname, "DOMAIN_REGION": dregion}))

    # 4o. ISR stack (userspace MPU RAM — ISP before PRS elevation)
    out.append(read_fragment(os.path.join(args.kernel_dir, "isr_stack.ld.in")))

    # 4p. Userspace-accessible kernel BSS (root thread stack, board IPC globals)
    out.append(read_fragment(os.path.join(args.kernel_dir, "user_bss.ld.in")))

    # 4q. User pool (always last in KERNEL_RAM)
    out.append(read_fragment(os.path.join(args.kernel_dir, "user_pool.ld.in")))

    # 4r. Secondary DSPR CSA/ISP/park — AFTER KERNEL_RAM so VMA '.' is not
    #     left in 0x6xxx/0x5xxx (would corrupt user_ram_start / zero_words).
    if flags.get("HAVE_CSA_CPU1"):
        cpu1 = os.path.join(args.arch_dir, "csa_pool_cpu1.ld.in")
        if os.path.exists(cpu1):
            out.append(read_fragment(cpu1))
    if flags.get("HAVE_CSA_CPU2"):
        cpu2 = os.path.join(args.arch_dir, "csa_pool_cpu2.ld.in")
        if os.path.exists(cpu2):
            out.append(read_fragment(cpu2))

    out.append("\n} /* SECTIONS */\n")

    mem_sym = os.path.join(args.kernel_dir, "memory_symbols.ld.in")
    if os.path.exists(mem_sym):
        out.append(read_fragment(mem_sym))

    # Optional chip ROM / PROVIDE fragment (e.g. ESP32-P4 esp_rom *.ld INCLUDE)
    rom_ld = os.path.join(args.chip_dir, "rom.ld")
    if os.path.exists(rom_ld):
        out.append(read_fragment(rom_ld))

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print(f"[generate_ld.py] wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
