/* SPDX-License-Identifier: MIT */
/*
 * In-process display + input protocol dummies for host unit tests.
 */
#include <stdint.h>
#include <string.h>
#include <ulmk_device.h>
#include <ulmk_device_display.h>
#include <ulmk_device_input.h>
#include "protocol_dummy.h"

static uint16_t g_disp_fb0[8];
static uint16_t g_disp_fb1[8];
static int g_disp_on;
static uintptr_t g_last_present;
static uint32_t g_last_n_rects;

int proto_disp_handle(struct proto_msg *msg)
{
	uint32_t idx;
	uint32_t len;
	uint32_t i;
	struct ulmk_disp_present_hdr hdr;
	const uint8_t *src;

	switch (msg->label) {
	case ULMK_DEV_REQ_OPEN:
	case ULMK_DEV_REQ_CLOSE:
		msg->words[0] = (uint32_t)ULMK_OK;
		return ULMK_OK;
	case ULMK_DEV_REQ_READ:
	case ULMK_DEV_REQ_SUBMIT:
	case ULMK_DEV_REQ_WAIT:
		msg->words[0] = (uint32_t)ULMK_ENOTSUP;
		return ULMK_ENOTSUP;
	case ULMK_DEV_REQ_WRITE:
		if ((msg->words[0] >> 16) & ULMK_DEV_F_INLINE) {
			len = msg->words[0] & 0xFFFFu;
			if (len < sizeof(hdr)) {
				msg->words[0] = (uint32_t)ULMK_EINVAL;
				return ULMK_EINVAL;
			}
			memset(&hdr, 0, sizeof(hdr));
			src = (const uint8_t *)&msg->words[1];
			for (i = 0u; i < sizeof(hdr) && i < len; i++)
				((uint8_t *)&hdr)[i] = src[i];
			g_last_present = (uintptr_t)hdr.fb_ptr;
			g_last_n_rects = hdr.n_rects;
			msg->words[0] = (uint32_t)ULMK_OK;
			return ULMK_OK;
		}
		msg->words[0] = (uint32_t)ULMK_EINVAL;
		return ULMK_EINVAL;
	case ULMK_DEV_REQ_INFO:
		msg->words[0] = (uint32_t)ULMK_OK;
		msg->words[1] = 1024u;
		msg->words[2] = 600u;
		msg->words[3] = 16u;
		msg->words[4] = 2048u;
		msg->words[5] = 2u;
		return ULMK_OK;
	case ULMK_DISP_IOCTL_GET_FB:
		idx = msg->words[0];
		if (idx > 1u) {
			msg->words[0] = (uint32_t)ULMK_EINVAL;
			return ULMK_EINVAL;
		}
		msg->words[0] = (uint32_t)ULMK_OK;
		msg->words[1] = (uint32_t)(uintptr_t)
			(idx == 0u ? g_disp_fb0 : g_disp_fb1);
		return ULMK_OK;
	case ULMK_DISP_IOCTL_ON:
		g_disp_on = msg->words[0] ? 1 : 0;
		msg->words[0] = (uint32_t)ULMK_OK;
		return ULMK_OK;
	default:
		msg->words[0] = (uint32_t)ULMK_EINVAL;
		return ULMK_EINVAL;
	}
}

int proto_input_handle(struct proto_msg *msg)
{
	switch (msg->label) {
	case ULMK_DEV_REQ_OPEN:
	case ULMK_DEV_REQ_CLOSE:
		msg->words[0] = (uint32_t)ULMK_OK;
		return ULMK_OK;
	case ULMK_DEV_REQ_WRITE:
	case ULMK_DEV_REQ_INFO:
	case ULMK_DEV_REQ_SUBMIT:
		msg->words[0] = (uint32_t)ULMK_ENOTSUP;
		return ULMK_ENOTSUP;
	case ULMK_DEV_REQ_READ:
		msg->words[0] = (uint32_t)sizeof(struct ulmk_input_event) |
				(ULMK_DEV_F_INLINE << 16);
		msg->words[1] = (uint32_t)(uint16_t)100 |
				((uint32_t)(uint16_t)200 << 16);
		msg->words[2] = (uint32_t)ULMK_INPUT_STATE_PRESSED;
		return ULMK_OK;
	case ULMK_DEV_REQ_WAIT:
		msg->words[0] = (uint32_t)ULMK_ENOTSUP;
		return ULMK_ENOTSUP;
	default:
		msg->words[0] = (uint32_t)ULMK_EINVAL;
		return ULMK_EINVAL;
	}
}

int proto_disp_on_state(void)
{
	return g_disp_on;
}

uintptr_t proto_disp_last_present(void)
{
	return g_last_present;
}

uint32_t proto_disp_last_n_rects(void)
{
	return g_last_n_rects;
}

void *proto_disp_fb0(void)
{
	return g_disp_fb0;
}

void *proto_disp_fb1(void)
{
	return g_disp_fb1;
}
