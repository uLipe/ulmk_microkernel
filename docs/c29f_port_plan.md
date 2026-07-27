# C29x/F29H85x Architecture Port Plan

**Status:** implementation in progress on `plan/c29f-port`  
**Planning branch:** `plan/c29f-port` in `ulmk`, `ulmk_boards`, and
`ulmk_apps`  
**Reference target:** TI LAUNCHXL-F29H85X with F29H850TU9  
**Validation probe:** XDS110 serial `CL850001`  
**Gate evidence:** `docs/gates/c29f_gate_evidence.md`  
**BSP:** `ulmk_boards/launchxl_f29h85x/README.md`

## 1. Goal

Add a production-quality `c29` architecture port and a minimal
`launchxl_f29h85x` BSP, including three-core SMP, SSU isolation, PIPE
interrupts, CPU timers, IPC reschedule IPIs, and command-line silicon
validation through Code Composer Studio.

The finished build must retain the ulmk single-artifact model. Internal
per-core images are allowed during the build, but the shipped result must be
one CPU1-loadable `ulmk.out` containing the CPU2 and CPU3 payloads.

The C29 CPU has 64-bit execution resources and can issue up to eight
instructions in parallel, but the C EABI is ILP32: pointers, `int`, `long`,
`size_t`, and `ptrdiff_t` are 32 bits. The port must therefore keep the
kernel's existing 32-bit address and syscall ABI assumptions.

## 2. Non-negotiable kernel contracts

- Context switches occur only at low-priority interrupt exit or syscall
  interrupt exit through `ulmk_kern_sched_dispatch()`.
- Idle only waits for an interrupt. It must not poll IPC state, drain a
  mailbox, or schedule.
- A remote wake must generate a real interrupt on the destination CPU.
- Kernel code depends on C29 only through `ulmk_arch.h`; C29 code must not
  include kernel-internal headers.
- All thread state needed after an interrupt must live in the thread's saved
  frame. A shared kernel interrupt stack may not retain a blocked thread's
  continuation.
- User and driver code must not gain kernel memory, PIPE configuration, SSU
  configuration, or unrestricted peripheral access by branching into a
  privileged section.
- SMP atomics must be implemented by verified hardware exclusion. Falling
  back to interrupt masking or compiler runtime locks is not acceptable.
- The C29 build must remain freestanding and must not acquire a dependency on
  the TI C runtime startup sequence, heap, constructors, or `main()`.

## 3. Authoritative references

References must be rechecked when implementation starts because the device
documentation and SDK are still changing.

- C29x CPU Reference Guide, `SPRUIY2A`, revision A.
- F29H85x and F29P58x Technical Reference Manual, `SPRUJ79`, revision B,
  published July 2026.
- F29H85x/F29P58x/F29P32x datasheet, revision D, published May 2026.
- F29H85x/F29P58x/F29P32x silicon errata, `SPRZ569`, revision E, published
  July 2026.
- LAUNCHXL-F29H85X user's guide, `SPRUJE5B`.
- Implementing Run-Time Safety and Security With the C29x SSU,
  `SPRADK2A`.
- TI C29x Clang Compiler Tools User's Guide, especially the EABI,
  intrinsics, assembler, and linker chapters.
- F29H85x SDK System Security Configuration and multicore examples.
- Local TI FreeRTOS C29 port, used as an ABI and context-frame reference,
  not copied into ulmk:
  `source/kernel/freertos/Source/portable/CCS/C2000_C29x`.

The implementation must audit the connected silicon revision against
`SPRZ569E` before clocks, reset behavior, flash prefetch, SSU, PIPE, or fault
recovery are finalized.

The public CPU guide does not replace the complete C29 instruction-set
material referenced by the compiler documentation. Obtain the complete
material through TI before production assembly is approved, particularly for
protected return, NMI/fault return, atomic windows, and status-register side
effects.

## 4. Facts already verified

### 4.1 Local tools

- CCS: `/home/ulipe/ti/ccs2040`.
- Compiler: TI C29 Clang `2.0.0.STS`.
- Compiler target: `c29-ti-none-eabi`.
- Linker: `c29lnk` `2.0.0.STS`; assembly is integrated into `c29clang`
  (`-x assembler-with-cpp`), with no separate `c29as`.
- Installed inspection/conversion tools include `c29readelf`, `c29objdump`,
  `c29nm`, `c29size`, `c29ofd`, `c29objcopy`, and `c29strip`.
- Local SDK: F29H85x SDK `1.02.01.00`.
- SysConfig: `1.26.0+4407`.
- Host CMake: `3.22.5`; native TIClang support requires CMake 3.29 or newer.
- Headless tools are present:
  - `ccs_base/scripting/bin/dss.sh`
  - `ccs_base/scripting/examples/loadti/loadti.sh`
  - `ccs_base/common/uscif/xds110/xdsdfu`
  - `ccs_base/common/uscif/xds110/xds110reset`

The current F29 SDK release, `26.00.00` STS, is newer than the local
installation and uses CCS 20.5, C29 Clang 2.2 LTS, and SysConfig 1.27. The
implementation must support configurable installation roots and must test at
least the local toolchain plus the current LTS compiler before release.

The compiler driver enables function/data sections and links TI libc, libc++,
libsys, and compiler-rt by default. `-nostartfiles` is ignored by the installed
driver; the freestanding backend must use a verified `-nostdlib` or
`-nodefaultlibs` flow and explicitly add only required compiler helpers.

### 4.2 ABI

Compiler predefined macros confirmed:

- `__TI_EABI__ == 1`
- `__SIZEOF_POINTER__ == 4`
- `__SIZEOF_INT__ == 4`
- `__SIZEOF_LONG__ == 4`
- `__SIZEOF_SIZE_T__ == 4`
- little-endian ELF output
- 8-byte stack alignment

The compiler can use M registers even in code that contains no explicit
floating-point operation. Every schedulable context must therefore preserve
all A, D, and M working registers.

### 4.3 Board and debugger

The connected XDS110 was verified in runtime/standard mode:

- USB VID:PID `0451:bef3`
- firmware `3.0.0.41`
- serial `CL850001`
- application UART:
  `/dev/serial/by-id/usb-Texas_Instruments_XDS110__03.00.00.41__Embed_with_CMSIS-DAP_CL850001-if00`
- auxiliary port, not for application logs:
  the corresponding `if03` device
