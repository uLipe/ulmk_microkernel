# ulmk — Public API Specification

**Version:** 1.0
**Header:** `#include <ulmk/microkernel.h>`
**Status:** Reflects the implemented API in `include/ulmk/microkernel.h`.

> **Purpose of this document:** complete reference for all syscall wrappers
> exposed to userspace components.  Consult this when writing a component or
> a board service.  For the arch-internal contract see `docs/arch_api_spec.md`.
> For the build system see `docs/build_system_spec.md`.

---

## Table of Contents

1. [Model](#1-model)
2. [Types and Constants](#2-types-and-constants)
3. [Error Codes](#3-error-codes)
4. [Privilege Levels](#4-privilege-levels)
5. [Capabilities](#5-capabilities)
6. [Thread API](#6-thread-api)
7. [IPC Endpoint API](#7-ipc-endpoint-api)
8. [Notification API](#8-notification-api)
9. [Memory API](#9-memory-api)
10. [IRQ API](#10-irq-api)
11. [Timekeeping — Kernel Sleep and Board Timer](#11-timekeeping--kernel-sleep-and-board-timer)
12. [Syscall Number Table](#12-syscall-number-table)
13. [Capability Grant API](#13-capability-grant-api)
14. [Boot Entry Point](#14-boot-entry-point)

---

## 1. Model

All public API functions are **static inline syscall wrappers** defined in
`include/ulmk/microkernel.h`.  They issue a TriCore `SYSCALL` instruction; the
kernel router in `kernel/syscall/syscall_router.c` dispatches to the handler.

Calling convention (TriCore ABI):

```
SYSCALL #N         D15 = N (syscall number / TIN)
                   D4  = arg0
                   D5  = arg1
                   D6  = arg2
                   D7  = arg3
                   D2  = return value (written by kernel before RFE)
```

Up to 4 arguments fit in registers.  When more are needed, the caller allocates
a small struct on its stack and passes a pointer as one of the four arguments
(see `ulmk_ep_reply_recv` and `ulmk_ep_recv_or_notif`).

The kernel entry path (`ulmk_arch_syscall_entry` in `arch/tricore/arch.c`) raises
`CCPN = 255` before calling the syscall router, disabling all hardware IRQs for
the duration of the syscall.

---

## 2. Types and Constants

```c
typedef uintptr_t ulmk_tid_t;      /* opaque TCB handle (kernel pointer cast) */
typedef uintptr_t ulmk_ep_t;       /* IPC endpoint handle — raw pointer */
typedef uintptr_t ulmk_notif_t;    /* notification object handle — raw pointer */

#define ULMK_TID_INVALID    ((ulmk_tid_t)0)
#define ULMK_EP_INVALID     ((ulmk_ep_t)0)
#define ULMK_NOTIF_INVALID  ((ulmk_notif_t)0)
```

Handles are raw kernel pointers.  They are valid only while the object exists;
using a freed handle is undefined behaviour.

### IPC message

```c
#define ULMK_MSG_WORDS  6

typedef struct {
    uint32_t label;               /* caller-defined message tag */
    uint32_t words[ULMK_MSG_WORDS]; /* 24 bytes of inline payload */
} ulmk_msg_t;
```

Total inline payload: 28 bytes per message (label + 6 words).  This is the
**control-plane envelope** only — the kernel stages it by pointer between the
caller's and server's userspace buffers (one copy each way on the hot path).

For larger payloads, put a pointer/length (or buffer id) in `words[]` and let
userspace share the data region (`ulmk_mem_grant()` or a buffer both sides can
already reach).  The kernel does **not** copy bulk data on IPC.

### Thread attributes

```c
typedef struct {
    const char    *name;        /* display name (up to 15 chars) */
    void         (*entry)(void *arg);
    void          *arg;
    uint8_t        priority;    /* 0 = highest, 255 = lowest */
    size_t         stack_size;  /* bytes; allocated from user_pool */
    ulmk_privilege_t privilege;   /* ULMK_PRIV_USER or ULMK_PRIV_DRIVER */
    size_t         heap_size;   /* 0 = no per-thread heap; last for compat */
} ulmk_thread_attr_t;
```

Always declare `ulmk_thread_attr_t attr = {0}` before setting individual fields so
that `heap_size` defaults safely to zero.

### Thread heap descriptor

```c
typedef struct {
    uintptr_t base;  /* start of the heap region within the slabAO */
    size_t    size;  /* bytes; equals attr.heap_size at creation */
} ulmk_heap_info_t;
```

Returned by `ulmk_get_thread_heap()`.

### Boot information

```c
#define ULMK_BOOT_MAX_MEM_REGIONS  4

typedef struct {
    struct {
        uintptr_t base;
        size_t    size;
    } mem[ULMK_BOOT_MAX_MEM_REGIONS];
    uint32_t  mem_count;
    uintptr_t csa_pool_base;
    size_t    csa_pool_size;
} ulmk_boot_info_t;
```

Valid only for the duration of `ulmk_root_thread()`.  Copy any fields needed
beyond bootstrap before spawning child threads.

### Memory domain descriptor

```c
#define ULMK_PERM_READ   (1u << 0)
#define ULMK_PERM_WRITE  (1u << 1)
#define ULMK_PERM_EXEC   (1u << 2)
#define ULMK_PERM_USER   (1u << 3)

typedef struct {
    const char *name;
    uintptr_t   start;
    uintptr_t   end;
    uint32_t    perms;
} ulmk_domain_desc_t;
```

---

## 3. Error Codes

| Constant | Value | Meaning |
|----------|-------|---------|
| `ULMK_OK` | 0 | Success |
| `ULMK_EINVAL` | −1 | Invalid argument |
| `ULMK_ENOMEM` | −2 | Out of memory / pool exhausted |
| `ULMK_EPERM` | −3 | Permission denied (missing capability or privilege) |
| `ULMK_ENOSPC` | −4 | No space in table |
| `ULMK_EDEADLK` | −5 | Would deadlock |
| `ULMK_ESRCH` | −6 | Thread or object not found |
| `ULMK_ETIMEOUT` | −7 | Timer deadline expired before operation completed |
| `ULMK_ECANCELED` | −8 | Operation cancelled (e.g. sleep cancel) |
| `ULMK_ENOTSUP` | −9 | Feature not compiled in (e.g. irq_attach off) |

---

## 4. Privilege Levels

```c
typedef enum {
    ULMK_PRIV_USER   = 0,   /* PSW.IO = 0: no peripheral access */
    ULMK_PRIV_DRIVER = 1,   /* PSW.IO = 1: peripheral access, restricted syscalls */
    ULMK_PRIV_KERNEL = 2,   /* PSW.IO = 2: supervisor — kernel-internal only */
} ulmk_privilege_t;
```

The root thread starts at `ULMK_PRIV_DRIVER`.  Most board services and driver
threads run at `ULMK_PRIV_DRIVER`.  Untrusted application threads use
`ULMK_PRIV_USER`.

---

## 5. Capabilities

Capabilities are a bitmask in the TCB.  Checked by the syscall router before
privileged operations.

| Constant | Bit | Operation gated |
|----------|-----|----------------|
| `ULMK_CAP_SPAWN` | 0 | `ulmk_thread_create()` |
| `ULMK_CAP_KILL` | 1 | `ulmk_thread_kill()` |
| `ULMK_CAP_IRQ` | 2 | `ulmk_irq_bind()`, `ulmk_irq_bind_hw()`, `ulmk_irq_attach()`, `ulmk_irq_attach_hw()`, `ulmk_irq_detach()`, `ulmk_irq_enable()`, `ulmk_irq_disable()`, `ulmk_irq_ack()` |
| `ULMK_CAP_MAP_PERIPH` | 3 | `ulmk_mem_map()` with `ULMK_MMAP_PERIPH` |
| `ULMK_CAP_GRANT_CAP` | 4 | `ulmk_cap_grant()` |
| `ULMK_CAP_MAP_SHARED` | 5 | `ulmk_mem_map()` with `ULMK_MMAP_SHARED` |
| `ULMK_CAP_ALL` | 0xFF | All capabilities; initial value of the root thread |

---

## 6. Thread API

### `ulmk_thread_self` — get own TID

```c
ulmk_tid_t ulmk_thread_self(void);
```

Returns the TID of the calling thread.  Available at any privilege level.

---

### `ulmk_thread_yield` — cooperative yield

```c
int ulmk_thread_yield(void);
```

Yields the CPU to the next runnable thread at the same priority (FIFO within
level).  Does **not** time-slice across equal-priority threads — use explicit
blocking or periodic yields for fairness at the same level.  Returns `ULMK_OK`.

---

### `ulmk_thread_create` — spawn a thread

```c
ulmk_tid_t ulmk_thread_create(const ulmk_thread_attr_t *attr);
```

Allocates a TCB and a stack of `attr->stack_size` bytes from the user pool,
initialises the CSA chain to start at `attr->entry(attr->arg)`, and makes the
thread runnable.

**Requires:** `ULMK_CAP_SPAWN`.

Returns the new TID, or `ULMK_TID_INVALID` on failure (`ULMK_ENOMEM`, `ULMK_ENOSPC`,
`ULMK_EPERM`).

---

### `ulmk_thread_kill` — terminate a thread

```c
int ulmk_thread_kill(ulmk_tid_t tid);
```

Terminates `tid`, frees its stack and CSA chain, and removes it from the
scheduler.  If `tid` is blocked on an endpoint or notification, it is removed
from the wait list first.

**Requires:** `ULMK_CAP_KILL`.

Returns `ULMK_OK`, `ULMK_ESRCH` (TID not found), or `ULMK_EPERM`.

---

### `ulmk_thread_suspend` / `ulmk_thread_resume`

```c
int ulmk_thread_suspend(ulmk_tid_t tid);
int ulmk_thread_resume(ulmk_tid_t tid);
```

Suspend removes the thread from the ready queue without terminating it.
Resume makes it runnable again.  A suspended thread does not consume CPU.

**Requires:** `ULMK_PRIV_DRIVER`.

---

### `ulmk_thread_priority_set` / `ulmk_thread_priority_get`

```c
int ulmk_thread_priority_set(ulmk_tid_t tid, uint8_t prio);
int ulmk_thread_priority_get(ulmk_tid_t tid);
```

Dynamic priority adjustment.  Priority 0 is highest.  Changes take effect at
the next scheduler decision point.

---

### `ulmk_thread_exit` — terminate self

```c
__attribute__((noreturn)) void ulmk_thread_exit(void);
```

Terminates the calling thread.  Never returns.  The `ulmk_root_thread()` function
must call this when bootstrapping is complete.

---

## 7. IPC Endpoint API

The IPC model is synchronous call-reply.  The caller blocks until the server
replies.  Priority inheritance: the server runs at max(server\_prio,
caller\_prio) while processing a call.

### `ulmk_ep_create`

```c
ulmk_ep_t ulmk_ep_create(void);
```

Allocates an endpoint from the kernel pool.  Returns `ULMK_EP_INVALID` if the
pool is exhausted.  The endpoint is owned by the calling thread.

**Important:** create the endpoint *before* spawning the server thread.  This
ensures clients can call the endpoint immediately after `<name>_init()` returns,
without a spin-wait.

---

### `ulmk_ep_call`

```c
int ulmk_ep_call(ulmk_ep_t ep, ulmk_msg_t *msg);
```

Sends `*msg` to the endpoint and blocks until the server calls `ulmk_ep_reply()`.
The reply overwrites `*msg` in place.  The kernel keeps a pointer to `msg` while
the caller is blocked (no TCB bounce of the envelope on the slow path).

Returns `ULMK_OK` or `ULMK_EINVAL` (bad endpoint).

---

### `ulmk_ep_recv`

```c
int ulmk_ep_recv(ulmk_ep_t ep, ulmk_msg_t *msg, ulmk_tid_t *sender);
```

Blocks until a message arrives on `ep`.  Fills `*msg` with the message and
`*sender` with the caller's TID.  The server must call `ulmk_ep_reply(sender, …)`
to unblock the caller.

---

### `ulmk_ep_reply`

```c
int ulmk_ep_reply(ulmk_tid_t sender, const ulmk_msg_t *reply);
```

Sends `*reply` to the blocked `sender` and makes it runnable.

---

### `ulmk_ep_reply_recv`

```c
int ulmk_ep_reply_recv(ulmk_ep_t ep, ulmk_tid_t sender,
                     const ulmk_msg_t *reply,
                     ulmk_msg_t *next, ulmk_tid_t *next_sender);
```

Atomic reply-and-receive: replies to `sender`, then immediately blocks on `ep`
for the next call.  Avoids a round-trip through the scheduler compared with
separate `ulmk_ep_reply` + `ulmk_ep_recv`.

The four output pointers are packed into a stack-allocated `ulmk_reply_recv_args_t`
to stay within the 4-register argument limit.

---

### `ulmk_ep_grant`

```c
int ulmk_ep_grant(ulmk_ep_t ep, ulmk_tid_t target);
```

Grants access to `ep` to `target`.  The target thread can then call `ulmk_ep_call`
on it.  The endpoint owner retains full access.

---

### `ulmk_ep_recv_or_notif`

```c
int ulmk_ep_recv_or_notif(ulmk_ep_t ep, ulmk_notif_t notif,
                        uint32_t mask,
                        ulmk_msg_t *msg, ulmk_tid_t *sender,
                        uint32_t *notif_bits);
```

Blocks on either an IPC call on `ep` or a notification signal on `notif` (with
bitmask `mask`), whichever arrives first.  Useful for a server that must also
react to hardware events.

Returns `ULMK_OK`.  On return, exactly one of `*sender != ULMK_TID_INVALID` (IPC
path) or `*notif_bits != 0` (notification path) is true.

---

### `ulmk_ep_destroy`

```c
int ulmk_ep_destroy(ulmk_ep_t ep);
```

Frees the endpoint.  Any threads blocked on `ep` are unblocked with
`ULMK_EINVAL`.

---

## 8. Notification API

Notifications are a lightweight one-to-many signalling primitive.  32 bits per
object; each bit is an independent flag.  Suitable for hardware IRQ delivery and
event broadcasting.

### `ulmk_notif_create`

```c
ulmk_notif_t ulmk_notif_create(void);
```

Allocates a notification object.  Returns `ULMK_NOTIF_INVALID` if the pool is
exhausted.

---

### `ulmk_notif_signal`

```c
int ulmk_notif_signal(ulmk_notif_t notif, uint32_t bits);
```

Atomically OR `bits` into the notification state.  If any thread is blocked
waiting for those bits, it is woken.

---

### `ulmk_notif_poll`

```c
uint32_t ulmk_notif_poll(ulmk_notif_t notif, uint32_t mask);
```

Returns the current set bits matching `mask` and clears them atomically.
Returns 0 if no bits matching `mask` are set.  Does not block.

---

### `ulmk_notif_wait`

```c
int ulmk_notif_wait(ulmk_notif_t notif, uint32_t mask, uint32_t *bits);
```

Blocks until at least one bit matching `mask` is set.  Clears and returns the
matching bits in `*bits`.

---

### `ulmk_notif_destroy`

```c
int ulmk_notif_destroy(ulmk_notif_t notif);
```

Frees the notification object.  Threads blocked on it are woken with
`ULMK_EINVAL`.

---

## 9. Memory API

### SlabAO per-thread heap model

Each thread may carry a private heap allocated at creation time by setting
`attr.heap_size > 0`.  The kernel allocates a contiguous *slabAO*
(`stack_size + heap_size` bytes) from `user_pool` and covers it with a single
MPU DPR.  The TCB lives in a separate allocation so userspace cannot reach
kernel metadata through its DPR.

### `ulmk_get_thread_heap`

```c
int ulmk_get_thread_heap(ulmk_heap_info_t *info);
```

Populates `*info` with the heap base and size for the calling thread.
Returns `ULMK_OK` on success, `ULMK_EPERM` if the thread has no heap
(`attr.heap_size == 0`).

---

### `ulmk_heap_extend`

```c
int ulmk_heap_extend(size_t size);
```

Allocates `size` bytes from the global `user_pool` and adds the new region as
an additional MPU DPR for the calling thread.  Requires `ULMK_PRIV_DRIVER`.
Returns `ULMK_OK`, `ULMK_ENOMEM`, `ULMK_EPERM`, or `ULMK_ENOSPC` (DPR limit
reached).

---

### `ulmk_mem_map`

```c
void *ulmk_mem_map(void *hint, size_t size, uint32_t perms, uint32_t flags);
```

Maps a memory region.  Flags:

| Flag | Meaning |
|------|---------|
| `ULMK_MMAP_ANON` | Anonymous mapping from `user_pool` |
| `ULMK_MMAP_PERIPH` | Map a peripheral MMIO region (requires `ULMK_CAP_MAP_PERIPH`) |
| `ULMK_MMAP_SHARED` | Map a fixed physical window (SDRAM, framebuffer, …); requires `ULMK_CAP_MAP_SHARED`. Arch MPU uses `ULMK_REGION_SHARED` (ARMv7-M: Normal WB non-shareable for FMC/LTDC — clean D-cache before LTDC/DMA; not `ULMK_MMAP_PERIPH`) |

---

### D-cache maintenance

```c
int ulmk_dcache_clean(const void *addr, size_t size);
int ulmk_dcache_invalidate(const void *addr, size_t size);
int ulmk_dcache_clean_invalidate(const void *addr, size_t size);
```

Requires `ULMK_PRIV_DRIVER`.  Implemented only when the arch sets
`ULMK_ARCH_HAS_CACHE` (ARM Cortex-M7 with `ULMK_BOARD_ENABLE_CPU_CACHE=1`).
Otherwise the syscalls return `ULMK_ENOTSUP`.  Pass `size == 0` to
`ulmk_dcache_clean` to clean the entire D-cache (set/way).

---

### `ulmk_mem_unmap`

```c
int ulmk_mem_unmap(void *addr, size_t size);
```

Unmaps a previously mapped region.

---

### `ulmk_mem_grant`

```c
int ulmk_mem_grant(void *addr, size_t size, ulmk_tid_t target, uint32_t perms);
```

Grants access to a memory region to `target` thread with the specified
permissions.  Used to share large buffers between components without copying.

---

## 10. IRQ API

Requires `ULMK_PRIV_DRIVER` privilege and `ULMK_CAP_IRQ` capability.

### `ulmk_irq_bind`

```c
int ulmk_irq_bind(uint8_t srpn, ulmk_notif_t notif, uint32_t bit);
```

Binds hardware interrupt `srpn` to bit `bit` in `notif`.  When the interrupt
fires, the kernel calls `ulmk_notif_signal(notif, 1u << bit)` from the ISR.

At most `ULMK_CONFIG_MAX_IRQ_BINDINGS` bindings can be active simultaneously.

**Syscall:** `ULMK_SYS_IRQ_BIND` (60).

---

### `ulmk_irq_enable` / `ulmk_irq_disable`

```c
int ulmk_irq_enable(uint8_t srpn);
int ulmk_irq_disable(uint8_t srpn);
```

Enable or disable the SRC for `srpn`.

**Syscalls:** `ULMK_SYS_IRQ_ENABLE` (61), `ULMK_SYS_IRQ_DISABLE` (62).

---

### `ulmk_irq_ack`

```c
int ulmk_irq_ack(uint8_t srpn);
```

Clear the pending flag in the SRC for `srpn`.  Must be called after processing
a level-triggered interrupt to prevent immediate re-entry.

**Syscall:** `ULMK_SYS_IRQ_ACK` (63).

---

### `ulmk_irq_bind_hw`

```c
int ulmk_irq_bind_hw(uint8_t srpn, ulmk_notif_t notif, uint32_t bit,
		     uintptr_t src_reg_addr);
```

Associate a fixed hardware SRC register (TriCore: absolute address of the
`SRC_*` register) with `srpn` and deliver `bit` in `notif` when the interrupt
fires.  Used by board services (timer, UART, etc.) that own a specific SRC.

**Requires:** `ULMK_CAP_IRQ` and `ULMK_PRIV_DRIVER`.
**Syscall:** `ULMK_SYS_IRQ_BIND_HW` (64).

---

### `ulmk_irq_attach` / `ulmk_irq_attach_hw`

```c
typedef bool (*ulmk_irq_attach_fn_t)(void *data);

ulmk_notif_t ulmk_irq_attach(uint8_t srpn, ulmk_irq_attach_fn_t fn, void *data);
ulmk_notif_t ulmk_irq_attach_hw(uint8_t srpn, ulmk_irq_attach_fn_t fn,
				void *data, uintptr_t src_reg_addr);
int ulmk_irq_detach(uint8_t srpn);
```

Register a userspace ISR fast-path callback.  **Opt-in** via
`ULMK_CONFIG_IRQ_ATTACH=1` (default **0**).  When the feature is compiled out,
`attach` / `attach_hw` / `detach` return `ULMK_ENOTSUP` (as a negative word for
the attach helpers).  Enabling this opens a trusted ISR path into userspace —
product responsibility.

On success a notification object owned by the binding is returned (bit 0); the
caller may `ulmk_notif_wait` on it.  When the IRQ fires the kernel invokes
`fn(data)` under the registering thread's memory map:

- return `true` — kernel acknowledges the source and signals the notif (even
  if no waiter is present);
- return `false` — callback is responsible for ack/rearm via MMIO (syscalls
  are rejected with `ULMK_EPERM` inside the callback); no notification.

Conflicts with an existing bind/attach on the same `srpn`.  A fault inside
the callback kills only the owning thread and tears the binding down.

Syscalls nested from the callback return `ULMK_EPERM` (`in_irq_attach` gate).
Arch trampolines currently run the callback under the kernel memory map
(TriCore cannot drop `PSW.IO` without a user-text return path; RISC-V/ARM
nested ecall/SVC from the IRQ path is unsafe under a user PMP/MPU switch).
A future user-text trampoline can tighten PRS/MPU isolation further.

**Requires:** `ULMK_CAP_IRQ` and `ULMK_PRIV_DRIVER`.
**Requires:** `ULMK_CONFIG_IRQ_ATTACH=1`.

**Syscalls:** `ULMK_SYS_IRQ_ATTACH` (65), `ULMK_SYS_IRQ_ATTACH_HW` (66),
`ULMK_SYS_IRQ_DETACH` (67).

Handle-or-error returns for attach: success is a notification handle (may be a
high pointer).  Errors are small negative `ULMK_E*` values.  Detect with:

```c
if (n == ULMK_NOTIF_INVALID ||
    ((int32_t)n < 0 && (int32_t)n >= -16)) {
	/* ULMK_ENOTSUP when ULMK_CONFIG_IRQ_ATTACH=0, else EINVAL/… */
}
```

---

## 11. Timekeeping — Kernel Sleep and Board Timer

The kernel owns a hierarchical **timing wheel** (`kernel/time/timer_wheel.c`)
driven by the arch periodic tick (`ulmk_arch_tick_init` / `ulmk_kern_timer_tick`).
Userspace sleeps and timed IPC waits arm timeouts on that wheel.

### `ulmk_sleep_ms` / `ulmk_sleep_cancel` / `ulmk_tick_start`

```c
int  ulmk_sleep_ms(uint32_t ms);
int  ulmk_sleep_cancel(ulmk_tid_t tid);
void ulmk_tick_start(void);
```

- `ulmk_sleep_ms` — block the calling thread for approximately `ms` milliseconds
  (rounded up to the next tick at `ULMK_CONFIG_TICK_HZ`, typically 1 kHz).
- `ulmk_sleep_cancel` — wake a sleeper with `ULMK_ECANCELED`.
- `ulmk_tick_start` — arm the arch tick once (idempotent).  Called from
  `board_timer_start()` during `board_services_init()`.

IPC/notif also expose timeout variants (`ulmk_ep_call_timeout`,
`ulmk_notif_wait_timeout`) that share the same wheel.

**TriCore SMP:** each core arms its own STM CMP0 tick and timing wheel
(`ulmk_arch_timer_wheel_cpu` → `cpu_id`).  Remote enqueue still uses GPSR IPI
for prompt wake.  RISC-V/ARM likewise keep a per-CPU wheel with a local tick.

### Board timer wrapper

Boards no longer implement compare-match sleep servers.  `board_timer.c` is a
thin wrapper:

```c
ulmk_tid_t board_timer_start(const ulmk_boot_info_t *info);
void       board_timer_sleep_us(uint32_t us);
```

`board_timer_start` maps any board-needed timer MMIO (e.g. TIM0 readback) and
calls `ulmk_tick_start()`.  `board_timer_sleep_us` converts µs → ms and calls
`ulmk_sleep_ms`.

### Board console

`board_console_putc` / `puts` / `printf` go through an IPC **console server**
so SMP output is line-atomic.  On TC275 the server sits on top of ASCLIN;
on QEMU it owns the virt/UART MMIO.

## 12. Syscall Number Table

Defined in `include/ulmk/syscall_nr.h`.  Single source of truth for both
userspace wrappers and the kernel router.

Numbers are sparse on purpose — room for additions per group without
renumbering.  Router upper bound: `ULMK_SYS_MAX = 128`.

| Nr | Symbol | Privilege / cap | API |
|----|--------|-----------------|-----|
| 1 | `ULMK_SYS_MMAP` | any; `CAP_MAP_PERIPH` if `MMAP_PERIPH` | `ulmk_mem_map` |
| 2 | `ULMK_SYS_MUNMAP` | any | `ulmk_mem_unmap` |
| 3 | `ULMK_SYS_MEM_GRANT` | any | `ulmk_mem_grant` |
| 4–6 | *(reserved)* | — | former malloc/free/aligned_alloc |
| 7 | `ULMK_SYS_HEAP_EXTEND` | any | `ulmk_heap_extend` |
| 8 | `ULMK_SYS_GET_THREAD_HEAP` | any | `ulmk_get_thread_heap` |
| 10 | `ULMK_SYS_YIELD` | any | `ulmk_thread_yield` |
| 11 | `ULMK_SYS_EXIT` | any | `ulmk_thread_exit` |
| 12 | `ULMK_SYS_SLEEP` | any | `ulmk_sleep_ms` |
| 13 | `ULMK_SYS_SLEEP_CANCEL` | any | `ulmk_sleep_cancel` |
| 14 | `ULMK_SYS_EP_CALL_TIMEOUT` | any | `ulmk_ep_call_timeout` |
| 15 | `ULMK_SYS_TICK_START` | any | `ulmk_tick_start` |
| 20 | `ULMK_SYS_THREAD_SELF` | any | `ulmk_thread_self` |
| 21 | `ULMK_SYS_CPU_ID` | any | `ulmk_cpu_id` |
| 22 | `ULMK_SYS_WCET_BIND` | any | `ulmk_wcet_bind` |
| 30 | `ULMK_SYS_EP_CREATE` | any | `ulmk_ep_create` |
| 31 | `ULMK_SYS_EP_CALL` | any | `ulmk_ep_call` |
| 32 | `ULMK_SYS_EP_RECV` | any | `ulmk_ep_recv` |
| 33 | `ULMK_SYS_EP_REPLY` | any | `ulmk_ep_reply` |
| 34 | `ULMK_SYS_EP_REPLY_RECV` | any | `ulmk_ep_reply_recv` |
| 35 | `ULMK_SYS_EP_GRANT` | any | `ulmk_ep_grant` |
| 36 | `ULMK_SYS_EP_RECV_OR_NOTIF` | any | `ulmk_ep_recv_or_notif` |
| 37 | `ULMK_SYS_EP_DESTROY` | any | `ulmk_ep_destroy` |
| 40 | `ULMK_SYS_NOTIF_CREATE` | any | `ulmk_notif_create` |
| 41 | `ULMK_SYS_NOTIF_SIGNAL` | any | `ulmk_notif_signal` |
| 42 | `ULMK_SYS_NOTIF_WAIT` | any | `ulmk_notif_wait` |
| 43 | `ULMK_SYS_NOTIF_POLL` | any | `ulmk_notif_poll` |
| 44 | `ULMK_SYS_NOTIF_DESTROY` | any | `ulmk_notif_destroy` |
| 45 | `ULMK_SYS_NOTIF_WAIT_TIMEOUT` | any | `ulmk_notif_wait_timeout` |
| 60 | `ULMK_SYS_IRQ_BIND` | DRIVER + `CAP_IRQ` | `ulmk_irq_bind` |
| 61 | `ULMK_SYS_IRQ_ENABLE` | DRIVER | `ulmk_irq_enable` |
| 62 | `ULMK_SYS_IRQ_DISABLE` | DRIVER | `ulmk_irq_disable` |
| 63 | `ULMK_SYS_IRQ_ACK` | DRIVER | `ulmk_irq_ack` |
| 64 | `ULMK_SYS_IRQ_BIND_HW` | DRIVER + `CAP_IRQ` | `ulmk_irq_bind_hw` |
| 65 | `ULMK_SYS_IRQ_ATTACH` | DRIVER + `CAP_IRQ`; needs `ULMK_CONFIG_IRQ_ATTACH=1` | `ulmk_irq_attach` |
| 66 | `ULMK_SYS_IRQ_ATTACH_HW` | DRIVER + `CAP_IRQ`; needs `ULMK_CONFIG_IRQ_ATTACH=1` | `ulmk_irq_attach_hw` |
| 67 | `ULMK_SYS_IRQ_DETACH` | DRIVER + `CAP_IRQ`; needs `ULMK_CONFIG_IRQ_ATTACH=1` | `ulmk_irq_detach` |
| 70 | `ULMK_SYS_THREAD_SPAWN` | DRIVER + `CAP_SPAWN` | `ulmk_thread_create` |
| 71 | `ULMK_SYS_THREAD_KILL` | DRIVER + `CAP_KILL` | `ulmk_thread_kill` |
| 72 | `ULMK_SYS_THREAD_SUSPEND` | DRIVER | `ulmk_thread_suspend` |
| 73 | `ULMK_SYS_THREAD_RESUME` | DRIVER | `ulmk_thread_resume` |
| 74 | `ULMK_SYS_THREAD_SET_PRIO` | DRIVER | `ulmk_thread_priority_set` |
| 75 | `ULMK_SYS_THREAD_GET_PRIO` | DRIVER | `ulmk_thread_priority_get` |
| 80 | `ULMK_SYS_PROC_CREATE` | DRIVER | *(reserved / process mgmt)* |
| 81 | `ULMK_SYS_PROC_DESTROY` | DRIVER | *(reserved / process mgmt)* |
| 82 | `ULMK_SYS_PROC_ADD_REGION` | DRIVER | *(reserved / process mgmt)* |
| 83 | `ULMK_SYS_PROC_GRANT_CAP` | DRIVER + `CAP_GRANT_CAP` | `ulmk_cap_grant` |
| 84 | `ULMK_SYS_PROC_GRANT_IRQ` | DRIVER | *(reserved)* |

Group summary:

```
 1–8   Memory / heap
10–15  Scheduling / time / timed IPC
20–22  Thread query / WCET
30–37  IPC endpoints
40–45  Notifications
60–67  IRQ (IO ≥ 1)
70–75  Thread management (IO ≥ 1)
80–84  Process / capability (IO ≥ 1)
```

While `ulmk_irq_in_attach()` is true (userspace ISR callback running), the
router rejects **all** nested syscalls with `ULMK_EPERM`.

When `ULMK_CONFIG_IRQ_ATTACH=0`, syscalls 65–67 still exist in the ABI but
return `ULMK_ENOTSUP` from the kernel handlers.

---

## 13. Capability Grant API

### `ulmk_cap_grant`

```c
int ulmk_cap_grant(ulmk_tid_t target, uint32_t caps);
```

Grants the capability bits in `caps` to `target`.  The caller must itself hold
`ULMK_CAP_GRANT_CAP`.  The root thread starts with `ULMK_CAP_ALL`.

Typical boot pattern:

```c
ulmk_tid_t driver = ulmk_thread_create(&driver_attr);
ulmk_cap_grant(driver, ULMK_CAP_IRQ | ULMK_CAP_MAP_PERIPH);
```

---

## 14. Boot Entry Point

```c
void ulmk_root_thread(const ulmk_boot_info_t *info);
```

User-provided strong symbol.  Called by the kernel as the first and only
userspace context.  Runs at `ULMK_PRIV_DRIVER` with `ULMK_CAP_ALL`.

Rules:
- Must never return.  Call `ulmk_thread_exit()` when bootstrapping is complete.
- Valid for the duration of `ulmk_root_thread()` only — copy `info` fields before
  spawning threads.
- Is the only thread in existence at entry; all other threads are created here
  or by threads created here.

See `docs/component_spec.md` for the component init convention and
`docs/application_development_guide.md` for a worked example.
