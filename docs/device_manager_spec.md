# ulmk — Device Manager Specification

**Status:** Implemented (userspace component `ulmk_device_manager`)
**Layer:** Mechanism (kernel tree) + class policy (`ulmk_apps/ulmk_device_classes`)

> Kernel is mechanism; product device contracts are policy.  The device
> manager is the one userspace exception that may live in the kernel repo
> because it is a pathname / IPC facility aligned with the microkernel —
> not a display/CAN/PWM driver.

---

## 1. Roles

| Piece | Repo | Responsibility |
|-------|------|----------------|
| `ulmk_device_manager` | `ulmk/components/` | Name service + generic `open`/`read`/`write`/`ioctl`/`info`/`submit`/`wait` + `ulmk_dev_ops` / `ulmk_dev_serve` |
| `ulmk_device_classes` | `ulmk_apps/` | Class IDs, ioctl codes, wire structs, thin helpers (`ulmk_disp_*`, `ulmk_can_*`, …) |
| `drivers/<name>_dm/` | board tree | One adapter thread per device; fills `ulmk_dev_ops`, calls legacy client APIs |
| App / demo | `ulmk_apps` or board `components/` | `ulmk_open("/dev/…")` + class helpers only — never `#include <display.h>` etc. |

Forbidden in `ulmk`: `ulmk_device_display.h`, `ulmk_device_can.h`, and any other class header.

---

## 2. Runtime model

```
app ──ulmk_open("/dev/foo")──► manager (LOOKUP) ──► handle {ep, server, class}
app ──ulmk_write/read/ioctl──► adapter ep ──ulmk_dev_serve──► ops
ops ──legacy client──► hardware server thread
```

Rules:

1. **One thread per device** (adapter). No demux of N devices on one thread.
2. Manager thread does **only** LOOKUP / REGISTER / UNREGISTER.
3. Hot path after `open` is `ep_call` to the adapter (one extra hop over legacy is OK).
4. Do not spawn legacy `*_init()` from inside an ops handler while holding a client `ep_call` — bring hardware up from root / `*_dm_init` / a bind helper.

---

## 3. Generic API (`ulmk_device.h`)

### Types

- `ulmk_dev_t` — client handle (ep, server tid, class, optional grant cache).
- `struct ulmk_dev_ops` — function table; `NULL` entry → `ULMK_ENOTSUP`.

### Manager

```c
int ulmk_dev_manager_init(void);
int ulmk_dev_register(const char *path, ulmk_ep_t ep, ulmk_tid_t server,
		      uint16_t class, uint8_t instance);
int ulmk_dev_unregister(const char *path);
```

### Client

```c
int ulmk_open(const char *path, ulmk_dev_t *dev);
int ulmk_close(ulmk_dev_t *dev);
int ulmk_read(ulmk_dev_t *dev, void *buf, size_t len);
int ulmk_write(ulmk_dev_t *dev, const void *buf, size_t len);
int ulmk_ioctl(ulmk_dev_t *dev, uint32_t req, uint32_t *args, uint32_t nargs);
int ulmk_dev_info(ulmk_dev_t *dev, uint32_t *out, uint32_t nout);
int ulmk_dev_submit(ulmk_dev_t *dev, const void *buf, size_t len);
int ulmk_dev_wait(ulmk_dev_t *dev, void *buf, size_t len);
```

### Server loop

```c
void ulmk_dev_serve(ulmk_ep_t ep, const struct ulmk_dev_ops *ops, void *ctx);
```

Wire protocol: request label = `ULMK_DEV_REQ_*` or class ioctl `>= ULMK_DEV_REQ_IOCTL`. Payloads ≤ `ULMK_DEV_INLINE_BYTES` (20) travel inline; larger buffers use `ulmk_mem_grant` to the server tid.

Built-in class for tests only: `ULMK_DEV_CLASS_ECHO`.

---

## 4. Class contracts (policy)

Headers under `ulmk_apps/ulmk_device_classes/include/`:

| Header | Path example | Floor |
|--------|--------------|--------|
| `ulmk_device_display.h` | `/dev/disp0` | `write` = present; `INFO` geom; ioctl `GET_FB` / `ON` |
| `ulmk_device_input.h` | `/dev/input0` | `read` → `ulmk_input_event` |
| `ulmk_device_can.h` | `/dev/can0` | `write`/`read` frames |
| `ulmk_device_pwm.h` | `/dev/pwm0` | ioctl CONFIG / SET_DUTY / ENABLE |
| `ulmk_device_adc.h` | `/dev/adc0` | `read` sample; ioctl CONFIG |
| `ulmk_device_gpio.h` | `/dev/gpio0` | `write`/`read` pin value; ioctl CONFIG |

Enable the component (or depend via `REQUIRES ulmk_device_classes`) so apps see the include path. Boards that compile adapters into the image also append that include dir from `board.cmake`.

---

## 5. Writing a DM adapter (board)

1. Keep the legacy driver (`drivers/foo/`) unchanged for existing demos.
2. Add `drivers/foo_dm/` with one server thread calling `ulmk_dev_serve`.
3. Fill only the ops you support; leave the rest `NULL`.
4. `foo_dm_init()` (from **root**): create ep, spawn DM thread, optionally start legacy `foo_init()` (or expose `foo_dm_bind_hw()` if bring-up must happen after `open`).
5. Put adapter state in `.user_bss` on TriCore (plain BSS can land in kernel SRAM and trap under `PRIV_DRIVER`).
6. Register from `board_devices_register*()` or the demo root: `ulmk_dev_register("/dev/foo0", ep, tid, CLASS, inst)`.
7. Demo app: `#include <ulmk_device_*.h>` only — zero legacy headers.

Minimal ops skeleton:

```c
static const struct ulmk_dev_ops g_ops = {
	.open = dm_open,
	.close = dm_close,
	.read = dm_read,	/* or NULL */
	.write = dm_write,
	.info = NULL,
	.ioctl = dm_ioctl,
	.submit = NULL,
	.wait = NULL,
};

static void foo_dm_server(void *arg)
{
	ulmk_dev_serve(g_ep, &g_ops, arg);
}
```

---

## 6. Enabling in a build

```bash
python3 tools/dev.py build --board <board> --no-components \
	--component lvgl_benchmark
# pulls ulmk_device_manager + ulmk_device_classes via REQUIRES
```

SDK packaging ships **only** `ulmk_device.h` (+ client/serve objects) under the device_manager component. Class headers are not part of the kernel SDK.

---

## 7. Related docs

- `docs/application_development_guide.md` — how apps and boards use the manager
- `docs/component_spec.md` — component packaging rules
- `.cursor/rules/driver-client-server.mdc` — legacy driver shape (1 server thread / instance)