- UARTA is routed to the XDS110 by default through GPIO42 TX and GPIO43 RX
- LED4 is GPIO19 and LED5 is GPIO62; both are active-low
- boot switch S3 selects flash with GPIO72/GPIO84 both high

The SDK LAUNCHXL blinky example was compiled with the installed compiler and
loaded to the board with `loadti.sh` and
`examples/device_support/targetconfigs/F29H850TU9.ccxml`.

This proves compiler, linker, XDS110 discovery, target connection, program
load, and run control. It does not yet prove ulmk context switching, SSU
isolation, flash boot, or SMP.

The older SDK makefile expects CCS 20.3 and SysConfig 1.25. The local
installation required explicit `CCS_PATH` and `SYSCFG_ROOT` overrides.
Its dummy-certificate post-build utility is not executable in the current
installation. RAM loading is unaffected; the production flash pipeline must
fix this without mutating a user's SDK installation.

`SPRZ569E` contains a mandatory SMP workaround: the RAM Data Line Buffer is
enabled by default and can return stale data when CPUs concurrently access a
shared RAM location. `MEM_DLB_CONFIG` is global and has enable bits for the
CPU1, CPU2, and CPU3 initiators. CPU1 must clear all three bits before shared
RAM is reinitialized, tested, or first used as shared state by ulmk, and
before secondaries are released. Boot ROM activity before CPU1 entry is
outside this software ordering claim.

## 5. Proposed architecture

The following choices are provisional until the feasibility gates in
Section 6 pass.

### 5.1 Execution and context model

Use Supervisor low-priority `INT` for every kernel-visible scheduling event:

- syscall doorbell
- scheduler tick
- device IRQ delivery
- IPC reschedule IPI
- voluntary yield and thread exit

Reserve `RTINT` and NMI for bounded non-scheduling work, fault capture, or
panic paths. Their protected hardware stacks are not used to retain thread
continuations.

C29 `INT` is not a privilege transition: the CPU accepts it only when
`CURRSP == INTSP`, and its vector must execute in that same SSU Stack. The
candidate layout therefore uses:

- one scheduler SSU Stack and one common unprivileged runtime Link for all
  schedulable user/driver code and the INT veneer
- each CPU's local Link 2 as the kernel Link in a separate protected Stack
- dynamic APRs to isolate the currently running thread's data, stack, grants,
  and driver MMIO

The first port does not assign stronger static authority to a driver Link:
explicit branches between Links in one Stack can change the active Link.
Driver privilege comes from capabilities plus dynamic APRs installed only for
the current thread. Gate D must reject any design where branching into another
component's code acquires static MMIO or memory authority.

The INT veneer has no kernel APR access. Sharing its Link and Stack with user code
means user code can branch to it, so every privileged entry validates that an
INT is active and that the frame/channel is a legitimate kernel source.

On `INT` entry:

1. The veneer saves a complete thread frame on the interrupted thread stack.
2. It records the resulting A15 in `ulmk_arch_ctx_t`.
3. It invokes the exact protected kernel dispatch entry with `CALL.PROT`.
4. Protected entry switches to the per-CPU kernel Stack/A15, validates the
   saved frame and source, and runs the C dispatcher.
5. `ulmk_kern_sched_dispatch()` only stages the selected context.
6. Kernel C returns through `RET.PROT`; the mandatory first caller packet is
   `EXIT.PROT`, which restores the scheduler-Stack A15 and completes the
   protected return.
7. Only after `EXIT.PROT`, the unprivileged INT epilogue applies the staged
   selection, restores that thread's A15 and full frame, then executes
   `RETI.INT`.

No protected-call frame or kernel stack continuation survives a context
switch. Initial launch, blocked syscall, voluntary yield, and IRQ preemption
must all converge on this same epilogue model.

Dynamic APR handoff is part of the context-switch proof. The old stack is
needed through `EXIT.PROT`, while the selected stack must be writable before
its frame is restored. A candidate two-phase epilogue maps both stacks only
while executing trusted veneer code, switches A15, then uses a validated short
protected finalizer to remove the old mapping before any selected-thread
instruction executes. Gate C/D must prove this window is interrupt-safe and
unreachable as an authority gain from ordinary user flow. If no bounded
handoff can remove the old stack before `RETI.INT`, the isolation design is
blocked.

The initial implementation saves all restorable thread state on every
kernel-visible interrupt. Based on the TI reference frame, the complete frame
is expected to consume 272 bytes including the hardware INT words. Hardware
transition fields such as `DSTS.INTS`, ISR priority, `CLINK`, and `RLINK` are
validated at each phase rather than compared as invariant thread sentinels.
The exact frame must be confirmed from generated assembly and measured on
silicon.

The candidate `ulmk_arch_ctx_t` contains only a 32-bit saved stack pointer,
but Gate B must prove that no other resumable state lives outside the saved
frame before this layout is frozen. A synthetic frame starts a new thread and
returns to a trampoline that calls `ulmk_thread_exit()` if the entry function
returns.

The initial launch is a separate one-way transition from boot Link 2/kernel
Stack into the common runtime Link/scheduler Stack. Gate B must prove the
exact assembly sequence establishes `CURRSP == INTSP`, the intended PSP/A15,
and a synthetic return frame without leaving a protected-call or boot-stack
continuation.

### 5.2 Protected INT bridge and syscall doorbell

C29 has no conventional unprivileged syscall instruction. A normal
`CALL.PROT` cannot itself be a blocking syscall continuation because the
protected-call stack is CPU-local and LIFO; arbitrary thread blocking and
wake order would violate that model.

The candidate syscall ABI is:

1. Userspace normalizes the syscall number and four 32-bit arguments into
   fixed registers and requests one reserved Supervisor software INT.
2. Every PIPE channel remains owned by the local kernel Link 2. Gate C must
   prove that the runtime Link's API permission for the doorbell permits only
   force—not read, clear, enable, disable, or force of any tick/IPI/device
   source. If the hardware API permission is broader, a tiny non-blocking
   protected stub validates and forces the doorbell under Link 2, then
   completes `RET.PROT`/`EXIT.PROT` before the INT can be serviced.
3. The normal INT veneer saves all arguments before any protected call.
4. The protected kernel entry begins with the required
   `ENTRY1.PROT || ENTRY2.PROT` packet, validates `DSTS.INTS`, source identity,
   frame bounds/alignment, and ownership, then receives only a saved-frame
   pointer through an explicitly preserved argument register.
5. Kernel C writes the return value into the saved result-register slot and
   stages any context switch.
