#!/usr/bin/env python3
"""
ulmk development container and build/test driver.

Subcommands:
    (none)              Enter interactive dev container shell
    build               Configure and compile kernel ELF (CMake/Ninja)
    build qemu          Build if needed, then run in QEMU
    build --kernel      Emit a distributable SDK (libs + linker + headers)
    components list     Discover under components/, <board>/components/, ../ulmk_apps/
    components status   Show manifest default vs .ulmk/components.conf
    components enable   Persist component ON in .ulmk/components.conf
    components disable  Remove component from .ulmk/components.conf
    tests unit          Run host-compiled unit tests
    tests e2e           Run SDK suite (black-box QEMU cases under tests/sdk_suite/)
    tests integ         Arch whitebox only (e.g. ctx_early_tricore)
    killall             Stop all running dev containers

Components default OFF in CMake manifests. Enable before building a demo
(hello_world is the only builtin; ping_pong lives in sibling ulmk_apps):

    python3 tools/dev.py components enable hello_world
    python3 tools/dev.py build --board boards/qemu_riscv_virt
    python3 tools/dev.py build qemu --board boards/qemu_riscv_virt

One-shot build without saving config:

    python3 tools/dev.py build --no-components --component hello_world
    python3 tools/dev.py build --no-components --component ping_pong
    python3 tools/dev.py build --no-components

Options:
    --rebuild           Force Docker image rebuild (shell entry only)

See also: python3 tools/dev.py --help
          python3 tools/dev.py build --help
          python3 tools/dev.py components --help
          python3 tools/dev.py tests --help
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

IMAGE_NAME     = "ulipe-microkernel:dev"
DOCKERFILE_DIR = Path(__file__).parent / "docker"
WORKSPACE_ROOT = Path(__file__).parent.parent.resolve()
TESTS_DIR      = WORKSPACE_ROOT / "tests"
BUILD_DIR      = WORKSPACE_ROOT.parent / "build"
COMPONENTS_CONF = WORKSPACE_ROOT / ".ulmk" / "components.conf"

DEFAULT_BOARD  = "boards/qemu_tc3xx"
DEFAULT_BOARD_RISCV = "boards/qemu_riscv_virt"

TEST_TIMEOUT = 120

ARCH_TRICORE_ONLY_TESTS = frozenset({"ctx_early_tricore"})
SDK_SUITE_DIR = TESTS_DIR / "sdk_suite"
SDK_WHITEBOX_DIR = SDK_SUITE_DIR / "arch_whitebox"

QEMU_TRICORE = "/opt/qemu-tricore/bin/qemu-system-tricore"
QEMU_RISCV   = "qemu-system-riscv32"
QEMU_ARM     = "qemu-system-arm"

# PATH prefix inside the container: all cross toolchains + QEMU forks.
CONTAINER_PATH = ('export PATH="/opt/qemu-tricore/bin:/opt/tricore-gcc-bin:'
                  '/opt/riscv-gcc-bin:/opt/arm-gcc-bin:${PATH}"')


# ---------------------------------------------------------------------------
# Docker helpers
# ---------------------------------------------------------------------------

def _check_docker() -> None:
    if shutil.which("docker") is None:
        sys.exit("docker not found on PATH — install Docker and try again.")


def _image_exists() -> bool:
    result = subprocess.run(
        ["docker", "image", "inspect", IMAGE_NAME],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def _build_image() -> None:
    print(f"Building {IMAGE_NAME} …")
    print("(first build compiles QEMU from source — expect ~20-30 min)\n")
    subprocess.run(
        [
            "docker", "build",
            "--target", "dev",
            "-t", IMAGE_NAME,
            str(DOCKERFILE_DIR),
        ],
        check=True,
    )


def _base_docker_cmd(interactive: bool) -> list[str]:
    cmd = [
        "docker", "run",
        "--rm",
        "--volume", f"{WORKSPACE_ROOT}:/workspace",
        "--workdir", "/workspace",
    ]
    if interactive:
        cmd += ["--interactive", "--tty"]
    return cmd


def _run_container() -> None:
    cmd = _base_docker_cmd(interactive=True)
    cmd += [IMAGE_NAME, "/bin/bash"]
    os.execvp("docker", cmd)


# ---------------------------------------------------------------------------
# Component discovery and configuration
# ---------------------------------------------------------------------------

_REGISTER_KW = frozenset({
    "NAME", "ENABLED", "SOURCES", "INCLUDE_DIRS", "REQUIRES",
    "ROOT_THREAD", "LINKER_FRAGMENT",
})


def _apps_mount() -> list[str]:
    """Mount sibling ulmk_apps so CMake can see ../ulmk_apps inside the container."""
    apps_root = WORKSPACE_ROOT.parent / "ulmk_apps"
    if apps_root.is_dir():
        return ["--volume", f"{apps_root}:/ulmk_apps:ro"]
    return []


def _component_scan_dirs(board_host: Path | None = None) -> list[Path]:
    """Return directories that may contain ulmk_component_register()."""
    dirs: list[Path] = []

    def _append_leaf_or_children(root: Path) -> None:
        """If CMakeLists registers a component, append root; else scan children.

        Supports aggregator packages (e.g. ulmk_apps/silicon/) whose top-level
        CMakeLists only add_subdirectory()s leaf components.
        """
        cmake = root / "CMakeLists.txt"
        if not cmake.is_file():
            return
        text = cmake.read_text()
        if re.search(r"ulmk_component_register\s*\(", text):
            dirs.append(root)
            return
        for sub in sorted(root.iterdir()):
            if sub.is_dir() and (sub / "CMakeLists.txt").is_file():
                dirs.append(sub)

    comp_root = WORKSPACE_ROOT / "components"
    if comp_root.is_dir():
        for entry in sorted(comp_root.iterdir()):
            if entry.is_dir():
                _append_leaf_or_children(entry)

    if board_host is not None:
        board_comps = board_host / "components"
        if board_comps.is_dir():
            for entry in sorted(board_comps.iterdir()):
                if entry.is_dir() and (entry / "CMakeLists.txt").is_file():
                    dirs.append(entry)

    apps_root = WORKSPACE_ROOT.parent / "ulmk_apps"
    if apps_root.is_dir():
        for entry in sorted(apps_root.iterdir()):
            if entry.is_dir():
                _append_leaf_or_children(entry)

    return dirs


def _parse_component_cmake(path: Path) -> dict | None:
    text = path.read_text()
    m = re.search(r"ulmk_component_register\s*\((.*)\)", text, re.DOTALL)
    if not m:
        return None

    body = m.group(1)
    name_m = re.search(r"NAME\s+(\w+)", body)
    if not name_m:
        return None

    enabled_m = re.search(r"ENABLED\s+(ON|OFF)", body)
    default = enabled_m.group(1) if enabled_m else "OFF"

    requires: list[str] = []
    req_m = re.search(
        r"REQUIRES\s+(.*?)(?=\n\s*(?:"
        + "|".join(_REGISTER_KW - {"REQUIRES"})
        + r")\b|\))",
        body,
        re.DOTALL,
    )
    if req_m:
        requires = req_m.group(1).split()

    try:
        comp_path = path.parent.relative_to(WORKSPACE_ROOT)
    except ValueError:
        comp_path = path.parent

    return {
        "name": name_m.group(1),
        "default": default,
        "requires": requires,
        "root_thread": bool(re.search(r"\bROOT_THREAD\b", body)),
        "path": comp_path,
    }


def _discover_components(board_host: Path | None = None) -> list[dict]:
    found: list[dict] = []
    for d in _component_scan_dirs(board_host):
        info = _parse_component_cmake(d / "CMakeLists.txt")
        if info:
            found.append(info)
    return sorted(found, key=lambda c: c["name"])


def _component_names(board_host: Path | None = None) -> list[str]:
    return [c["name"] for c in _discover_components(board_host)]


def _load_components_conf() -> set[str]:
    if not COMPONENTS_CONF.is_file():
        return set()
    enabled: set[str] = set()
    for line in COMPONENTS_CONF.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        name, val = line.split("=", 1)
        name = name.strip()
        val = val.strip().upper()
        if val in ("ON", "1", "YES", "TRUE"):
            enabled.add(name)
    return enabled


def _save_components_conf(enabled: set[str]) -> None:
    COMPONENTS_CONF.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Managed by: python3 tools/dev.py components enable/disable",
        "",
    ]
    for name in sorted(enabled):
        lines.append(f"{name}=ON")
    lines.append("")
    COMPONENTS_CONF.write_text("\n".join(lines))


def _resolve_enabled_components(
    cli_components: list[str] | None,
    no_components: bool,
) -> set[str]:
    if no_components:
        return set(cli_components or [])

    enabled = _load_components_conf()
    for name in cli_components or []:
        enabled.add(name)
    return enabled


def _component_cmake_flags(
    enabled: set[str], board_host: Path | None = None
) -> list[str]:
    flags: list[str] = []
    for name in _component_names(board_host):
        val = "ON" if name in enabled else "OFF"
        flags.append(f"-DULMK_COMP_{name}_ENABLED={val}")
    return flags


def _validate_component_names(
    names: list[str], board_host: Path | None = None
) -> None:
    known = set(_component_names(board_host))
    for name in names:
        if name not in known:
            sys.exit(
                f"error: unknown component '{name}'.\n"
                f"Known: {', '.join(sorted(known)) or '(none)'}"
            )


def _run_components(args: argparse.Namespace) -> None:
    action = args.components_action
    board_host, _, _ = _resolve_board_path(getattr(args, "board", None))
    comps = _discover_components(board_host)

    if action == "list":
        print(f"components ({len(comps)}):")
        for c in comps:
            root = " ROOT_THREAD" if c["root_thread"] else ""
            req = f" REQUIRES={','.join(c['requires'])}" if c["requires"] else ""
            print(f"  {c['name']:20s} default={c['default']}{root}{req}")
            print(f"    {c['path']}")
        return

    conf = _load_components_conf()

    if action == "status":
        print(f"{'COMPONENT':<20} {'DEFAULT':<8} {'CONFIG':<8} {'ROOT':<5} REQUIRES")
        for c in comps:
            cfg = "ON" if c["name"] in conf else "-"
            root = "yes" if c["root_thread"] else "-"
            req = ",".join(c["requires"]) if c["requires"] else "-"
            print(f"{c['name']:<20} {c['default']:<8} {cfg:<8} {root:<5} {req}")
        if conf:
            print(f"\nconfig: {COMPONENTS_CONF.relative_to(WORKSPACE_ROOT)}")
        else:
            print("\nconfig: (none — all components OFF)")
        return

    if action == "enable":
        _validate_component_names(args.names, board_host)
        conf |= set(args.names)
        _save_components_conf(conf)
        print(f"enabled: {', '.join(args.names)}")
        print(f"saved → {COMPONENTS_CONF.relative_to(WORKSPACE_ROOT)}")
        return

    if action == "disable":
        _validate_component_names(args.names, board_host)
        for name in args.names:
            conf.discard(name)
        _save_components_conf(conf)
        print(f"disabled: {', '.join(args.names)}")
        print(f"saved → {COMPONENTS_CONF.relative_to(WORKSPACE_ROOT)}")
        return

    sys.exit(f"error: unknown components action '{action}'")


# ---------------------------------------------------------------------------
# Build helpers
# ---------------------------------------------------------------------------

def _parse_board_cmake(board_dir: Path) -> dict:
    """Extract UL_BOARD_* variables from board.cmake."""
    cmake_file = board_dir / "board.cmake"
    if not cmake_file.exists():
        sys.exit(f"error: board.cmake not found in {board_dir}")

    text = cmake_file.read_text()
    board: dict = {}
    for m in re.finditer(r'set\(\s*(\w+)\s+(.*?)\)', text, re.DOTALL):
        var = m.group(1)
        raw = re.sub(r'#[^\n]*', '', m.group(2))
        values = [v.strip('"') for v in raw.split()]
        board[var] = values[0] if len(values) == 1 else values
    return board


def _board_arch(board: dict) -> str:
    arch = board.get("UL_BOARD_ARCH", "tricore")
    if isinstance(arch, list):
        arch = arch[0] if arch else "tricore"
    return arch


def _toolchain_for_arch(arch: str, board: dict | None = None) -> str:
    if board:
        tc = board.get("UL_BOARD_TOOLCHAIN", "")
        if isinstance(tc, list):
            tc = tc[0] if tc else ""
        if tc:
            return f"/workspace/cmake/toolchain-{tc}.cmake"
    return f"/workspace/cmake/toolchain-{arch}-gcc.cmake"


def _build_subdir(arch: str, board_name: str) -> str:
    """Per-board build dir so distinct boards of one arch never share objects
    (e.g. the two ARM Cortex-M boards mps2-an500 / mps2-an505)."""
    return f"ulipe-{arch}-{board_name}"


def _qemu_binary(arch: str) -> str:
    if arch == "riscv":
        return QEMU_RISCV
    if arch == "arm":
        return QEMU_ARM
    return QEMU_TRICORE


def _resolve_board_path(board_arg: str | None) -> tuple[Path, str, list[str]]:
    if board_arg:
        board_host = Path(board_arg).resolve()
        if not board_host.is_dir():
            sys.exit(f"error: board directory not found: {board_arg}")
        if board_host.is_relative_to(WORKSPACE_ROOT):
            board_container = f"/workspace/{board_host.relative_to(WORKSPACE_ROOT)}"
            extra_mounts: list[str] = []
        else:
            board_container = "/board"
            extra_mounts = ["--volume", f"{board_host}:/board:ro"]
    else:
        board_host = WORKSPACE_ROOT / DEFAULT_BOARD
        board_container = f"/workspace/{DEFAULT_BOARD}"
        extra_mounts = []
    return board_host, board_container, extra_mounts


def _build_shell(board_container: str, board: dict,
                 clean: bool, run_qemu: bool,
                 component_flags: list[str], build_subdir: str,
                 optimize_size: bool = False,
                 enable_smp: bool = False) -> str:
    arch = _board_arch(board)
    toolchain = _toolchain_for_arch(arch, board)
    qemu = _qemu_binary(arch)

    lines: list[str] = [
        "set -e",
        CONTAINER_PATH,
        "",
    ]

    if clean:
        lines += [
            "echo '--- clean ---'",
            f"rm -rf /build/{build_subdir}",
            "",
        ]

    cfg = [
        "echo '--- configure ---'",
        f"cmake -S /workspace -B /build/{build_subdir} \\",
        f"    -DCMAKE_TOOLCHAIN_FILE={toolchain} \\",
        f"    -DULMK_CHIP_DIR={board_container} \\",
    ]
    if optimize_size:
        cfg.append("    -DULMK_OPTIMIZE_SIZE=ON \\")
    if enable_smp:
        cfg.append("    -DULMK_CONFIG_ENABLE_SMP=1 \\")
    for flag in component_flags:
        cfg.append(f"    {flag} \\")
    cfg += [
        "    -GNinja \\",
        "    --no-warn-unused-cli",
        "",
    ]
    lines.extend(cfg)

    lines += [
        "echo '--- build ---'",
        f"ninja -C /build/{build_subdir}",
        "",
        f"echo 'Build OK → /build/{build_subdir}/ulmk'",
    ]

    if run_qemu:
        machine = board.get("UL_BOARD_QEMU_MACHINE", "")
        if isinstance(machine, list):
            machine = machine[0] if machine else ""
        extra = board.get("UL_BOARD_QEMU_EXTRA", "")
        if isinstance(extra, list):
            extra_args = " ".join(extra)
        else:
            extra_args = str(extra) if extra else ""
        lines += [
            "",
            "echo '--- running in QEMU (Ctrl-C to stop) ---'",
            f"{qemu} -machine {machine} {extra_args} \\",
            f"    -kernel /build/{build_subdir}/ulmk -nographic",
        ]

    return "\n".join(lines)


def _qemu_only_shell(board: dict, build_subdir: str) -> str:
    """Run an already-built kernel in QEMU without reconfiguring."""
    arch = _board_arch(board)
    qemu = _qemu_binary(arch)
    machine = board.get("UL_BOARD_QEMU_MACHINE", "")
    if isinstance(machine, list):
        machine = machine[0] if machine else ""
    extra = board.get("UL_BOARD_QEMU_EXTRA", "")
    if isinstance(extra, list):
        extra_args = " ".join(extra)
    else:
        extra_args = str(extra) if extra else ""

    return "\n".join([
        "set -e",
        CONTAINER_PATH,
        f"test -f /build/{build_subdir}/ulmk",
        "echo '--- running in QEMU (Ctrl-C to stop) ---'",
        f"{qemu} -machine {machine} {extra_args} \\",
        f"    -kernel /build/{build_subdir}/ulmk -nographic",
    ])


def _sdk_build_shell(board_container: str, board: dict, board_name: str,
                     clean: bool, optimize_size: bool = False) -> str:
    """Compile kernel + arch + board into a distributable SDK directory.

    Delegates the actual work to tools/sdk_build.sh so the exact build and
    assemble steps stay identical between this driver and the SDK consumer
    integration test (tests/sdk_e2e).
    """
    arch = _board_arch(board)
    toolchain = _toolchain_for_arch(arch, board)
    build = f"/build/ulipe-{arch}-sdk"
    # Kept under dist/ so the output tree never collides with the CMake
    # validation executable, which is itself named "ulmk" at the build root.
    sdk = f"{build}/dist/ulmk"

    clean_flag = " --clean" if clean else ""
    size_flag = " --optimize-size" if optimize_size else ""
    return (
        "set -e\n"
        f"bash /workspace/tools/sdk_build.sh"
        f" --toolchain {toolchain}"
        f" --chip-dir {board_container}"
        f" --arch {arch}"
        f" --board-name {board_name}"
        f" --build-dir {build}"
        f" --out-dir {sdk}"
        f"{clean_flag}{size_flag}"
    )


def _run_sdk_build(args: argparse.Namespace) -> None:
    if args.action == "qemu":
        sys.exit("error: 'build qemu' cannot be combined with --kernel")
    if not args.board:
        sys.exit("error: --kernel requires --board <board_dir>")

    board_host, board_container, extra_mounts = _resolve_board_path(args.board)
    board = _parse_board_cmake(board_host)
    board_name = board_host.name
    arch = _board_arch(board)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    shell_cmd = _sdk_build_shell(
        board_container, board, board_name, args.clean,
        getattr(args, "optimize_size", False))

    cmd = [
        "docker", "run", "--rm",
        "--volume", f"{WORKSPACE_ROOT}:/workspace",
        "--volume", f"{BUILD_DIR}:/build",
    ] + extra_mounts + _apps_mount()
    cmd += [IMAGE_NAME, "/bin/bash", "-c", shell_cmd]
    subprocess.run(cmd, check=True)

    host_sdk = BUILD_DIR / f"ulipe-{arch}-sdk" / "dist" / "ulmk"
    print(f"\nDistributable SDK on host → {host_sdk}")


def _run_host_build(board_host: Path, board: dict, build_subdir: str,
                    clean: bool, component_flags: list[str],
                    enable_smp: bool, optimize_size: bool) -> None:
    """Native host cmake/ninja for boards that need Espressif toolchain."""
    arch = _board_arch(board)
    tc_name = board.get("UL_BOARD_TOOLCHAIN", f"{arch}-gcc")
    if isinstance(tc_name, list):
        tc_name = tc_name[0] if tc_name else f"{arch}-gcc"
    toolchain = WORKSPACE_ROOT / "cmake" / f"toolchain-{tc_name}.cmake"
    build_path = BUILD_DIR / build_subdir

    env = os.environ.copy()
    idf = env.get("ESP_IDF_PATH") or env.get("IDF_PATH") or "/home/ulipe/fun/esp-idf"
    env["ESP_IDF_PATH"] = idf
    env["IDF_PATH"] = idf

    esp_bins = sorted(Path.home().glob(
        ".espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin"))
    # Prefer GCC 14+ (esp-14.*) over legacy esp-2021r2 which rejects
    # -march=…_zicsr_zifencei.
    preferred = [p for p in esp_bins if "esp-14" in str(p) or "esp-15" in str(p)]
    pick = preferred[-1] if preferred else (esp_bins[-1] if esp_bins else None)
    if pick:
        env["PATH"] = f"{pick}:{env.get('PATH', '')}"
        env["CMAKE_C_COMPILER"] = str(pick / "riscv32-esp-elf-gcc")
        env["CMAKE_ASM_COMPILER"] = str(pick / "riscv32-esp-elf-gcc")

    if clean and build_path.exists():
        import shutil
        shutil.rmtree(build_path)
    build_path.mkdir(parents=True, exist_ok=True)

    cfg = [
        "cmake", "-S", str(WORKSPACE_ROOT), "-B", str(build_path),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DULMK_CHIP_DIR={board_host}",
        "-GNinja", "--no-warn-unused-cli",
    ]
    if optimize_size:
        cfg.append("-DULMK_OPTIMIZE_SIZE=ON")
    if enable_smp:
        cfg.append("-DULMK_CONFIG_ENABLE_SMP=1")
    cfg.extend(component_flags)

    print("--- host configure ---")
    subprocess.run(cfg, check=True, env=env)
    print("--- host build ---")
    subprocess.run(["ninja", "-C", str(build_path)], check=True, env=env)
    print(f"Build OK → {build_path}/ulmk")


def _run_build(args: argparse.Namespace) -> None:
    if getattr(args, "kernel", False):
        _run_sdk_build(args)
        return

    run_qemu = (args.action == "qemu")

    board_host, board_container, extra_mounts = _resolve_board_path(args.board)
    board = _parse_board_cmake(board_host)
    arch = _board_arch(board)
    build_subdir = _build_subdir(arch, board_host.name)

    qemu_machine = board.get("UL_BOARD_QEMU_MACHINE", "")
    if isinstance(qemu_machine, list):
        qemu_machine = qemu_machine[0] if qemu_machine else ""
    if run_qemu and not qemu_machine:
        sys.exit(
            "error: board does not support QEMU "
            "(UL_BOARD_QEMU_MACHINE not set in board.cmake)"
        )

    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    enabled = _resolve_enabled_components(
        getattr(args, "components", None),
        getattr(args, "no_components", False),
    )
    component_flags = _component_cmake_flags(enabled, board_host)
    if "silicon_wcet" in enabled:
        component_flags.append("-DULMK_CONFIG_SYSCALL_WCET=1")
    enable_smp = bool(getattr(args, "enable_smp", False))
    if "silicon_smp_smoke" in enabled:
        enable_smp = True

    host_flag = board.get("UL_BOARD_HOST_BUILD", "0")
    if isinstance(host_flag, list):
        host_flag = host_flag[0] if host_flag else "0"
    if str(host_flag) in ("1", "ON", "TRUE", "true"):
        _run_host_build(
            board_host, board, build_subdir, args.clean, component_flags,
            enable_smp, getattr(args, "optimize_size", False))
        return

    elf = BUILD_DIR / build_subdir / "ulmk"
    if run_qemu and not args.clean and elf.is_file():
        shell_cmd = _qemu_only_shell(board, build_subdir)
    else:
        shell_cmd = _build_shell(
            board_container, board, args.clean, run_qemu, component_flags,
            build_subdir, getattr(args, "optimize_size", False),
            enable_smp)

    cmd = [
        "docker", "run", "--rm",
        "--volume", f"{WORKSPACE_ROOT}:/workspace",
        "--volume", f"{BUILD_DIR}:/build",
    ] + extra_mounts + _apps_mount()

    if run_qemu and sys.stdout.isatty():
        cmd += ["--interactive", "--tty"]

    cmd += [IMAGE_NAME, "/bin/bash", "-c", shell_cmd]
    subprocess.run(cmd, check=True)


# ---------------------------------------------------------------------------
# Killall
# ---------------------------------------------------------------------------

def _killall() -> None:
    result = subprocess.run(
        ["docker", "ps", "-q", "--filter", f"ancestor={IMAGE_NAME}"],
        capture_output=True,
        text=True,
    )
    containers = result.stdout.split()
    if not containers:
        print("No running containers found.")
        return
    print(f"Killing {len(containers)} container(s)…")
    subprocess.run(["docker", "kill"] + containers, check=False)
    print("Done.")


# ---------------------------------------------------------------------------
# Test runner helpers
# ---------------------------------------------------------------------------

def _discover_tests(kind: str, arch: str | None = None,
                    *, include_smp: bool = False,
                    include_smp4: bool = False) -> list[str]:
    result = []
    if kind == "e2e":
        # Prefer sdk_suite/<case>/; keep legacy *_e2e dirs as fallback.
        if SDK_SUITE_DIR.is_dir():
            for d in sorted(SDK_SUITE_DIR.iterdir()):
                if not d.is_dir() or d.name.startswith("_"):
                    continue
                if d.name == "arch_whitebox":
                    continue
                # smp4_*: 4-hart scale cases — only with --include-smp4.
                if d.name.startswith("smp4_"):
                    if not include_smp4 or arch != "riscv":
                        continue
                # smp_*: 2-hart cases — --include-smp (full UP+smp) or
                # --include-smp4 (smp-only scale job also reuses them).
                elif d.name.startswith("smp_"):
                    if not (include_smp or include_smp4) or arch != "riscv":
                        continue
                elif include_smp4:
                    # smp4 job: skip UP suite — only smp_* + smp4_*.
                    continue
                if (d / "Makefile").exists():
                    result.append(f"sdk_suite/{d.name}")
            if result:
                return result
        for d in sorted(TESTS_DIR.iterdir()):
            if d.is_dir() and d.name.endswith("_e2e") and (d / "Makefile").exists():
                result.append(d.name)
        return result

    for d in sorted(TESTS_DIR.iterdir()):
        if not d.is_dir() or not (d / "Makefile").exists():
            continue
        if d.name.endswith("_e2e") or d.name == "sdk_suite":
            continue
        is_unit = d.name.endswith("_unit")
        if (kind == "unit") == is_unit:
            result.append(d.name)
    # Arch whitebox (legacy integ path) — TriCore CSA early, etc.
    if kind == "integ" and SDK_WHITEBOX_DIR.is_dir():
        for d in sorted(SDK_WHITEBOX_DIR.iterdir()):
            if d.is_dir() and (d / "Makefile").exists():
                result.append(f"sdk_suite/arch_whitebox/{d.name}")
    return result


def _sdk_cache_vars(arch: str, make_extra: str, test_name: str = "") -> str:
    """Make variables for a shared SDK cache directory inside the container."""
    board = "qemu_tc3xx"
    if arch == "riscv":
        m = re.search(r"BOARD=boards/(\S+)", make_extra)
        if not m:
            m = re.search(r"BOARD=(\S+)", make_extra)
        board = m.group(1) if m else "qemu_riscv_virt"
        if board.startswith("boards/"):
            board = board[len("boards/"):]
    elif arch == "arm":
        m = re.search(r"ARM_BOARD=(\S+)", make_extra)
        board = m.group(1) if m else "qemu_mps2_an500"
    tag = f"{arch}_{board}_gcc"
    # SMP sdk_suite cases build a separate kernel with ULMK_CONFIG_ENABLE_SMP=1.
    base = test_name.rsplit("/", 1)[-1] if test_name else ""
    if base.startswith("smp_") or base.startswith("smp4_"):
        tag += "_smp"
    # irq_attach needs ULMK_CONFIG_IRQ_ATTACH=1 (default off).
    if base == "irq_attach":
        tag += "_irqattach"
    return f"SDK_CACHE=/workspace/tests/sdk_suite/_sdk_cache/{tag}"


def _run_one_shell(kind: str, name: str, arch: str, make_extra: str = "",
                   *, with_path: bool = True) -> str:
    # make_extra carries per-board make variables (e.g. ARM_BOARD=<name>) so a
    # single arch that ships multiple QEMU boards selects the right one.
    ext = f" {make_extra}" if make_extra else ""
    steps = [
        f"echo '=== {name} (ARCH={arch}{ext}) ==='",
        f"cd /workspace/tests/{name}",
        "make clean -s 2>/dev/null || true",
    ]
    if kind == "e2e":
        if with_path:
            steps.insert(0, CONTAINER_PATH)
        cache = _sdk_cache_vars(arch, make_extra, name)
        steps.append(
            f"timeout --kill-after=5 {TEST_TIMEOUT} "
            f"make run ARCH={arch}{ext} {cache}")
        return " && ".join(steps)
    if kind == "integ":
        steps.append(f"make gen_config -s ARCH={arch}{ext}")
    steps.append("rm -rf integ_kernel libtest_kernel.a 2>/dev/null || true")
    steps += [
        f"make -s ARCH={arch}{ext}",
        f"timeout --kill-after=5 {TEST_TIMEOUT} make run ARCH={arch}{ext}",
    ]
    return " && ".join(steps)


def _filter_integ_tests(tests: list[str], arch: str) -> list[str]:
    if arch == "tricore":
        return tests
    return [t for t in tests if Path(t).name not in ARCH_TRICORE_ONLY_TESTS
            and t not in ARCH_TRICORE_ONLY_TESTS]


def _json_record(kind: str, name: str, arch: str, board: str,
                 status: str) -> str:
    """Append one JSONL row when ULMK_TEST_JSON_DIR is set."""
    payload = json.dumps(
        {
            "case": name,
            "kind": kind,
            "arch": arch,
            "board": board or "",
            "status": status,
        },
        separators=(",", ":"),
    )
    # Single-quoted JSON is safe: our fields have no single quotes.
    return (
        "if [ -n \"${ULMK_TEST_JSON_DIR:-}\" ]; then "
        f"printf '%s\\n' '{payload}' "
        ">> \"${ULMK_TEST_JSON_DIR}/results.jsonl\"; "
        f"fi; echo '{payload}'"
    )


def _run_all_shell(kind: str, tests: list[str], arch: str,
                   make_extra: str = "", board: str = "",
                   result_arch: str | None = None) -> str:
    rec_arch = result_arch if result_arch is not None else arch
    lines = [CONTAINER_PATH, "FAILED=''"]
    if kind == "e2e" and tests:
        cache = _sdk_cache_vars(arch, make_extra, tests[0])
        ext = f" {make_extra}" if make_extra else ""
        lines.append(
            f"echo '=== warming SDK cache ===' && "
            f"mkdir -p /workspace/tests/sdk_suite/_sdk_cache && "
            f"( cd /workspace/tests/{tests[0]} && "
            f"make sdk -s ARCH={arch}{ext} {cache} ) || "
            f"FAILED=\"$FAILED sdk_warm\""
        )
    for name in tests:
        snippet = _run_one_shell(kind, name, arch, make_extra, with_path=False)
        rec_ok = _json_record(kind, name, rec_arch, board, "PASS")
        rec_bad = _json_record(kind, name, rec_arch, board, "FAIL")
        lines.append(
            f"if ( {snippet} ); then {rec_ok}; "
            f"else FAILED=\"$FAILED {name}\"; {rec_bad}; fi"
        )
    lines.append(
        "if [ -z \"$FAILED\" ]; then "
        "echo; echo '=== ALL TESTS PASSED ==='; "
        "else "
        "echo; echo \"=== FAILED:$FAILED ===\"; exit 1; "
        "fi"
    )
    return " ; ".join(lines)


def _run_tests(args: argparse.Namespace) -> None:
    kind      = args.kind
    test_name = args.test
    include_smp = bool(getattr(args, "include_smp", False))
    include_smp4 = bool(getattr(args, "include_smp4", False))

    _killall()

    board_host, _, _ = _resolve_board_path(getattr(args, "board", None))
    arch = _board_arch(_parse_board_cmake(board_host))

    if include_smp and include_smp4:
        sys.exit("error: use either --include-smp or --include-smp4, not both")

    if include_smp or include_smp4:
        if kind != "e2e":
            sys.exit(
                "error: --include-smp/--include-smp4 is only valid with "
                "'tests e2e'"
            )
        if arch != "riscv":
            sys.exit(
                "error: --include-smp/--include-smp4 requires a RISC-V board "
                "(QEMU SMP is not supported elsewhere)"
            )
        if include_smp4 and board_host.name != "qemu_riscv_virt_smp4":
            sys.exit(
                "error: --include-smp4 requires "
                "--board boards/qemu_riscv_virt_smp4"
            )

    # ARM ships two QEMU boards (mps2-an500 / mps2-an505) under one arch; the
    # test Makefiles pick the board via ARM_BOARD, so forward the selected one.
    # RISC-V smp4 board: override BOARD + SMP_N so smp_* reuse the 4-hart ELF.
    make_extra = ""
    if arch == "arm":
        make_extra = f"ARM_BOARD={board_host.name}"
    elif arch == "riscv" and board_host.name != "qemu_riscv_virt":
        make_extra = f"BOARD=boards/{board_host.name}"
        if include_smp4 or board_host.name == "qemu_riscv_virt_smp4":
            make_extra += " SMP_N=4"
    board_name = board_host.name
    json_arch = "host" if kind == "unit" else arch
    json_board = "" if kind == "unit" else board_name

    json_dir = getattr(args, "json_dir", None)
    docker_env: list[str] = []
    if json_dir:
        json_path = Path(json_dir).resolve()
        json_path.mkdir(parents=True, exist_ok=True)
        docker_env = ["-e", "ULMK_TEST_JSON_DIR=/test-results"]

    # Single --test sdk_suite/smp*_ on RISC-V may omit the include flags.
    base = test_name.rsplit("/", 1)[-1] if test_name else ""
    discover_smp = include_smp or (
        bool(test_name) and arch == "riscv" and base.startswith("smp_")
        and not base.startswith("smp4_")
    )
    discover_smp4 = include_smp4 or (
        bool(test_name) and arch == "riscv" and base.startswith("smp4_")
    )

    if args.list:
        tests = _discover_tests(
            kind, arch, include_smp=include_smp, include_smp4=include_smp4
        )
        if kind == "integ":
            tests = _filter_integ_tests(tests, arch)
        print(f"{kind} tests ({len(tests)}) [ARCH={arch}]:")
        for name in tests:
            print(f"  {name}")
        return

    if test_name:
        available = _discover_tests(
            kind, arch, include_smp=discover_smp, include_smp4=discover_smp4
        )
        if kind == "integ":
            available = _filter_integ_tests(available, arch)
        if test_name not in available:
            sys.exit(
                f"error: unknown {kind} test '{test_name}'.\n"
                f"Available: {', '.join(available)}"
            )
        print(f"Running {kind} test: {test_name} (ARCH={arch})")
        inner = _run_one_shell(kind, test_name, arch, make_extra)
        if json_dir:
            shell_cmd = (
                f"{CONTAINER_PATH}; "
                f"if ( {inner} ); then "
                f"{_json_record(kind, test_name, json_arch, json_board, 'PASS')}; "
                f"else "
                f"{_json_record(kind, test_name, json_arch, json_board, 'FAIL')}; "
                f"exit 1; fi"
            )
        else:
            shell_cmd = inner
        cmd = _base_docker_cmd(interactive=False)
        if json_dir:
            cmd += ["--volume", f"{Path(json_dir).resolve()}:/test-results"]
            cmd += docker_env
        cmd += [IMAGE_NAME, "/bin/bash", "-c", shell_cmd]
        os.execvp("docker", cmd)
    else:
        tests = _discover_tests(
            kind, arch, include_smp=include_smp, include_smp4=include_smp4
        )
        if kind == "integ":
            tests = _filter_integ_tests(tests, arch)
        if not tests:
            sys.exit(f"No {kind} tests found under {TESTS_DIR}")
        print(f"Running {len(tests)} {kind} test(s) [ARCH={arch}]: {', '.join(tests)}")
        shell_cmd = _run_all_shell(
            kind, tests, arch, make_extra, json_board,
            result_arch=json_arch,
        )
        cmd = _base_docker_cmd(interactive=False)
        if json_dir:
            cmd += ["--volume", f"{Path(json_dir).resolve()}:/test-results"]
            cmd += docker_env
        cmd += [IMAGE_NAME, "/bin/bash", "-c", shell_cmd]
        os.execvp("docker", cmd)


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    epilog = """
examples:
  python3 tools/dev.py
  python3 tools/dev.py --rebuild
  python3 tools/dev.py components list
  python3 tools/dev.py components status
  python3 tools/dev.py components enable hello_world
  python3 tools/dev.py components disable hello_world
  python3 tools/dev.py build
  python3 tools/dev.py build --clean
  python3 tools/dev.py build --no-components --component hello_world
  python3 tools/dev.py build --no-components --component ping_pong
  python3 tools/dev.py build --no-components
  python3 tools/dev.py build qemu
  python3 tools/dev.py build --board boards/qemu_riscv_virt
  python3 tools/dev.py build qemu --board boards/qemu_riscv_virt
  python3 tools/dev.py build qemu --board boards/qemu_mps2_an500
  python3 tools/dev.py build qemu --board boards/qemu_mps2_an505
  python3 tools/dev.py build --kernel --board boards/qemu_tc3xx
  python3 tools/dev.py build --kernel --board /path/to/external_board
  python3 tools/dev.py tests unit
  python3 tools/dev.py tests e2e
  python3 tools/dev.py tests e2e --board boards/qemu_riscv_virt
  python3 tools/dev.py tests e2e --board boards/qemu_riscv_virt --include-smp
  python3 tools/dev.py tests e2e --test sdk_suite/abi_smoke
  python3 tools/dev.py tests e2e --list
  python3 tools/dev.py tests integ --test sdk_suite/arch_whitebox/ctx_early_tricore
  python3 tools/dev.py killall
