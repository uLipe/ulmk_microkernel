/* SPDX-License-Identifier: MIT */
/*
 * device_manager — echo / display / input dummies via ulmk_device_manager
 * (sdk_suite / QEMU).
 */
#include "sdk_test_util.h"
#include <ulmk_device.h>
#include <ulmk_device_display.h>
#include <ulmk_device_input.h>
#include "device_internal.h"
#include <string.h>

#define ECHO_PATH	"/dev/echo0"
#define DISP_PATH	"/dev/disp0"
#define INPUT_PATH	"/dev/input0"
#define ECHO_BUF	64u
#define IOCTL_PING	(ULMK_DEV_REQ_IOCTL + 1u)
#define DISP_W		320u
#define DISP_H		240u
#define DISP_BPP	16u
#define DISP_STRIDE	640u
#define DISP_N_FB	2u
#define DISP_FB_WORDS	8u

static int g_pass;
static int g_fail;
static ulmk_ep_t g_echo_ep = ULMK_EP_INVALID;
static uint8_t g_echo_buf[ECHO_BUF];
static uint32_t g_echo_len;
static ulmk_tid_t g_echo_tid = ULMK_TID_INVALID;

static ulmk_ep_t g_disp_ep = ULMK_EP_INVALID;
static ulmk_tid_t g_disp_tid = ULMK_TID_INVALID;
static uint16_t g_disp_fb[2][DISP_FB_WORDS];
static int g_disp_on;

static ulmk_ep_t g_input_ep = ULMK_EP_INVALID;
static ulmk_tid_t g_input_tid = ULMK_TID_INVALID;

static void check(const char *name, int ok)
{
	sdk_puts(ok ? ".ok " : ".FAIL ");
	sdk_puts(name);
	sdk_puts("\n");
	if (ok)
		g_pass++;
	else
		g_fail++;
}

#define CHECK(name, cond) check((name), (cond) ? 1 : 0)

static void echo_server(void *arg)
{
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_tid_t sender;
	uint32_t len;
	uint32_t i;
	const uint8_t *src;
	uint8_t *dst;

	(void)arg;
	for (;;) {
		if (ulmk_ep_recv(g_echo_ep, &msg, &sender) != ULMK_OK)
			continue;

		reply.label = 0u;
		reply.words[0] = (uint32_t)(int32_t)ULMK_EINVAL;
		reply.words[1] = 0u;
		reply.words[2] = 0u;
		reply.words[3] = 0u;
		reply.words[4] = 0u;
		reply.words[5] = 0u;

		switch (msg.label) {
		case ULMK_DEV_REQ_OPEN:
			g_echo_len = 0u;
			reply.words[0] = (uint32_t)ULMK_OK;
			break;

		case ULMK_DEV_REQ_CLOSE:
			g_echo_len = 0u;
			reply.words[0] = (uint32_t)ULMK_OK;
			break;

		case ULMK_DEV_REQ_WRITE:
			if ((msg.words[0] >> 16) & ULMK_DEV_F_INLINE) {
				len = msg.words[0] & 0xFFFFu;
				if (len > ECHO_BUF)
					len = ECHO_BUF;
				for (i = 0u; i < len; i++) {
					uint32_t w = i / 4u;
					g_echo_buf[i] = (uint8_t)
						((msg.words[1u + w] >>
						  ((i % 4u) * 8u)) & 0xFFu);
				}
				g_echo_len = len;
				reply.words[0] = (uint32_t)ULMK_OK;
			} else {
				src = (const uint8_t *)(uintptr_t)msg.words[0];
				len = msg.words[1];
				if (!src || len == 0u || len > ECHO_BUF) {
					reply.words[0] =
						(uint32_t)(int32_t)ULMK_EINVAL;
					break;
				}
				for (i = 0u; i < len; i++)
					g_echo_buf[i] = src[i];
				g_echo_len = len;
				reply.words[0] = (uint32_t)ULMK_OK;
			}
			break;

		case ULMK_DEV_REQ_READ:
			if ((msg.words[0] >> 16) & ULMK_DEV_F_INLINE) {
				len = msg.words[0] & 0xFFFFu;
				if (len > g_echo_len)
					len = g_echo_len;
				if (len > ULMK_DEV_INLINE_BYTES)
					len = ULMK_DEV_INLINE_BYTES;
				reply.words[0] = len | (ULMK_DEV_F_INLINE << 16);
				for (i = 0u; i < len; i++) {
					uint32_t w = i / 4u;
					reply.words[1u + w] |=
						((uint32_t)g_echo_buf[i]) <<
						((i % 4u) * 8u);
				}
			} else {
				dst = (uint8_t *)(uintptr_t)msg.words[0];
				len = msg.words[1];
				if (!dst || len == 0u) {
					reply.words[0] =
						(uint32_t)(int32_t)ULMK_EINVAL;
					break;
				}
				if (len > g_echo_len)
					len = g_echo_len;
				for (i = 0u; i < len; i++)
					dst[i] = g_echo_buf[i];
				reply.words[0] = len;
			}
			break;

		case ULMK_DEV_REQ_INFO:
			reply.words[0] = (uint32_t)ULMK_OK;
			reply.words[1] = ECHO_BUF;
			reply.words[2] = g_echo_len;
			break;

		case IOCTL_PING:
			reply.words[0] = (uint32_t)ULMK_OK;
			reply.words[1] = 0xA5A5u;
			break;

		default:
			reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
			break;
		}
		ulmk_ep_reply(sender, &reply);
	}
}