6. `RET.PROT` and the caller's mandatory `EXIT.PROT` packet complete before
   the INT epilogue selects and resumes a thread.

Protected calls and returns reset the C29 atomic counter, so an atomic window
cannot be used to bridge a protected return. Exact `PRESERVE`, register-zeroing,
`ENTRY`/`EXIT`, PIPE owner/API Link, and Supervisor-INT behavior must be
confirmed from compiler output and silicon.

“Supervisor INT” is not a privilege class or arbitrary per-channel mode. The
candidate requires `PRI_LEVEL == INT_RTINT_THRESHOLD` together with
`SUP_IGN_INTE_EN=1`, allowing the event through even when user code clears
`DSTS.INTE`. Tick, syscall, IPC IPI, and any device source required for
scheduling progress use that exact configuration.

These sources share the threshold priority and may enter while another INT is
active despite ordinary INT masking. Gate C must characterize ties and
nesting, then implement a bounded per-CPU outermost guard: nested Supervisor
arrivals may save/ack/coalesce work but never independently call the
scheduler. Before the outer epilogue applies a staged frame it drains the
coalesced state and reruns or revalidates dispatch, so a nested wake cannot
make the selection stale. Gate C must also determine whether repeated user
`ATOMIC.REG` can indefinitely defer Supervisor INT; if so, the supported
threat model and mitigation must be explicit.

### 5.3 SSU model

Use SSU runtime isolation as the C29 MPU backend:

- Link 2 is the secure-root context of each C29 CPU. CPU1 coordinates boot,
  but this does not imply unrestricted global access.
- Each CPU programs only the SSU/APR resources that the device ownership
  tables permit. Cross-CPU configuration is performed by the owning core or
  by an explicitly documented CPU1 boot path.
- Kernel text, data, PIPE configuration, SSU configuration, and kernel stacks
  are kernel-only APRs.
- User/driver executable text and the INT veneer use the common non-kernel
  runtime Link in the scheduler Stack selected by `INTSP`.
- Driver peripheral reach is granted by APR permissions, not by exposing
  secure-root access.
- One or more uncommitted APR slots are reprogrammed on context switch to
  represent the current thread's stack, data, shared mappings, and grants.
- Static linker sections define kernel, the veneer, and only genuinely common
  shared-memory APRs. Per-component executable text and driver MMIO are
  dynamic current-thread mappings.
- `ulmk_arch_mpu_addr_permitted()` validates against the current TCB region
  list; it does not infer permission merely from the current Link.

All CPUs have their own Link/Stack/AP configuration. The same logical layout
must be installed on CPU1, CPU2, and CPU3 before userspace starts.

Normal APR placement is 4 KiB granular, so the C29 linker cannot reuse the
64-byte TriCore domain alignment. Budget the 64 APRs per CPU explicitly.
Static Link-to-Stack assignments are fixed at boot; only Link 2 may update
APR address, executable-Link, and permission fields at runtime. Update
dynamic slots only while executing from static kernel code/stack: disable the
slot, program and validate the complete descriptor, then enable it. No
intermediate state may overlap static kernel or PIPE/SSU ranges.

SSU mode 1 is sufficient only for mechanical CPU/context bring-up; APR
enforcement is disabled there. Gate D requires a minimal CRC-valid `SECCFG`
and reboot into SSU mode 2 after a tested recovery procedure exists.
`COMMIT`, SSU mode 3, debug-zone locking, and irreversible security settings
remain deferred. Link 2 AP override is restricted to controlled boot
configuration and is disabled before the first user thread.

SSU denial does not guarantee that every peripheral read lacks side effects;
some accesses can reach read-clear/FIFO logic even when the return data is
blocked. The BSP must classify such peripherals, and Gate D must not claim
complete MMIO containment solely from APR read denial.

### 5.4 PIPE and IRQ mapping

Treat the F29 PIPE channel number as ulmk's logical `srpn`.

- The board maps peripheral events to PIPE channel numbers.
- `ulmk_arch_irq_src_configure()` keeps every channel's OWNER_LINK at local
  kernel Link 2, applies fixed arch-private API-Link rules, and programs
  priority, context, and CPU-local PIPE state. SSU concepts do not leak into
  the generic kernel API.
- Userspace never receives access to PIPE configuration registers.
- The runtime Link receives no operational access to tick, IPI, or device
  channels. Its syscall-doorbell access is accepted only after Gate C proves
  force-only semantics; otherwise the short protected force stub is mandatory.
- Every scheduling source is a Supervisor INT targeting the scheduler Stack
  and unprivileged INT-veneer Link.
- Ack ordering is peripheral flag clear first, then PIPE flag clear when the
  source requires both.
- The tick and IPC IPI use reserved kernel channels that cannot be bound by
  userspace.
- `ulmk_irq_attach` starts disabled for the first protected bring-up. Its
  callback trampoline is implemented only after the base SSU and fault paths
  are proven.

The first port disables ordinary INT nesting. Threshold-priority Supervisor
arrivals that bypass `DSTS.INTE` follow the bounded nested path established by
Gate C. If a higher-priority event requires RTINT, it performs bounded
non-scheduling work and requests a Supervisor INT after returning. Only the
outermost INT epilogue may stage and apply a context switch.

### 5.5 Tick and idle

Use one CPU timer per active CPU, initially CPU timer 2 as in the TI FreeRTOS
port. Every CPU advances its own ulmk timing wheel.

The current generic idle thread is kernel-privileged, which would leave C29 on
the kernel Stack and prevent `INT` while `CURRSP != INTSP`. Add
`ULMK_ARCH_IDLE_ENTRY` and `ULMK_ARCH_IDLE_PRIVILEGE` hooks whose default
values preserve the existing ports. C29 selects `ulmk_arch_idle_entry`, a
user-privileged leaf assembly trampoline in the common runtime Link/scheduler
Stack; normal thread initialization then represents its stack as an explicit
dynamic region. The trampoline only enables interrupts and executes the
documented low-power wait instruction if IPC and timer events wake it
reliably; otherwise it uses the architecturally safe idle instruction sequence
proven by Gate B. It never polls an IPC flag or schedules.

### 5.6 Atomics

Do not use C11, `__atomic`, or `__sync` operations directly. The installed
compiler advertises zero always-lock-free sizes, and some builtins call
runtime helpers.

The candidate 32-bit CAS and fetch-add wrapper uses:

