# C29/F29H85x Feasibility Gate Evidence

**Branch:** `plan/c29f-port`  
**Date:** 2026-07-23  
**Probe:** XDS110 `CL850001`  
**Toolchain:** TI C29 Clang `2.0.0.STS` (`c29-ti-none-eabi`)

## Gate A — compiler, linker, freestanding ABI

| Check | Result |
|-------|--------|
| ILP32 (`sizeof(void*)/int/long/size_t == 4`) | PASS (`_Static_assert`) |
| ELF32 little-endian Machine 92 | PASS (`c29readelf`) |
| Integrated asm via `c29clang` | PASS |
| Freestanding `-nostdlib` + TI `.cmd` | PASS |
| No TI CRT / no libc in bare image | PASS |
| `ATOMIC.MEM` / `ATOMIC.END` for CAS | PASS (no `__atomic_*` helper) |
| CLZ via `__builtin_c29_i32_clzeros_d` | PASS |
| Host CMake 3.22.5 vs TIClang 3.29 | WORKAROUND: custom toolchain |

## Gate B — context / INT exit

| Check | Result |
|-------|--------|
| Full A/D/M frame (264 B) + `RETI.INT` veneer | PASS (code) |
| First launch via `RET` | PASS (root prints) |
| Idle on runtime Link + `ENINT`/`IDLE` | PASS (code) |
| PIPE `MEM_INIT` + vector @ `+0x5000` | PASS (syscall INT works) |
| Deferred coop switch while INT active | PASS (required; direct `RET` blocked further INTs) |
| CPUTIMER2 periodic wake of sleeper | **PARTIAL** — ISR hits during boot (`g_c29_tick_hits>0`); sleep after `RETI` to idle does not yet resume. Banner HIL uses `yield` heartbeat. |

## Gate C — INTSP / syscall doorbell

| Check | Result |
|-------|--------|
| INTSP = Stack2 (TI `Interrupt_initModule`) | PASS |
| INT_SW2 force doorbell @ `0x300223F8` | PASS (syscalls return) |
| `SUP_IGN_INTE_EN` + `GLOBAL_EN` | PASS |
| Nesting outermost-only schedule | PASS (`g_c29_int_depth`) |

## Gate D — SSU isolation

| Check | Result |
|-------|--------|
| Dynamic APR program/disable API | CODE READY (`mpu.c`) |
| `addr_permitted` shadow | PASS (logic) |
| Link2 override clear on SSUMODE2 | CODE READY (`ulmk_arch_mpu_enable`) |
| SECCFG MODE2 blob (CRC + necessary APRs) | **HOST PASS** (`tools/c29_seccfg_gen.py` + `seccfgChecker.py` Error count=0; board `seccfg/seccfg_cpu*.c`) |
| Packaging strips NonMain by default | **PASS** (`package-seccfg.sh`; `ULMK_C29_SECCFG_COMMIT=1` opt-in) |
| Negative isolation HIL (MODE1) | **PASS SKIP** (`hil-ssu-neg.sh` → `C29SSU_SKIP` / mode=0x30) |
| NonMain SECCFG program (silicon) | **PARTIAL / FRAGILE** — first MODE2 write OK (`SECCFG+0x7F8=0x0C` via loadti+`dss-flash-norest`). XRSn proves flash boot again under MODE1. Later `0x30→0x0C` needs NonMain erase (NOR 0→1); Entire Flash erase left APR area `0xFF…FE` but **program no longer sticks** (plugin reports Full load; dump still blank head + residual `MODE=0x30`). TI FAQ: disable verify for SECCFG; still BAD with `VerifyAfterProgramLoad=No verification`. |
| Negative MMIO isolation HIL (MODE2) | **BLOCKED** — XRSn required for SECCFG apply; MODE2 re-program currently fails after erase. RAM HIL still PASS (`C29SSU_SKIP`). |
| Fault recovery on CPU1/2/3 | **DEFERRED** — NMI → `ulmk_kern_trap_recoverable` prints kill then loops; full continue-on-idle not proven |

## Gate E — atomics / IPI / DLB

| Check | Result |
|-------|--------|
| CAS/add via atomic builtins | PASS (Gate A) |
| DLB clear all three CPU bits | PASS (`ulmk_arch_init` + board_init) |
| Directed IPC FLAG0 IPI (6 pairs) | CODE READY (`smp.c`); smoke sends IPIs after mask=0x7 |
| Three-core atomic stress HIL | **DEFERRED** — needs longer SMP stress |

## Gate F — one-artifact multicore

| Check | Result |
|-------|--------|
| CPU2 @ LPA1 `0x20108000` / CPU3 @ CPA0 `0x20110000` stubs | PASS (`c29nm` / map) |
| SSU reset vector release | PASS — `ISR1.PROT`, handshake in SHARED_RAM `0x200F8000`, `DEF_NMI=RST_VECT` (GEL) |
| Split-lock before CPU2 | PASS |
| Ready mask `0x7` HIL | **PASS** (`hil-smp-smoke.sh` / `c29_smp_smoke:CORE_READY mask=0x7`) |
| Flash SMP POR (BANKMODE0) | **PASS** (`hil-smp-flash.sh`: DSLite Main + contiguous stubs, DSS reset, `smp ready mask=0x7` / `CORE_READY mask=0x7`) |

## Gate G — HIL control

| Check | Result |
|-------|--------|
| Probe serial `CL850001`, UART `if00` only | PASS |
| Board lock + loadti timeout | PASS |
| Reject flash without BANKMODE/lifecycle | PASS (`hil-flash-por.sh`) |
| SECCFG packaging script | **PASS** host path — MODE2 blobs + strip-by-default; NonMain commit opt-in |
| Flash autonomous after JTAG sysreset (no reload, no session) | **PASS** (`hil-flash-por.sh` + `dss-reset-run.js`; UP `C29SLEEP` / `ulmk:`; INTOSC2 UART) |
| Flash UART capture before DSS reset | **PASS** — open `cat` on UART before `dss-reset-run` (boot banners otherwise lost within ~100ms) |
| Pre-flash halt before DSLite | **PASS** — `dss-preflash-halt.js` avoids `wr_pll.alg` timeout on free-running XIP idle |
| Physical button / XRSn POR | **DEFERRED** (nSRST via `xds110reset` does not reboot C29 on LAUNCHXL) |

## Decisions locked

- Freestanding link uses `-nostdlib`.
- PIPE vectors at `PIPE_BASE+0x5000+n*4`; enable in `INT_CTL_L`; force/clear in `INT_CTL_H`.
- Coop switches inside INT are deferred to `RETI.INT`.
- Secondary payloads live in the single CPU1-loadable `ulmk.out`.