static void disp_server(void *arg)
{
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_tid_t sender;
	uint32_t idx;
	uintptr_t fb;

	(void)arg;
	for (;;) {
		if (ulmk_ep_recv(g_disp_ep, &msg, &sender) != ULMK_OK)
			continue;

		reply.label = 0u;
		reply.words[0] = (uint32_t)(int32_t)ULMK_EINVAL;
		reply.words[1] = 0u;
		reply.words[2] = 0u;
		reply.words[3] = 0u;
		reply.words[4] = 0u;
		reply.words[5] = 0u;

		switch (msg.label) {
		case ULMK_DEV_REQ_OPEN:
		case ULMK_DEV_REQ_CLOSE:
			reply.words[0] = (uint32_t)ULMK_OK;
			break;

		case ULMK_DEV_REQ_READ:
		case ULMK_DEV_REQ_SUBMIT:
		case ULMK_DEV_REQ_WAIT:
			reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
			break;

		case ULMK_DEV_REQ_WRITE: {
			struct ulmk_disp_present_hdr hdr;
			const uint8_t *src;
			uint32_t i;
			uint32_t plen;

			if (!((msg.words[0] >> 16) & ULMK_DEV_F_INLINE)) {
				reply.words[0] =
					(uint32_t)(int32_t)ULMK_EINVAL;
				break;
			}
			plen = msg.words[0] & 0xFFFFu;
			if (plen < sizeof(hdr)) {
				reply.words[0] =
					(uint32_t)(int32_t)ULMK_EINVAL;
				break;
			}
			src = (const uint8_t *)&msg.words[1];
			for (i = 0u; i < sizeof(hdr); i++)
				((uint8_t *)&hdr)[i] = src[i];
			fb = (uintptr_t)hdr.fb_ptr;
			if (fb != (uintptr_t)g_disp_fb[0] &&
			    fb != (uintptr_t)g_disp_fb[1]) {
				reply.words[0] =
					(uint32_t)(int32_t)ULMK_EINVAL;
				break;
			}
			(void)hdr.n_rects;
			reply.words[0] = (uint32_t)ULMK_OK;
			break;
		}

		case ULMK_DEV_REQ_INFO:
			reply.words[0] = (uint32_t)ULMK_OK;
			reply.words[1] = DISP_W;
			reply.words[2] = DISP_H;
			reply.words[3] = DISP_BPP;
			reply.words[4] = DISP_STRIDE;
			reply.words[5] = DISP_N_FB;
			break;

		case ULMK_DISP_IOCTL_GET_FB:
			idx = msg.words[0];
			if (idx > 1u) {
				reply.words[0] =
					(uint32_t)(int32_t)ULMK_EINVAL;
				break;
			}
			reply.words[0] = (uint32_t)ULMK_OK;
			reply.words[1] = (uint32_t)(uintptr_t)g_disp_fb[idx];
			break;

		case ULMK_DISP_IOCTL_ON:
			g_disp_on = msg.words[0] ? 1 : 0;
			reply.words[0] = (uint32_t)ULMK_OK;
			break;

		default:
			reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
			break;
		}
		ulmk_ep_reply(sender, &reply);
	}
}