- `__builtin_c29_atomic_mem_enter()`
- one aligned shared-memory read/modify/write sequence
- `__builtin_c29_atomic_leave()`
- compiler `"memory"` barriers around the window

Before this mechanism is accepted, Gate E records the exact mnemonic,
instruction-packet count, memory operation, and termination sequence emitted
by every supported compiler. TI documentation uses both `ATOMIC.M`-family
terminology and compiler intrinsic names; the plan does not assume a specific
assembler spelling. No `__atomic_*` helper may remain in the image.

The public CPU guide and current TRM disagree on whether the relevant atomic
counter permits 64 or 256 instruction packets, while the no-argument memory
intrinsic does not expose that bound directly. Treat 64 as the conservative
architectural ceiling when auditing generated windows. If the compiler
sequence cannot be proven to complete within it, use reviewed assembly backed
by the complete ISA material or reject the mechanism.

Memory-controller exclusion does not by itself define a portable C
acquire/release model across different controllers. Release/acquire semantics
require an explicit TI architectural/compiler guarantee or a more restricted
IPC-based ordering contract. Stress tests are necessary evidence, not a
substitute for that contract. Atomic tests run without debugger halts because
a halt releases the hardware exclusion window; NMI behavior must also be
tested.

### 5.7 SMP boot and image layout

HSM/ROM is the initial boot authority. After it releases CPU1, CPU1
coordinates application memory setup and the secondary C29 cores.

For an SMP build:

1. Build internal CPU1, CPU2, and CPU3 images from the same source graph.
2. Place CPU1 code in LPA0 or CPU1 flash.
3. Place CPU2 code in LPA1; CPU2 has no direct flash execution path.
4. Place CPU3 code in CPA0 for RAM tests and its assigned flash region for
   flash builds.
5. Link the required CRC-valid CPU1, CPU2, and CPU3 SSU configuration sections
   at addresses derived from the selected BANKMODE.
6. Embed CPU2 and CPU3 payload sections in the final CPU1 `ulmk.out`; the
   flash profile also carries the CPU1/CPU3 boot certificate and metadata
   sections required by the current device lifecycle.
7. CPU1 switches CPU1/CPU2 to split mode at the documented point and waits at
   least 24 cycles before using CPU2 independently.
8. CPU1 clears all CPU1/CPU2/CPU3 DLB enable bits before ulmk performs
   RAM/ECC reinitialization, tests, or first shared use.
9. CPU1 initializes clocks, RAM/ECC, shared kernel state, and SSU.
10. CPU1 copies payloads when the selected boot mode requires it.
11. CPU1 configures boot and NMI vectors, releases CPU2 and CPU3, and waits
    for a shared ready mask.

Shared kernel data uses a fixed LDA address range visible to all CPUs. CPU1
initializes shared `.data`/`.bss` exactly once before releasing secondaries.
Per-CPU stacks and private state use separate linker sections.

The per-core program memories have different physical addresses. No code
pointer created on CPU1 may be sent to another core unchanged. Inventory every
cross-core code pointer, including thread entries, IRQ-attach callbacks,
function tables, and pointers stored in read-only data.

The preferred model assigns generated IDs to every cross-core-callable entry
and resolves the ID through a core-local table. C29 sets
`ULMK_ARCH_CTX_FABRICATE_ON_AFFINITY_CPU=1`, reusing the kernel's existing
deferred context-fabrication path. `ulmk_arch_ctx_init()` therefore runs on
the destination CPU. Generated source-address tables cover CPU1, CPU2, and
CPU3, mapping a raw entry from whichever CPU created the thread to an ID; a
core-local table maps that ID to the destination address. The arch does not
inspect kernel-internal TCB fields. The same resolver applies to
`ulmk_arch_start_secondary()` and every supported cross-core callback.
Gate F tests all nine creator-to-destination combinations. Offset translation
is allowed only if the build proves equal offsets for every exported symbol
and disables transformations such as LTO/ICF that can make layouts diverge.

If the device provides a documented CPU-local program alias suitable for all
three cores, that is preferable and must be tested before retaining software
translation.

LPAx totals 64 KiB, but the simultaneous reference RAM layout gives CPU2
LPA1 32 KiB while CPU1 uses LPA0; CPU3 similarly receives 32 KiB CPA0. These
32 KiB limits apply to the initial tri-core RAM profile. A flash profile may
allocate more of LPAx to CPU2 only when the memory allocator proves that CPU1
does not require the displaced region. Every profile fails at link time on
overflow. Common compiler options target the CPU1/CPU2-compatible 32-bit FPU
subset; CPU3-only FPU64 optimization is deferred.

A UP profile must explicitly choose between verified CPU1/CPU2 lockstep or
split CPU1 with secondaries held in reset. `LSEN=1` alone is not a safety
profile because the lockstep comparator has separate enable/configuration.
SMP always uses split mode, and returning to reset defaults requires the
documented reset class.

### 5.8 IPI

Use the F29 IPC blocks for directed reschedule interrupts.

- Reserve one channel/flag per CPU pair for the kernel.
- `ulmk_arch_send_ipi(cpu)` sets the destination's reserved IPC flag.
- The destination PIPE INT clears/acknowledges the IPC event, calls
  `ulmk_kern_ipi_from_isr()`, and dispatches at ISR exit.
- Validate all six directed paths: 1→2, 1→3, 2→1, 2→3, 3→1, and 3→2.
- Coalesce repeated kicks with the existing per-CPU ulmk pending state.

## 6. Feasibility gates

These are acceptance gates, not a demand to finish the whole port before
Phase 1. Phase 0 runs the small blocking probes from A, C, D, E, and G before
their mechanisms are frozen; the complete B, D, E, F, and G criteria close in
the implementation phase that owns them. A failed gate updates this plan
before dependent work expands.

### Gate A — compiler, linker, and freestanding ABI

Deliver:

- CMake 3.29+ TIClang smoke project.
- C, assembly, archive, link, map, ELF inspection, binary extraction, and size
  operations through the tools actually shipped with the selected C29
  compiler package.
- TI `.cmd` script containing ulmk-required boundary symbols.
- Compile-time ABI assertions.
- Link with no TI CRT startup, heap, C++ initialization, or unresolved atomic
  helper.
- Link trace for `uint64_t` add/subtract/compare/multiply/divide operations
  used by the timer wheel, with every required runtime helper identified.

Pass:

- one RAM `.out` loads and prints a deterministic banner
- the selected ELF inspector confirms C29 ELF/EABI
- the map contains every required ulmk linker symbol
- no implicit libc/libc++/libsys dependency remains