"""
    parser = argparse.ArgumentParser(
        description=(
            "ulmk multi-arch dev container — build, components, tests, QEMU"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog,
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Force a full Docker image rebuild before entering the shell",
    )

    subparsers = parser.add_subparsers(dest="command")

    build_p = subparsers.add_parser(
        "build",
        help="Build the kernel ELF for a target board",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Compile the kernel with CMake/Ninja.\n\n"
            "  build           configure + compile the demo ELF\n"
            "  build qemu      compile (if needed) then launch QEMU\n"
            "                  uses /opt/qemu-tricore/bin on TriCore,\n"
            "                  qemu-system-riscv32 on RISC-V and\n"
            "                  qemu-system-arm on ARM Cortex-M\n"
            "  build --kernel --board PATH\n"
            "                  emit a distributable SDK for external toolchains:\n"
            "                    ulmk/lib/ulmk_kernel_<arch>_<board>_gcc.a\n"
            "                    ulmk/lib/ulmk_board_<arch>_<board>_gcc.a\n"
            "                    ulmk/linker/linker_<arch>_<board>_gcc.ld\n"
            "                    ulmk/include/{ulmk,board}/*.h"
        ),
    )
    build_p.add_argument(
        "action",
        nargs="?",
        choices=["qemu"],
        default=None,
        help="'qemu' — build (if needed) then run in QEMU",
    )
    build_p.add_argument(
        "--board",
        metavar="PATH",
        default=None,
        help="Board directory with board.cmake (default: boards/qemu_tc3xx)",
    )
    build_p.add_argument(
        "--kernel",
        action="store_true",
        help="Build a distributable SDK (two static libs + processed linker "
             "script + public headers) instead of a demo ELF; requires --board",
    )
    build_p.add_argument(
        "--clean",
        action="store_true",
        help="Remove previous build artefacts before compiling",
    )
    build_p.add_argument(
        "--optimize-size",
        action="store_true",
        help="Compile kernel/arch with -Os instead of the default -Ofast",
    )
    build_p.add_argument(
        "--enable-smp",
        action="store_true",
        help="Build with ULMK_CONFIG_ENABLE_SMP=1 (requires board NUM_CPU>1)",
    )

    build_p.add_argument(
        "--component",
        metavar="NAME",
        dest="components",
        action="append",
        default=None,
        help="Enable a component for this build (repeatable; overrides .conf)",
    )
    build_p.add_argument(
        "--no-components",
        action="store_true",
        help="Ignore .ulmk/components.conf; only --component flags apply",
    )

    comp_p = subparsers.add_parser(
        "components",
        help="Discover and enable/disable kernel components",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Components live under components/, <board>/components/, and\n"
            "../ulmk_apps/.  All are OFF by default; enable them explicitly.\n"
            "Pass --board so board-local components are discoverable.\n\n"
            "  components list --board PATH\n"
            "  components status --board PATH\n"
            "  components enable --board PATH NAME ...\n"
            "  components disable --board PATH NAME ..."
        ),
    )
    comp_board = argparse.ArgumentParser(add_help=False)
    comp_board.add_argument(
        "--board",
        metavar="PATH",
        default=None,
        help="Board directory (default: boards/qemu_tc3xx); scans PATH/components/",
    )
    comp_sub = comp_p.add_subparsers(dest="components_action", required=True)
    comp_sub.add_parser(
        "list", parents=[comp_board], help="List all discoverable components"
    )
    comp_sub.add_parser(
        "status", parents=[comp_board], help="Show default vs configured state"
    )
    en_p = comp_sub.add_parser(
        "enable", parents=[comp_board], help="Enable components (saved to .conf)"
    )
    en_p.add_argument("names", nargs="+", metavar="NAME")
    dis_p = comp_sub.add_parser(
        "disable", parents=[comp_board], help="Disable components (saved to .conf)"
    )
    dis_p.add_argument("names", nargs="+", metavar="NAME")

    subparsers.add_parser(
        "killall",
        help="Kill all running dev containers (useful when QEMU hangs)",
    )

    tests_p = subparsers.add_parser(
        "tests",
        help="Build and run tests inside the container",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Run unit, integration or end-to-end tests inside the container.\n\n"
            "  tests unit              host-compiled Unity tests\n"
            "  tests integ             QEMU integration tests\n"
            "  tests e2e               SDK consumer end-to-end tests (UP)\n"
            "  tests e2e --include-smp RISC-V: UP suite + smp_* cases\n"
            "  tests e2e --include-smp4 RISC-V 4-hart: smp_* + smp4_* only\n"
            "  tests <kind> --list     show available suites\n"
            "  tests <kind> --test NAME run one suite\n"
            "  tests <kind> --board PATH  select architecture via board"
        ),
    )
    tests_p.add_argument(
        "kind",
        choices=["unit", "integ", "e2e"],
        help="Test suite type",
    )
    tests_p.add_argument(
        "--board",
        metavar="PATH",
        default=None,
        help="Board for integ/e2e tests (default: boards/qemu_tc3xx)",
    )
    tests_p.add_argument(
        "--test",
        metavar="NAME",
        default=None,
        help="Run a single test by directory name",
    )
    tests_p.add_argument(
        "--list",
        action="store_true",
        help="List available tests of the given kind and exit",
    )
    tests_p.add_argument(
        "--include-smp",
        action="store_true",
        help="e2e only (RISC-V): also run sdk_suite/smp_* multi-hart cases",
    )
    tests_p.add_argument(
        "--include-smp4",
        action="store_true",
        help=(
            "e2e only (RISC-V smp4 board): run smp_* + smp4_* only "
            "(no full UP suite)"
        ),
    )
    tests_p.add_argument(
        "--json-dir",
        metavar="DIR",
        default=None,
        help="Append per-case JSONL results under DIR (for CI report)",
    )

    return parser.parse_args()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    args = _parse_args()
    _check_docker()

    if args.command == "killall":
        _killall()
        return

    if args.rebuild or not _image_exists():
        try:
            _build_image()
        except subprocess.CalledProcessError:
            sys.exit("Docker image build failed.")

    if args.command == "build":
        _run_build(args)
    elif args.command == "components":
        _run_components(args)
    elif args.command == "tests":
        _run_tests(args)
    else:
        _run_container()


if __name__ == "__main__":
    main()