static void input_server(void *arg)
{
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_tid_t sender;
	struct ulmk_input_event ev;

	(void)arg;
	for (;;) {
		if (ulmk_ep_recv(g_input_ep, &msg, &sender) != ULMK_OK)
			continue;

		reply.label = 0u;
		reply.words[0] = (uint32_t)(int32_t)ULMK_EINVAL;
		reply.words[1] = 0u;
		reply.words[2] = 0u;
		reply.words[3] = 0u;
		reply.words[4] = 0u;
		reply.words[5] = 0u;

		switch (msg.label) {
		case ULMK_DEV_REQ_OPEN:
		case ULMK_DEV_REQ_CLOSE:
			reply.words[0] = (uint32_t)ULMK_OK;
			break;

		case ULMK_DEV_REQ_WRITE:
		case ULMK_DEV_REQ_INFO:
		case ULMK_DEV_REQ_SUBMIT:
		case ULMK_DEV_REQ_WAIT:
			reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
			break;

		case ULMK_DEV_REQ_READ:
			ev.x = 11;
			ev.y = 22;
			ev.state = ULMK_INPUT_STATE_PRESSED;
			ev._pad[0] = 0u;
			ev._pad[1] = 0u;
			ev._pad[2] = 0u;
			reply.words[0] = (uint32_t)sizeof(ev) |
					(ULMK_DEV_F_INLINE << 16);
			reply.words[1] = (uint32_t)(uint16_t)ev.x |
					((uint32_t)(uint16_t)ev.y << 16);
			reply.words[2] = (uint32_t)ev.state;
			break;

		default:
			reply.words[0] = (uint32_t)(int32_t)ULMK_ENOTSUP;
			break;
		}
		ulmk_ep_reply(sender, &reply);
	}
}

static int start_echo(void)
{
	ulmk_thread_attr_t attr = { 0 };

	g_echo_ep = ulmk_ep_create();
	if (g_echo_ep == ULMK_EP_INVALID)
		return ULMK_ENOMEM;

	attr.name = "echo0";
	attr.entry = echo_server;
	attr.arg = NULL;
	attr.priority = 2u;
	attr.stack_size = 2048u;
	attr.privilege = ULMK_PRIV_DRIVER;
	g_echo_tid = ulmk_thread_create(&attr);
	if (g_echo_tid == ULMK_TID_INVALID)
		return ULMK_ENOMEM;
	return ULMK_OK;
}

static int start_disp(void)
{
	ulmk_thread_attr_t attr = { 0 };

	g_disp_ep = ulmk_ep_create();
	if (g_disp_ep == ULMK_EP_INVALID)
		return ULMK_ENOMEM;

	attr.name = "disp0";
	attr.entry = disp_server;
	attr.arg = NULL;
	attr.priority = 2u;
	attr.stack_size = 2048u;
	attr.privilege = ULMK_PRIV_DRIVER;
	g_disp_tid = ulmk_thread_create(&attr);
	if (g_disp_tid == ULMK_TID_INVALID)
		return ULMK_ENOMEM;
	return ULMK_OK;
}