### Gate B — complete context and interrupt-exit switch

Deliver:

- full C29 context frame and synthetic initial frame
- same-Stack unprivileged INT veneer
- protected bridge that enters and leaves the per-CPU kernel Stack before the
  context handoff
- `INT` epilogue using a per-thread frame
- one-way first launch into the runtime Link/scheduler Stack
- runtime-Link idle trampoline and explicit idle-stack region
- cooperative yield plus timer preemption

Pass:

- sentinels survive in every A, D, and M register
- RPC, restorable DSTS/ESTS fields, flags, A15, automatic INT words, and
  8-byte alignment survive; `INTS`, ISR priority, `CLINK`, and `RLINK` match
  their expected transition state at every phase
- at least one million mixed cooperative/preemptive switches at optimized
  builds
- ordinary INT nesting remains disabled; nested Supervisor arrivals are
  bounded/coalesced; RTINT/NMI never schedule
- first launch, normal return-to-exit, block, wake, kill, and reuse pass
- first launch and idle both satisfy `CURRSP == INTSP` with no boot/protected
  frame residue; tick and IPI preempt idle
- no switch is performed from idle or ordinary kernel C code
- no kernel/protected stack frame survives the handoff
- a one-word `ulmk_arch_ctx_t` is accepted only after all resumable state is
  proven to reside in the thread frame

### Gate C — INTSP bridge and protected syscall entry

Deliver:

- one scheduler Stack satisfying `CURRSP == INTSP`
- unprivileged assembly INT veneer and exact protected kernel entry
- normalized four-argument syscall ABI
- dedicated Supervisor PIPE software interrupt
- proven force-only runtime API access or the short protected force fallback
- bounded old-stack/new-stack APR handoff with protected cleanup
- return-value injection into the saved frame

Pass:

- user code can force only the intended doorbell source
- user code cannot read, clear, enable, disable, or force tick/IPI/device
  channels through PIPE operational registers
- a direct branch/call into veneer or kernel code without active INT cannot
  gain kernel authority
- the first kernel packet and caller return packet satisfy
  `ENTRY1.PROT || ENTRY2.PROT` and `EXIT.PROT` rules
- the protected call returns before context selection is applied
- two or more threads may block in syscalls and wake in non-LIFO order
- malformed entry, branch, stack, and pointer tests fault only the caller
- back-to-back optimized syscalls preserve all declared clobbers
- `DISINT` cannot suppress syscall, tick, or IPI
- every Supervisor source uses the threshold-priority plus
  `SUP_IGN_INTE_EN` contract, and tie/nesting tests prove only the outermost
  epilogue schedules
- a nested wake after initial staging forces the outer epilogue to rerun or
  revalidate dispatch before applying the selected frame
- no selected-thread instruction executes while both old and new private
  stacks are mapped; direct calls to the cleanup entry cannot alter APRs
- repeated `ATOMIC.REG` behavior is measured and either mitigated or recorded
  as a threat-model limit

### Gate D — SSU isolation and recoverable faults

Deliver:

- linker-generated Link/Stack/AP layout
- per-thread dynamic APR programming
- fault decode and user-vs-kernel attribution
- exact allowlist for driver MMIO apertures

Pass:

- user read/write/execute attempts against kernel memory fault
- cross-thread stack/data access faults
- driver MMIO works only with an explicit mapped region
- branches among every candidate user/driver/veneer Link pair cannot acquire
  static authority; the common-runtime-Link design remains mandatory unless
  this is disproved
- representative unauthorized writes to SSU, PIPE, clock/reset, and
  peripheral control registers are denied or trapped
- registers that can latch or acknowledge a write before fault delivery are
  absent from the user/driver attack surface
- grants appear and disappear at the correct syscall boundaries
- repeated switches prove no previous-thread stack/APR remains reachable
- kernel, ISR, NMI, or ambiguous faults panic
- a user fault kills one thread and the rest of the system continues
- the faulting instruction is not re-executed after the thread is killed
- no interrupt, protected-call, or context frame leaks during recovery
- recovery is proven separately on CPU1, CPU2, and CPU3; CPU1 reset
  propagation is not accepted as CPU-local recovery

The public material routes protection faults through NMI/error aggregation,
whose protected frame is not an ordinary writable software frame. Gate D must
prove a supported return redirection or a bounded core-local recovery
mechanism. If hardware permits only device reset, stop and revise the product
fault contract before implementing the remaining isolation layer.

MMIO denial can be reported after a peripheral observed the transaction.
Therefore Gate D treats SSU primarily as a memory/code/stack boundary and
keeps system-control apertures permanently kernel-only. Driver MMIO uses
deny-by-default dynamic exact apertures. The port does not claim transactional
rollback for hostile MMIO writes.

### Gate E — three-core atomics and IPIs

Deliver:

- reviewed CAS, fetch-add, spinlock, and ordering primitives
- directed IPC IPI implementation
- shared LDA test area

Pass:

- no compiler atomic helper in the image
- the emitted atomic mnemonic/window/termination sequence is approved for
  every supported compiler
- all CPU1/CPU2/CPU3 enable bits in global `MEM_DLB_CONFIG` are confirmed
  clear before ulmk RAM reinitialization/test/first shared use and secondary
  release
- debugger halts are disabled during atomic stress
- three-core contended counter has no lost updates
- CAS winner tests never report two winners
- release/acquire message-passing stress reports no stale payload
- the TI architectural/compiler ordering contract is cited for every
  acquire/release primitive, or the primitive is rejected
- NMI injection does not violate the accepted exclusion contract
- all six directed IPI paths preempt a lower-priority destination thread
- remote wake works while the destination is idle
- long scheduler/pool stress has no deadlock or starvation

### Gate F — one-artifact multicore boot

Deliver:

- three internal core-specific linked images
- one final CPU1 `.out`
- CPU2/CPU3 payload packaging and boot
- CRC-valid per-core SSU configuration sections plus lifecycle-appropriate
  CPU1/CPU3 certificate and boot metadata packaging
- affinity-CPU context fabrication through the existing deferred-init
  contract
- generated cross-core entry IDs and per-core resolver tables
- build-time cross-core pointer inventory

Pass:

- one `loadti` invocation starts all three CPUs
- CPU1 reports ready mask `0x7`
- each CPU runs a pinned ulmk thread, tick, and IPI handler
- the initial CPU2 and CPU3 RAM payloads each remain within their 32 KiB
  profile budget
