/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "device_internal.h"

#ifndef UL_UNIT_TEST

static int ensure_grant(ulmk_dev_t *dev, const void *buf, size_t len)
{
	uintptr_t a = (uintptr_t)buf;
	uintptr_t g;
	int rc;

	if (!dev || !buf || len == 0u)
		return ULMK_EINVAL;
	if (dev->server == ULMK_TID_INVALID)
		return ULMK_EINVAL;

	if (dev->grant_addr) {
		g = (uintptr_t)dev->grant_addr;
		if (a >= g && (a + len) <= (g + dev->grant_size))
			return ULMK_OK;
		(void)ulmk_mem_revoke(dev->grant_addr, dev->server);
		dev->grant_addr = NULL;
		dev->grant_size = 0u;
	}

	rc = ulmk_mem_grant((void *)buf, len, dev->server,
			    ULMK_PERM_READ | ULMK_PERM_WRITE);
	if (rc == ULMK_OK) {
		dev->grant_addr = (void *)buf;
		dev->grant_size = len;
		return ULMK_OK;
	}
	/*
	 * Grant matches region base only.  Stack / interior pointers are
	 * still visible through the shared user-RAM window on every current
	 * arch — proceed without caching a grant.
	 */
	if (rc == ULMK_EPERM)
		return ULMK_OK;
	return rc;
}

static void pack_inline(ulmk_msg_t *msg, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t i;
	uint32_t w;

	/* words[0] = len | (INLINE << 16); payload in words[1..5]. */
	msg->words[0] = (uint32_t)len | (ULMK_DEV_F_INLINE << 16);
	msg->words[1] = 0u;
	msg->words[2] = 0u;
	msg->words[3] = 0u;
	msg->words[4] = 0u;
	msg->words[5] = 0u;
	for (i = 0u; i < len && i < ULMK_DEV_INLINE_BYTES; i++) {
		w = i / 4u;
		msg->words[1u + w] |= ((uint32_t)p[i]) << ((i % 4u) * 8u);
	}
}

static int is_inline_msg(const ulmk_msg_t *msg)
{
	return ((msg->words[0] >> 16) & ULMK_DEV_F_INLINE) != 0u;
}

static void unpack_inline(const ulmk_msg_t *msg, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	uint32_t i;
	uint32_t w;

	for (i = 0u; i < len && i < ULMK_DEV_INLINE_BYTES; i++) {
		w = i / 4u;
		p[i] = (uint8_t)((msg->words[1u + w] >> ((i % 4u) * 8u)) &
				 0xFFu);
	}
}

