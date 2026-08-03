/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <string.h>
#include "device_internal.h"

#ifndef UL_UNIT_TEST

static int is_inline_w0(uint32_t w0)
{
	return ((w0 >> 16) & ULMK_DEV_F_INLINE) != 0u;
}

static void pack_inline_reply(ulmk_msg_t *reply, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t i;
	uint32_t w;

	reply->words[0] = (uint32_t)len | (ULMK_DEV_F_INLINE << 16);
	reply->words[1] = 0u;
	reply->words[2] = 0u;
	reply->words[3] = 0u;
	reply->words[4] = 0u;
	reply->words[5] = 0u;
	for (i = 0u; i < len && i < ULMK_DEV_INLINE_BYTES; i++) {
		w = i / 4u;
		reply->words[1u + w] |= ((uint32_t)p[i]) << ((i % 4u) * 8u);
	}
}

static void unpack_inline_req(const ulmk_msg_t *msg, void *buf, size_t len)
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

void ulmk_dev_serve(ulmk_ep_t ep, const struct ulmk_dev_ops *ops, void *ctx)
{
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_tid_t sender;
	int rc;
	uint32_t len;
	uint8_t inline_buf[ULMK_DEV_INLINE_BYTES];
	void *buf;
	uint32_t args[6];
	uint32_t i;

	if (ep == ULMK_EP_INVALID || !ops)
		return;

	for (;;) {
		if (ulmk_ep_recv(ep, &msg, &sender) != ULMK_OK)
			continue;

		reply.label = 0u;
		for (i = 0u; i < 6u; i++)
			reply.words[i] = 0u;
		reply.words[0] = (uint32_t)(int32_t)ULMK_EINVAL;

		switch (msg.label) {
		case ULMK_DEV_REQ_OPEN:
			rc = ops->open ? ops->open(ctx) : ULMK_ENOTSUP;
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_REQ_CLOSE:
			rc = ops->close ? ops->close(ctx) : ULMK_ENOTSUP;
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_REQ_WRITE:
			if (!ops->write) {
				reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
				break;
			}
			if (is_inline_w0(msg.words[0])) {
				len = msg.words[0] & 0xFFFFu;
				if (len > ULMK_DEV_INLINE_BYTES)
					len = ULMK_DEV_INLINE_BYTES;
				unpack_inline_req(&msg, inline_buf, len);
				rc = ops->write(ctx, inline_buf, len,
						ULMK_DEV_F_INLINE);
			} else {
				buf = (void *)(uintptr_t)msg.words[0];
				len = msg.words[1];
				rc = ops->write(ctx, buf, len, 0u);
			}
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_REQ_READ:
			if (!ops->read) {
				reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
				break;
			}
			if (is_inline_w0(msg.words[0])) {
				len = msg.words[0] & 0xFFFFu;
				if (len > ULMK_DEV_INLINE_BYTES)
					len = ULMK_DEV_INLINE_BYTES;
				rc = ops->read(ctx, inline_buf, len,
					       ULMK_DEV_F_INLINE);
				if (rc < 0) {
					reply.words[0] = (uint32_t)(int32_t)rc;
				} else {
					pack_inline_reply(&reply, inline_buf,
							  (size_t)rc);
				}
			} else {
				buf = (void *)(uintptr_t)msg.words[0];
				len = msg.words[1];
				rc = ops->read(ctx, buf, len, 0u);
				reply.words[0] = (uint32_t)(int32_t)rc;
			}
			break;

		case ULMK_DEV_REQ_INFO:
			if (!ops->info) {
				reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
				break;
			}
			len = msg.words[0];
			if (len > 5u)
				len = 5u;
			rc = ops->info(ctx, &reply.words[1], len);
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_REQ_SUBMIT:
			if (!ops->submit) {
				reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
				break;
			}
			if (is_inline_w0(msg.words[0])) {
				len = msg.words[0] & 0xFFFFu;
				if (len > ULMK_DEV_INLINE_BYTES)
					len = ULMK_DEV_INLINE_BYTES;
				unpack_inline_req(&msg, inline_buf, len);
				rc = ops->submit(ctx, inline_buf, len);
			} else if (msg.words[0] == 0u) {
				rc = ops->submit(ctx, NULL, 0u);
			} else {
				buf = (void *)(uintptr_t)msg.words[0];
				len = msg.words[1];
				rc = ops->submit(ctx, buf, len);
			}
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_REQ_WAIT:
			if (!ops->wait) {
				reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
				break;
			}
			if (msg.words[0] == 0u) {
				rc = ops->wait(ctx, NULL, 0u);
				reply.words[0] = (uint32_t)(int32_t)rc;
			} else if (is_inline_w0(msg.words[0])) {
				len = msg.words[0] & 0xFFFFu;
				if (len > ULMK_DEV_INLINE_BYTES)
					len = ULMK_DEV_INLINE_BYTES;
				rc = ops->wait(ctx, inline_buf, len);
				if (rc < 0) {
					reply.words[0] = (uint32_t)(int32_t)rc;
				} else {
					pack_inline_reply(&reply, inline_buf,
							  (size_t)rc);
				}
			} else {
				buf = (void *)(uintptr_t)msg.words[0];
				len = msg.words[1];
				rc = ops->wait(ctx, buf, len);
				reply.words[0] = (uint32_t)(int32_t)rc;
			}
			break;

		default:
			/*
			 * Class ioctl codes are labels >= ULMK_DEV_REQ_IOCTL
			 * (client sets msg.label = req).  Handler fills
			 * args[0]=rc and optional out args.
			 */
			if (msg.label < ULMK_DEV_REQ_IOCTL || !ops->ioctl) {
				reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
				break;
			}
			for (i = 0u; i < 6u; i++)
				args[i] = msg.words[i];
			rc = ops->ioctl(ctx, msg.label, args, 6u);
			for (i = 0u; i < 6u; i++)
				reply.words[i] = args[i];
			if (rc != ULMK_OK)
				reply.words[0] = (uint32_t)(int32_t)rc;
			break;
		}

		(void)ulmk_ep_reply(sender, &reply);
	}
}

#endif /* !UL_UNIT_TEST */