- no unresolved raw code pointer crosses a core boundary
- entry/callback resolution passes all nine creator-to-destination CPU
  combinations
- RAM mode, flash mode, reset, and repeated reload pass
- the one-artifact flow passes under SSUMODE2 with the selected BANKMODE
- flash mode boots without an attached debug session

### Gate G — repeatable HIL control

Deliver:

- CCS root/version discovery with no hard-coded home directory
- XDS110 serial selection
- DSS load, attach, run, halt, symbol, register, and memory operations
- `xds110reset` integration
- read-only capture of silicon revision, device lifecycle, active BANKMODE,
  and reset capabilities before any flash operation
- controlled power-cycle or manual POR procedure for reset-sensitive tests
- stable application-UART capture
- timeout, cleanup, and exclusive board locking

Pass:

- scripts reject the wrong probe or auxiliary UART
- flash commands refuse to run when lifecycle/BANKMODE is unknown or differs
  from the reviewed test profile
- a failed test leaves the board recoverable
- test output names case, build ID, core mask, and PASS/FAIL
- the same test can run repeatedly without CCS GUI state
- split/lockstep and SSUMODE tests start from their required reset class, not
  merely a warm debugger reset

## 7. Build-system work

### 7.1 CMake and tool discovery

Require CMake 3.29 or newer only when the selected board uses C29. Native
TIClang compiler identification starts there; the current 3.20 project
minimum remains valid for existing boards. The C29 toolchain fails before
`project()` with a clear version error instead of forcing a fake compiler ID.
Gate A pins the exact working CMake/TIClang tuple.

Add `cmake/toolchain-c29-ticlang.cmake` with cache/environment discovery for:

- `TI_CCS_ROOT`
- `TI_C29_CGT_ROOT`
- `TI_F29H85X_SDK_ROOT`
- `TI_SYSCONFIG_ROOT`

The toolchain locates `c29clang`, `c29lnk`, `c29ar`, `c29readelf`,
`c29objdump`, `c29nm`, `c29size`, `c29ofd`, and `c29objcopy`; sets
`CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`; proves `.S` preprocessing and
C29 integrated-assembly compilation; and records versions in the build
manifest. Gate A repeats the capability check for every supported compiler
release rather than assuming the local STS file set.

No source or script may contain `/home/ulipe/ti`.

Wire C29 through all current selectors, not only the toolchain:

- `cmake/arch.cmake` accepts `UL_BOARD_ARCH=c29` and selects
  `toolchain-c29-ticlang.cmake`
- `cmake/arch_sources.cmake` defines the C29 kernel/executable source sets
- `tools/dev.py` stops deriving every toolchain as
  `toolchain-<arch>-gcc.cmake` and accepts the external TI mount
- SDK-suite selection gains a silicon runner instead of a QEMU command

`board.cmake` and `board_config.h` both declare
`ULMK_BOARD_C29_DLB_WORKAROUND=1` for their respective configure-time and C
consumers. `cmake/config.cmake` rejects a C29 SMP configure unless the CMake
declaration is present; runtime boot still reads back the hardware bits.

Audit every GCC/GNU-specific assumption in `CMakeLists.txt`,
`cmake/optimize.cmake`, `cmake/component_api.cmake`, and
`cmake/linker_api.cmake`. This includes `-fno-data-sections`,
`-fno-tree-loop-distribute-patterns`, `-Ofast`, `-fno-inline`,
`-nostartfiles`, `-Wl,*`, start/end groups, archive naming, section naming,
and `.S` behavior. Add compiler-family helpers for compile, archive, and link
options; do not scatter `if(C29)` conditionals over targets. Do not hide
unsupported options globally with `-Qunused-arguments`.

The link backend must not rely on `-nostartfiles`: the installed driver
ignores it. Gate A compares `-nostdlib` and `-nodefaultlibs`, verifies the
resulting library list from the link trace/map, and explicitly selects any
required 64-bit arithmetic or compiler-runtime helpers. C29 leading-zero
count uses the verified `__builtin_c29_i32_clzeros_d` intrinsic rather than
assuming generic `__builtin_clz` lowering.

### 7.2 TI linker backend

GNU linker fragments cannot be passed to the TI linker. Keep the existing GNU
renderer untouched and add an explicit TI-C29 renderer selected by
`--dialect ti-c29 --core <1|2|3>`:

- board input is `memory.cmd`
- dedicated TI templates reproduce the symbols that kernel code actually
  consumes
- component, application, domain, stack, user-pool, and shared sections are
  generated from the same CMake registrations
- CPU1, CPU2, and CPU3 scripts receive an explicit core parameter
- per-core SSU configuration and BANKMODE-dependent addresses are generated
  from one reviewed policy input
- the canonical-address/entry-ID and per-core local resolver tables are
  generated and compared
- generated maps are checked for region overflow, executable/data placement,
  and the cross-core entry-ID table

Gate A must first prove TI linker syntax for archive-member selection,
retention, alignment, NOLOAD-like sections, fill/ECC behavior, load/run
addresses, symbol definitions, and unused-section elimination. Existing GNU
fragments hard-code selectors such as `*libulmk_kernel.a:(...)`; the TI
backend must not reuse that syntax blindly.

The TI path never calls the GNU `parse_flags()` routine or interprets
`HAVE_CSA/HAVE_SMALL_DATA/HAVE_BMHD`. Core, BANKMODE, SSU policy, and C29
feature inputs arrive as explicit generator arguments; `memory.cmd` supplies
only named device regions that the TI renderer validates.

Final entry addresses require a deterministic two-pass link:

1. first-link CPU1/CPU2/CPU3 and emit symbol manifests
2. run a CMake custom command that assigns stable IDs and generates all
   creator-address-to-ID plus local-ID-to-address tables
3. final-link with those tables in reserved terminal sections that cannot move
   executable symbols
4. compare final symbols to the first-pass manifests and fail on drift or an
   unregistered cross-core entry

`_ulmk_finalize_build()` must select linker suffix, generator dialect, map
flags, library search/order semantics, runtime libraries, and dead-section
elimination from a compiler-family backend. The C29 multicore path creates
three internal executable targets with core-specific scripts and exposes only
the packed CPU1 target as `ulmk`. The flash post-link backend adds the
lifecycle-appropriate certificate/metadata without modifying files inside the
TI SDK installation.

