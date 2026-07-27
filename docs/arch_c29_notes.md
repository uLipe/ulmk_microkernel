# C29 architecture notes (ULMK)

See `docs/c29f_port_plan.md` for the full design and
`docs/gates/c29f_gate_evidence.md` for silicon results.

## Quick facts

- ILP32 / ELF32, stack grows **up** (`ULMK_ARCH_STACK_GROWS_UP`).
- Context frame: 264 bytes (A14/RPC/DSTS/ESTS + XA/XD/XM).
- Switches only at INT/syscall exit via deferred `RETI.INT` staging.
- PIPE vectors at `PIPE_BASE + 0x5000 + n*4`; enable `INT_CTL_L`, force/clear `INT_CTL_H`.
- Must `MEM_INIT` PIPE before programming vectors; INTSP = Stack2.
- Secondary stubs (RAM): CPU2 LPA1 `0x20108000`, CPU3 CPA0 `0x20110000`
  (CPU1 may spill into CPA1 `0x20118000`; CPU2 cannot fetch CPA).
- Secondary reset stubs must start with `ISR1.PROT||ISR2.PROT`; handshake
  words in SHARED_RAM (`0x200F8000`/`04`).  Point `DEF_NMI_VECT` at the same
  address as `RST_VECT` during bring-up (GEL style) — a distinct `+0x40` NMI
  veneer hits `NMI_ISR_ENTRY_ERR` before the store.
- Flash SMP (BANKMODE0): stubs linked after CPU1 `.text`/`.data` as
  `.cpu2_stub`/`.cpu3_stub` (only when `ULMK_CONFIG_ENABLE_SMP=1`); CPU1
  copies into LPA1/CPA0 before release.  `package-seccfg` keeps stubs out of
  the cert BIN and programs them only with `ULMK_C29_FLASH_SECONDARY=1`.
  HIL: open UART *before* DSS reset; `dss-preflash-halt.js` before DSLite
  (idle XIP → `wr_pll.alg` timeout otherwise).
- Clear `MEM_DLB_CONFIG` CPU1/2/3 bits before shared RAM / SMP (SPRZ569E).

## Open follow-ups

- SSUMODE2 NonMain flash (`ULMK_C29_SECCFG_COMMIT=1` + `hil-ssu-mode2.sh`) —
  host CRC/checker OK; silicon commit still pending (brick risk).
- Full recoverable fault continue-after-kill on NMI (Gate D).
- Full `C29SMP_PASS` (IPI/sleep after ready) when tick path is complete on flash.
- Physical XRSn POR (nSRST via `xds110reset` does not reboot C29 on LAUNCHXL).
