/* SPDX-License-Identifier: MIT */
#ifndef ULMK_DEVICE_H
#define ULMK_DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include <ulmk/microkernel.h>

#define ULMK_DEV_MAX			16u
/*
 * Inline payload uses msg.words[1..5] (5 × 4 bytes).  Larger transfers
 * grant the caller's buffer to the driver once and cache the range.
 * Pathnames share that window so they never cross domains as pointers.
 */
#define ULMK_DEV_INLINE_BYTES		20u
#define ULMK_DEV_PATH_MAX		(ULMK_DEV_INLINE_BYTES + 1u)

/* Well-known device classes used by the manager itself / tests.
 * Product classes (display, input, can, …) are policy: they live in
 * ulmk_apps/ulmk_device_classes (or a board), never in this component.
 */
#define ULMK_DEV_CLASS_NONE		0u
#define ULMK_DEV_CLASS_ECHO		0xFEu	/* test / dummy */

/* Protocol opcodes spoken by every device server. */
#define ULMK_DEV_REQ_OPEN		0x01u
#define ULMK_DEV_REQ_CLOSE		0x02u
#define ULMK_DEV_REQ_READ		0x03u
#define ULMK_DEV_REQ_WRITE		0x04u
#define ULMK_DEV_REQ_INFO		0x05u
#define ULMK_DEV_REQ_SUBMIT		0x06u
#define ULMK_DEV_REQ_WAIT		0x07u
#define ULMK_DEV_REQ_IOCTL		0x40u

/* words[2] flag on READ/WRITE: payload lives in words[1..] of the reply/req. */
#define ULMK_DEV_F_INLINE		(1u << 0)

typedef struct {
	ulmk_ep_t  ep;
	ulmk_tid_t server;
	uint16_t   class;
	uint8_t    instance;
	uint8_t    flags;
	void      *grant_addr;
	size_t     grant_size;
} ulmk_dev_t;

/*
 * Per-device operations — filled by the adapter/server thread.
 * NULL entry → ULMK_ENOTSUP for that opcode.
 * One device = one thread calling ulmk_dev_serve() (no shared demux).
 */
struct ulmk_dev_ops {
	int (*open)(void *ctx);
	int (*close)(void *ctx);
	int (*read)(void *ctx, void *buf, size_t len, uint32_t flags);
	int (*write)(void *ctx, const void *buf, size_t len, uint32_t flags);
	int (*info)(void *ctx, uint32_t *out, uint32_t nout);
	int (*ioctl)(void *ctx, uint32_t req, uint32_t *args, uint32_t nargs);
	int (*submit)(void *ctx, const void *buf, size_t len);
	int (*wait)(void *ctx, void *buf, size_t len);
};

/**
 * @brief Device server loop: recv → ops → reply.  Does not return.
 */
void ulmk_dev_serve(ulmk_ep_t ep, const struct ulmk_dev_ops *ops, void *ctx);

/**
 * @brief Spawn the device-manager server (call once from root / board init).
 */
int ulmk_dev_manager_init(void);

/**
 * @brief Publish a pathname → driver endpoint mapping.
 */
int ulmk_dev_register(const char *path, ulmk_ep_t ep, ulmk_tid_t server,
		      uint16_t class, uint8_t instance);

int ulmk_dev_unregister(const char *path);

int ulmk_open(const char *path, ulmk_dev_t *dev);
int ulmk_close(ulmk_dev_t *dev);
int ulmk_read(ulmk_dev_t *dev, void *buf, size_t len);
int ulmk_write(ulmk_dev_t *dev, const void *buf, size_t len);
int ulmk_ioctl(ulmk_dev_t *dev, uint32_t req, uint32_t *args, uint32_t nargs);
int ulmk_dev_info(ulmk_dev_t *dev, uint32_t *out, uint32_t nout);
int ulmk_dev_submit(ulmk_dev_t *dev, const void *buf, size_t len);
int ulmk_dev_wait(ulmk_dev_t *dev, void *buf, size_t len);

#endif /* ULMK_DEVICE_H */