Update `tools/sdk_build.sh` as part of this work: it currently assumes host
`ar`, a `_gcc` artifact tag, GNU `.ld`, `-T`, textual archive-name rewriting,
GNU `grep` validation, and one directly linkable image. C29 SDK output uses
the discovered C29 archiver, a `ticlang` tag, processed core-specific `.cmd`
files, and a manifest that identifies the CPU1 load artifact plus embedded
secondary payloads. The SDK consumer rules in
`tests/sdk_suite/sdk_case.mk` need a C29 compile/link backend and DSS/HIL
runner; changing `sdk_build.sh` alone is insufficient. Its C29 interface uses
`C29_DSS_RUNNER`, `C29_CCXML`, `C29_PROBE_SERIAL`, `C29_UART`, and
`C29_HIL_TIMEOUT` instead of `QEMU_RUNNER`, `MACHINE`, and `QEMU_EXTRA`.

### 7.3 Proprietary tools in the dev environment

Do not redistribute CCS or the SDK in the repository image.

Extend `tools/dev.py` with an optional read-only TI installation mount and
explicit USB/serial device forwarding for a self-hosted HIL runner. Normal
QEMU and host-unit jobs must remain usable without TI software.

The host path selected by `TI_INSTALL_ROOT` mounts read-only at `/ti`.
Container-side discovery derives CCS, SDK, SysConfig, and compiler roots
beneath `/ti`, and extends `CONTAINER_PATH` with
`${TI_C29_CGT_ROOT}/bin`. HIL mode alone forwards the selected
`/dev/bus/usb` XDS110 node and by-id application UART; compile-only C29 jobs
receive neither device.

The C29 compile and HIL lanes run on a licensed self-hosted runner. The board
is an exclusive resource keyed by XDS110 serial.

## 8. Repository changes

### 8.1 `ulmk`

Planned areas:

- `arch/c29/include/arch_config.h`
- `arch/c29/include/ulmk_arch.h`
- `arch/c29/include/ulmk_syscall_abi.h`
- `arch/c29/startup.S`
- `arch/c29/trap.S`
- `arch/c29/ctx_switch.S`
- `arch/c29/arch.c`
- `arch/c29/irq.c`
- `arch/c29/mpu.c`
- `arch/c29/smp.c`
- `arch/c29/linker/`
- `kernel/kernel_main.c` arch-neutral idle entry/privilege hooks
- TIClang toolchain and TI linker generation
- two-pass cross-core entry-table generator
- C29 compile/disassembly tests
- C29 HIL runner integration
- architecture/API/build/test documentation updates

The split among C files may change after the spikes, but kernel/arch layering
must not. The C29 `ulmk_arch.h` is checked against the complete symbol set used
by the current kernel, including SMP/IPI, cycle counter, scheduler-switch, MPU,
IRQ-attach, fault-restore, and boot callbacks; the older prose specification
alone is not treated as an exhaustive header.

### 8.2 `ulmk_boards`

Add `launchxl_f29h85x/` with:

- `board.cmake` with `UL_BOARD_ARCH=c29` and
  `ULMK_BOARD_C29_DLB_WORKAROUND=1`
- `board_config.h` with `ULMK_ARCH_NUM_CPU=3`, PIPE/timer/IPC addresses, and a
  matching `ULMK_BOARD_C29_DLB_WORKAROUND=1`
- `memory.cmd`
- pinned SysConfig source/configuration inputs
- early clock, watchdog, ECC RAM, pinmux, UART, and LED initialization
- XDS110/CCS environment discovery
- target `.ccxml`
- load, flash, reset, debug, serial capture, and HIL scripts
- recovery and jumper/boot-mode documentation

C29 SMP configure fails unless the selected board declares the DLB workaround;
early boot also reads back all three global enable bits before releasing a
secondary. The build-time declaration never substitutes for the runtime
check.

Use the XDS110 application UART, initially at 115200 8N1. CPU1 is the only
normal console writer; other CPUs report through shared result records that
CPU1 prints.

The reference SysConfig pins UARTA frame 0 (`0x60070000`) and emits the
peripheral-frame mapping consumed by the BSP and SSU policy. Peripheral frames
are relocatable in `0x00400000` steps, so neither the arch layer nor MMIO
allowlist may assume frame 0 globally.

Early RAM tests may use generated TI driverlib/SysConfig output. The final
BSP should link only the required vendor pieces and must document their
licenses and exact SDK compatibility.

### 8.3 `ulmk_apps`

Add narrowly scoped components in this order:

- C29 board banner, LED, and UART smoke
- register-context stress
- syscall/block/wake stress
- SSU negative-access tests
- three-core atomic and IPC IPI smoke
- three-core scheduler stress
- baseline hello/ping-pong
- existing silicon SDK-suite cases adapted to the C29 HIL protocol

Bring-up-only components are removed or clearly marked once equivalent
permanent tests cover them.

## 9. Implementation sequence

### Phase 0 — feasibility

Run the blocking compiler/linker, INT/protected-entry, recoverable-fault,
atomic-ordering, and HIL-control probes from Gates A, C, D, E, and G as small,
reviewable spikes. Keep each spike separate from broad kernel changes. Record
generated assembly, linker maps, silicon revision, measured behavior, and the
exact TI tool versions.

Exit: every blocking question has executable evidence and this plan reflects
the selected mechanisms.

### Phase 1 — build and linker foundation

- land TIClang discovery
- land TI `.cmd` generation
- produce one-core RAM `ulmk.out`
- keep all existing GNU boards unchanged

Exit: Gate A passes and C29 startup reaches early UART from
`ulmk_kern_start()`.

### Phase 2 — UP kernel

- complete context entry/exit
- add tick, yield, syscalls, IRQ routing, and root thread
- run with SSU permissive only long enough to debug the mechanics

Exit: Gates B and C pass; baseline plus UP HIL
scheduler/IPC/notif/timer tests pass.

### Phase 3 — SSU isolation

- generate static Link/Stack/AP layout
- add dynamic thread regions and grants
- add recoverable fault policy
- enable driver/user separation

Exit: Gate D passes without Link2 override outside controlled
boot/configuration code.

### Phase 4 — three-core SMP

- package and boot CPU2/CPU3
- add shared state, atomics, IPC IPI, per-CPU ticks, and ready barriers
- land the two-pass post-link entry-ID/resolver table generator
- enforce CPU affinity, all creator/destination mappings, and pointer-inventory
  checks

Exit: Gate E and the RAM portion of Gate F pass; all three CPUs schedule
concurrently and pass stress.