static int start_input(void)
{
	ulmk_thread_attr_t attr = { 0 };

	g_input_ep = ulmk_ep_create();
	if (g_input_ep == ULMK_EP_INVALID)
		return ULMK_ENOMEM;

	attr.name = "input0";
	attr.entry = input_server;
	attr.arg = NULL;
	attr.priority = 2u;
	attr.stack_size = 2048u;
	attr.privilege = ULMK_PRIV_DRIVER;
	g_input_tid = ulmk_thread_create(&attr);
	if (g_input_tid == ULMK_TID_INVALID)
		return ULMK_ENOMEM;
	return ULMK_OK;
}

void ulmk_root_thread(const ulmk_boot_info_t *info)
{
	ulmk_dev_t dev;
	uint8_t small[8];
	uint8_t out[48];
	uint32_t info_w[4];
	uint32_t ioctl_args[2];
	uint32_t w;
	uint32_t h;
	uint32_t bpp;
	uint32_t stride;
	uint32_t n_fb;
	void *fb0;
	void *fb1;
	struct ulmk_disp_rect rect;
	struct ulmk_input_event ev;
	int n;
	int i;

	board_services_init(info);
	sdk_puts("device_manager: start\n");
	g_pass = 0;
	g_fail = 0;

	CHECK("mgr_init", ulmk_dev_manager_init() == ULMK_OK);
	CHECK("echo_start", start_echo() == ULMK_OK);
	CHECK("register",
	      ulmk_dev_register(ECHO_PATH, g_echo_ep, g_echo_tid,
				ULMK_DEV_CLASS_ECHO, 0u) == ULMK_OK);
	CHECK("register_dup",
	      ulmk_dev_register(ECHO_PATH, g_echo_ep, g_echo_tid,
				ULMK_DEV_CLASS_ECHO, 0u) != ULMK_OK);

	CHECK("open_missing",
	      ulmk_open("/dev/nope", &dev) == ULMK_ESRCH);
	CHECK("open", ulmk_open(ECHO_PATH, &dev) == ULMK_OK);

	for (i = 0; i < 8; i++)
		small[i] = (uint8_t)(0x10u + i);
	CHECK("write_inline", ulmk_write(&dev, small, 8u) == ULMK_OK);
	memset(out, 0, sizeof(out));
	n = ulmk_read(&dev, out, 8u);
	CHECK("read_inline", n == 8 && out[0] == 0x10u && out[7] == 0x17u);

	/*
	 * Large payload: map a dedicated anon block so grant can take the
	 * region base (stack interiors are EPERM).
	 */
	{
		uint8_t *mapped;
		int j;

		mapped = (uint8_t *)ulmk_mem_map(NULL, 64u,
						 ULMK_PERM_READ | ULMK_PERM_WRITE,
						 ULMK_MMAP_ANON);
		CHECK("map_big", sdk_map_ok(mapped));
		if (!sdk_map_ok(mapped))
			goto report;
		for (j = 0; j < 48; j++)
			mapped[j] = (uint8_t)j;
		CHECK("write_grant", ulmk_write(&dev, mapped, 48u) == ULMK_OK);
		memset(out, 0, sizeof(out));
		n = ulmk_read(&dev, out, 48u);
		CHECK("read_grant", n == 48 && out[0] == 0u && out[47] == 47u);
		mapped[0] = 0xAAu;
		CHECK("write_cache", ulmk_write(&dev, mapped, 48u) == ULMK_OK);
		CHECK("unmap_big", ulmk_mem_unmap(mapped, 64u) == ULMK_OK);
	}

	info_w[0] = 0u;
	CHECK("info", ulmk_dev_info(&dev, info_w, 2u) == ULMK_OK);
	CHECK("info_cap", info_w[0] == ECHO_BUF);

	ioctl_args[0] = 0u;
	ioctl_args[1] = 0u;
	CHECK("ioctl_ping",
	      ulmk_ioctl(&dev, IOCTL_PING, ioctl_args, 2u) == ULMK_OK);
	CHECK("ioctl_val", ioctl_args[1] == 0xA5A5u);

	CHECK("write_null", ulmk_write(&dev, NULL, 4u) == ULMK_EINVAL);
	CHECK("close", ulmk_close(&dev) == ULMK_OK);
	CHECK("close_dup", ulmk_close(&dev) == ULMK_EINVAL);

	CHECK("unregister", ulmk_dev_unregister(ECHO_PATH) == ULMK_OK);
	CHECK("open_after_unreg",
	      ulmk_open(ECHO_PATH, &dev) == ULMK_ESRCH);

	/* --- display class --- */
	CHECK("disp_start", start_disp() == ULMK_OK);
	CHECK("disp_register",
	      ulmk_dev_register(DISP_PATH, g_disp_ep, g_disp_tid,
				ULMK_DEV_CLASS_DISPLAY, 0u) == ULMK_OK);
	CHECK("disp_open", ulmk_open(DISP_PATH, &dev) == ULMK_OK);
	w = 0u;
	h = 0u;
	bpp = 0u;
	stride = 0u;
	n_fb = 0u;
	CHECK("disp_info",
	      ulmk_disp_info(&dev, &w, &h, &bpp, &stride, &n_fb) == ULMK_OK);
	CHECK("disp_geom",
	      w == DISP_W && h == DISP_H && bpp == DISP_BPP &&
	      stride == DISP_STRIDE && n_fb == DISP_N_FB);
	fb0 = NULL;
	fb1 = NULL;
	CHECK("disp_get_fb0", ulmk_disp_get_fb(&dev, 0u, &fb0) == ULMK_OK);
	CHECK("disp_fb0", fb0 == (void *)g_disp_fb[0]);
	CHECK("disp_get_fb1", ulmk_disp_get_fb(&dev, 1u, &fb1) == ULMK_OK);
	CHECK("disp_fb1", fb1 == (void *)g_disp_fb[1] && fb1 != fb0);
	CHECK("disp_on", ulmk_disp_on(&dev, 1) == ULMK_OK && g_disp_on == 1);
	rect.x = 0;
	rect.y = 0;
	rect.w = 16;
	rect.h = 16;
	CHECK("disp_present",
	      ulmk_disp_write_present(&dev, fb0, &rect, 1u) == ULMK_OK);
	CHECK("disp_close", ulmk_close(&dev) == ULMK_OK);
	CHECK("disp_unregister", ulmk_dev_unregister(DISP_PATH) == ULMK_OK);

	/* --- input class --- */
	CHECK("input_start", start_input() == ULMK_OK);
	CHECK("input_register",
	      ulmk_dev_register(INPUT_PATH, g_input_ep, g_input_tid,
				ULMK_DEV_CLASS_INPUT, 0u) == ULMK_OK);
	CHECK("input_open", ulmk_open(INPUT_PATH, &dev) == ULMK_OK);
	memset(&ev, 0, sizeof(ev));
	n = ulmk_input_read(&dev, &ev);
	CHECK("input_read",
	      n == (int)sizeof(ev) && ev.x == 11 && ev.y == 22 &&
	      ev.state == ULMK_INPUT_STATE_PRESSED);
	CHECK("input_close", ulmk_close(&dev) == ULMK_OK);
	CHECK("input_unregister",
	      ulmk_dev_unregister(INPUT_PATH) == ULMK_OK);

report:
	sdk_puts("device_manager: pass=");
	sdk_put_u32((uint32_t)g_pass);
	sdk_puts(" fail=");
	sdk_put_u32((uint32_t)g_fail);
	sdk_puts("\n");
	sdk_puts(g_fail == 0 ? "device_manager: PASS\n" :
			       "device_manager: FAIL\n");
	ulmk_thread_exit();
}