int ulmk_open(const char *path, ulmk_dev_t *dev)
{
	ulmk_msg_t msg;
	ulmk_msg_t open_msg;
	int rc;

	if (!dev || !ulmk_dev_path_ok(path))
		return ULMK_EINVAL;
	if (g_ulmk_dev_mgr_ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;

	dev->ep = ULMK_EP_INVALID;
	dev->server = ULMK_TID_INVALID;
	dev->grant_addr = NULL;
	dev->grant_size = 0u;
	dev->flags = 0u;

	msg.label = ULMK_DEV_MGR_LOOKUP;
	ulmk_dev_msg_pack_path(&msg, path);
	rc = ulmk_ep_call(g_ulmk_dev_mgr_ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	if ((int)(int32_t)msg.words[0] != ULMK_OK)
		return (int)(int32_t)msg.words[0];

	dev->ep = (ulmk_ep_t)msg.words[1];
	dev->server = (ulmk_tid_t)msg.words[2];
	dev->class = (uint16_t)msg.words[3];
	dev->instance = (uint8_t)msg.words[4];

	open_msg.label = ULMK_DEV_REQ_OPEN;
	open_msg.words[0] = 0u;
	rc = ulmk_ep_call(dev->ep, &open_msg);
	if (rc != ULMK_OK) {
		dev->ep = ULMK_EP_INVALID;
		return rc;
	}
	if ((int)(int32_t)open_msg.words[0] != ULMK_OK) {
		rc = (int)(int32_t)open_msg.words[0];
		dev->ep = ULMK_EP_INVALID;
		return rc;
	}
	return ULMK_OK;
}

int ulmk_close(ulmk_dev_t *dev)
{
	ulmk_msg_t msg;
	int rc;

	if (!dev || dev->ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;

	if (dev->grant_addr && dev->server != ULMK_TID_INVALID)
		(void)ulmk_mem_revoke(dev->grant_addr, dev->server);
	dev->grant_addr = NULL;
	dev->grant_size = 0u;

	msg.label = ULMK_DEV_REQ_CLOSE;
	msg.words[0] = 0u;
	rc = ulmk_ep_call(dev->ep, &msg);
	dev->ep = ULMK_EP_INVALID;
	dev->server = ULMK_TID_INVALID;
	if (rc != ULMK_OK)
		return rc;
	return (int)(int32_t)msg.words[0];
}

int ulmk_write(ulmk_dev_t *dev, const void *buf, size_t len)
{
	ulmk_msg_t msg;
	int rc;

	if (!dev || dev->ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;
	if (len == 0u)
		return ULMK_OK;
	if (!buf)
		return ULMK_EINVAL;

	msg.label = ULMK_DEV_REQ_WRITE;
	if (len <= ULMK_DEV_INLINE_BYTES) {
		pack_inline(&msg, buf, len);
	} else {
		rc = ensure_grant(dev, buf, len);
		if (rc != ULMK_OK)
			return rc;
		msg.words[0] = (uint32_t)(uintptr_t)buf;
		msg.words[1] = (uint32_t)len;
	}

	rc = ulmk_ep_call(dev->ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	return (int)(int32_t)msg.words[0];
}

int ulmk_read(ulmk_dev_t *dev, void *buf, size_t len)
{
	ulmk_msg_t msg;
	int rc;
	uint32_t got;

	if (!dev || dev->ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;
	if (len == 0u)
		return ULMK_OK;
	if (!buf)
		return ULMK_EINVAL;

	msg.label = ULMK_DEV_REQ_READ;
	if (len <= ULMK_DEV_INLINE_BYTES) {
		msg.words[0] = (uint32_t)len | (ULMK_DEV_F_INLINE << 16);
	} else {
		rc = ensure_grant(dev, buf, len);
		if (rc != ULMK_OK)
			return rc;
		msg.words[0] = (uint32_t)(uintptr_t)buf;
		msg.words[1] = (uint32_t)len;
	}

	rc = ulmk_ep_call(dev->ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	/*
	 * Reply: words[0] = byte count (>=0) or -errno.  Inline payloads
	 * keep the INLINE flag in the high half of words[0] from the
	 * server, or return the count alone with data in words[1..].
	 */
	if ((int)(int32_t)msg.words[0] < 0 &&
	    (int)(int32_t)msg.words[0] >= -16)
		return (int)(int32_t)msg.words[0];
	got = msg.words[0] & 0xFFFFu;
	if (is_inline_msg(&msg) || len <= ULMK_DEV_INLINE_BYTES) {
		if (got > len)
			got = (uint32_t)len;
		if (got > ULMK_DEV_INLINE_BYTES)
			got = ULMK_DEV_INLINE_BYTES;
		unpack_inline(&msg, buf, got);
	}
	return (int)got;
}

int ulmk_ioctl(ulmk_dev_t *dev, uint32_t req, uint32_t *args, uint32_t nargs)
{
	ulmk_msg_t msg;
	uint32_t i;
	int rc;

	if (!dev || dev->ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;
	if (nargs > 5u)
		return ULMK_EINVAL;

	msg.label = req;
	msg.words[0] = 0u;
	for (i = 0u; i < 6u; i++)
		msg.words[i] = 0u;
	for (i = 0u; i < nargs; i++)
		msg.words[i] = args ? args[i] : 0u;

	rc = ulmk_ep_call(dev->ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	if (args && nargs > 0u) {
		for (i = 0u; i < nargs && i < 6u; i++)
			args[i] = msg.words[i];
	}
	return (int)(int32_t)msg.words[0];
}

int ulmk_dev_info(ulmk_dev_t *dev, uint32_t *out, uint32_t nout)
{
	ulmk_msg_t msg;
	uint32_t i;
	int rc;

	if (!dev || dev->ep == ULMK_EP_INVALID || !out || nout == 0u)
		return ULMK_EINVAL;

	msg.label = ULMK_DEV_REQ_INFO;
	msg.words[0] = nout;
	rc = ulmk_ep_call(dev->ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	rc = (int)(int32_t)msg.words[0];
	if (rc != ULMK_OK)
		return rc;
	for (i = 0u; i < nout && i < 5u; i++)
		out[i] = msg.words[1u + i];
	return ULMK_OK;
}

int ulmk_dev_submit(ulmk_dev_t *dev, const void *buf, size_t len)
{
	ulmk_msg_t msg;
	int rc;

	if (!dev || dev->ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;
	if (len > 0u && !buf)
		return ULMK_EINVAL;

	if (len > ULMK_DEV_INLINE_BYTES) {
		rc = ensure_grant(dev, buf, len);
		if (rc != ULMK_OK)
			return rc;
		msg.words[0] = (uint32_t)(uintptr_t)buf;
		msg.words[1] = (uint32_t)len;
	} else if (len > 0u) {
		pack_inline(&msg, buf, len);
	} else {
		msg.words[0] = 0u;
	}
	msg.label = ULMK_DEV_REQ_SUBMIT;
	rc = ulmk_ep_call(dev->ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	return (int)(int32_t)msg.words[0];
}

int ulmk_dev_wait(ulmk_dev_t *dev, void *buf, size_t len)
{
	ulmk_msg_t msg;
	int rc;
	uint32_t got;

	if (!dev || dev->ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;

	msg.label = ULMK_DEV_REQ_WAIT;
	if (len > 0u && buf) {
		if (len <= ULMK_DEV_INLINE_BYTES) {
			msg.words[0] = (uint32_t)len | (ULMK_DEV_F_INLINE << 16);
		} else {
			rc = ensure_grant(dev, buf, len);
			if (rc != ULMK_OK)
				return rc;
			msg.words[0] = (uint32_t)(uintptr_t)buf;
			msg.words[1] = (uint32_t)len;
		}
	} else {
		msg.words[0] = 0u;
	}

	rc = ulmk_ep_call(dev->ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	if ((int)(int32_t)msg.words[0] < 0 &&
	    (int)(int32_t)msg.words[0] >= -16)
		return (int)(int32_t)msg.words[0];
	got = msg.words[0] & 0xFFFFu;
	if (buf && len > 0u &&
	    (is_inline_msg(&msg) || len <= ULMK_DEV_INLINE_BYTES)) {
		if (got > len)
			got = (uint32_t)len;
		if (got > ULMK_DEV_INLINE_BYTES)
			got = ULMK_DEV_INLINE_BYTES;
		unpack_inline(&msg, buf, got);
	}
	return (int)got;
}

#endif /* !UL_UNIT_TEST */