### Phase 5 — flash and operational tooling

- add signed/dummy-certificate development image flow as appropriate for the
  board life-cycle state
- add flash-safe DSS operations and cold boot
- add command-line multicore debug

Exit: flash/SSUMODE2 Gate F and all Gate G criteria pass, including repeatable
reset, attach, symbol load, fault dump, and standalone boot.

### Phase 6 — regression and documentation

- run all existing TriCore, RISC-V, ARMv7-M, and ARMv8-M regressions
- add C29 compile lane and C29 HIL lane
- update porting, architecture API, linker, build, and application guides
- document supported TI tool versions and recovery procedures

Exit: the complete release matrix passes.

## 10. HIL protocol

Tests should emit machine-readable lines on the CPU1 UART:

```text
ULMK-HIL:<case>:START build=<id> silicon=<revision>
ULMK-HIL:<case>:CORE_READY mask=0x7
ULMK-HIL:<case>:PASS
```

On failure, CPU1 prints the failing core, stage, last fault/PIPE/IPC state,
and a stable numeric reason before halting or entering the debugger-safe
failure loop.

The HIL runner:

1. acquires the board lock
2. verifies XDS110 serial and firmware
3. verifies the application UART interface
4. builds or validates the requested artifact
5. starts UART capture
6. loads/flashes and runs through DSS
7. waits for the exact PASS marker
8. captures timeout diagnostics from all CPUs
9. resets or releases the target to a documented state
10. releases the lock

RAM loading is the default during bring-up. Flash and security configuration
tests are separate and opt-in until board recovery is proven. Before the first
flash command, the runner records the actual device lifecycle and BANKMODE;
it must not infer GP/HS-FS/HS-SE state from the board model or debugger
availability.

## 11. Regression policy

Every C29 kernel change must run:

- architecture-independent host unit tests
- C29 compile/link/disassembly checks
- C29 UP HIL
- C29 three-core SMP HIL when SMP, scheduler, IRQ, atomic, IPC, memory, or
  linker code is touched
- existing four-board unit, SDK, and baseline QEMU regression

Before merge, the self-hosted C29 HIL lane is mandatory. A compiler-only job
does not substitute for the absent emulator.

## 12. Principal risks

### Protected syscall entry

Highest risk. The protected-call stack and PIPE ownership rules may prevent a
safe software-interrupt doorbell. Gate C decides feasibility before the
public syscall layer is ported. Threshold-priority Supervisor nesting and the
mandatory post-`RET.PROT` `EXIT.PROT` packet are part of the same gate.

### Cross-Link authority

Branching between Links assigned to one Stack can change active authority.
The initial common runtime Link and dynamic driver MMIO policy are mandatory
until Gate D proves an alternative cannot be used for escalation.

### Dynamic APR handoff

Returning from Link 2 restores the old scheduler-Stack A15 before the veneer
can adopt the selected A15. The port needs an interrupt-safe two-stack window
and protected cleanup with no user execution in between; otherwise stack
isolation cannot survive a context switch.

### Cross-core atomic exclusion

Compiler atomics are not a valid contract. The memory-wrapper behavior of
the emitted `ATOMIC.M`-family sequence must be demonstrated across all three
CPUs under contention, and its memory-ordering scope must be established
separately from mutual exclusion.

### Shared-RAM stale reads

The current errata affects every listed silicon revision. SMP is invalid
unless all three global initiator-enable bits are clear before ulmk's first
shared use and secondary release, and the workaround is covered by a
message-passing stress test.

### Recoverable SSU faults

Protection violations enter the NMI/error path, not a conventional writable
trap frame. The port must prove that it can prevent re-execution of the
faulting instruction and continue on another thread; otherwise the current
recoverable-fault contract cannot be claimed.

### Multicore program addresses

Different LPA/CPA addresses make shared function pointers unsafe. Link-map
equivalence alone is insufficient; generated entry IDs and the cross-core
pointer inventory must be enforced.

### CPU2 code budget

The simultaneous tri-core RAM profile budgets 32 KiB LPA1 for CPU2 and 32 KiB
CPA0 for CPU3. Any larger profile must prove non-overlap explicitly. Overflow
is a hard product limit and fails at link time.

### SSU configuration lifecycle

Committed/flash security configuration can remove debug or recovery access.
Early work must remain in reversible RAM-mode configuration and follow the
current silicon errata.

### Toolchain drift and licensing

The local compiler is STS and older than the current LTS release. Discovery,
version checks, and a self-hosted licensed runner are required; TI binaries
must not be committed.

### Interrupt cost

Saving the entire C29 register file is large. Correctness comes first; later
optimization requires measured WCET and proof that compiler-used M registers
remain preserved.

### Single physical target

The board cannot service parallel jobs. HIL needs serialization, hard
timeouts, recovery, and enough diagnostics to distinguish target failure from
probe/UART failure.

## 13. Definition of done

The C29/F29H85x port is complete only when:

- UP and three-core SMP builds produce one final `ulmk.out`.
- RAM load and standalone flash boot both pass.
- SSUMODE2 images contain valid per-core security configuration at the
  BANKMODE-selected addresses and the required boot certificate/metadata.
- Root thread, syscalls, IPC, notifications, timers, IRQ binding, thread
  lifecycle, capabilities, memory grants, and fault policy pass on silicon.
- Three per-CPU ticks and all directed reschedule IPIs pass.
- Atomic, scheduler, pool, and remote-wake stress pass on all three CPUs.
- SSU negative tests prove kernel/thread isolation, deny privileged control
  apertures, and enforce exact dynamic driver MMIO mappings without claiming
  rollback of side-effecting MMIO.
- CPU2 size and every linker/AP limit are checked automatically.
- Command-line load, flash, reset, attach, run, halt, symbol load, memory
  inspection, and fault capture work without CCS GUI interaction.
- No path or test depends on `/home/ulipe`, `ttyACM0`, or probe enumeration
  order.
- Current silicon errata and supported TI tool versions are documented.
- Existing TriCore, RISC-V, ARMv7-M, and ARMv8-M regression remains green.

## 14. Review checkpoints

Request focused review after:

1. syscall/INT/SSU gate design
2. full context-frame assembly
3. TI linker and three-image packaging
4. emitted `ATOMIC.M`-family sequence and memory-ordering evidence
5. SSU fault and grant model
6. final HIL and flash recovery procedure

Do not combine these checkpoints into one large architecture PR.
