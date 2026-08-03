/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define UL_UNIT_TEST 1

#include "device_internal.h"
#include "protocol_dummy.h"
#include <ulmk_device.h>
#include <ulmk_device_display.h>
#include <ulmk_device_input.h>

static int tests_run;
static int tests_failed;

#define CHECK(cond, msg) \
	do { \
		tests_run++; \
		if (!(cond)) { \
			tests_failed++; \
			printf("  FAIL [line %d] %s\n", __LINE__, (msg)); \
		} else { \
			printf("  PASS %s\n", (msg)); \
		} \
	} while (0)

static void test_path_ok(void)
{
	char longp[ULMK_DEV_INLINE_BYTES + 4];
	uint32_t i;

	CHECK(ulmk_dev_path_ok("/dev/echo0") == 1, "path ok");
	CHECK(ulmk_dev_path_ok(NULL) == 0, "null path");
	CHECK(ulmk_dev_path_ok("") == 0, "empty path");
	CHECK(ulmk_dev_path_ok("dev/echo") == 0, "no leading slash");
	CHECK(ulmk_dev_path_ok("/") == 0, "slash only");

	longp[0] = '/';
	for (i = 1u; i < ULMK_DEV_INLINE_BYTES; i++)
		longp[i] = 'a';
	longp[ULMK_DEV_INLINE_BYTES] = '\0';
	CHECK(ulmk_dev_path_ok(longp) == 0, "path too long for wire");
}

static void test_register_lookup(void)
{
	struct ulmk_dev_entry tab[4];
	struct ulmk_dev_entry out;
	int rc;

	memset(tab, 0, sizeof(tab));

	rc = ulmk_dev_table_register(tab, 4, "/dev/echo0", (ulmk_ep_t)10u,
				     (ulmk_tid_t)20u, ULMK_DEV_CLASS_ECHO, 0u);
	CHECK(rc == ULMK_OK, "register echo0");

	rc = ulmk_dev_table_lookup(tab, 4, "/dev/echo0", &out);
	CHECK(rc == ULMK_OK, "lookup echo0");
	CHECK(out.ep == (ulmk_ep_t)10u, "ep matches");
	CHECK(out.server == (ulmk_tid_t)20u, "server matches");
	CHECK(out.class == ULMK_DEV_CLASS_ECHO, "class matches");

	rc = ulmk_dev_table_lookup(tab, 4, "/dev/missing", &out);
	CHECK(rc == ULMK_ESRCH, "lookup missing");
}

static void test_dup_and_full(void)
{
	struct ulmk_dev_entry tab[2];
	int rc;

	memset(tab, 0, sizeof(tab));
	CHECK(ulmk_dev_table_register(tab, 2, "/a", (ulmk_ep_t)1u,
				      (ulmk_tid_t)1u, 1u, 0u) == ULMK_OK,
	      "slot0");
	CHECK(ulmk_dev_table_register(tab, 2, "/a", (ulmk_ep_t)2u,
				      (ulmk_tid_t)2u, 1u, 0u) == ULMK_EINVAL,
	      "dup rejected");
	CHECK(ulmk_dev_table_register(tab, 2, "/b", (ulmk_ep_t)2u,
				      (ulmk_tid_t)2u, 1u, 0u) == ULMK_OK,
	      "slot1");
	rc = ulmk_dev_table_register(tab, 2, "/c", (ulmk_ep_t)3u,
				     (ulmk_tid_t)3u, 1u, 0u);
	CHECK(rc == ULMK_ENOSPC, "table full");
}

static void test_unregister(void)
{
	struct ulmk_dev_entry tab[2];
	struct ulmk_dev_entry out;

	memset(tab, 0, sizeof(tab));
	CHECK(ulmk_dev_table_register(tab, 2, "/dev/x", (ulmk_ep_t)5u,
				      (ulmk_tid_t)6u, 1u, 0u) == ULMK_OK,
	      "reg");
	CHECK(ulmk_dev_table_unregister(tab, 2, "/dev/x") == ULMK_OK, "unreg");
	CHECK(ulmk_dev_table_lookup(tab, 2, "/dev/x", &out) == ULMK_ESRCH,
	      "gone after unreg");
	CHECK(ulmk_dev_table_unregister(tab, 2, "/dev/x") == ULMK_ESRCH,
	      "unreg missing");
	CHECK(ulmk_dev_table_register(tab, 2, "/dev/x", (ulmk_ep_t)7u,
				      (ulmk_tid_t)8u, 1u, 0u) == ULMK_OK,
	      "reuse slot");
}

static void test_bad_args(void)
{
	struct ulmk_dev_entry tab[1];
	struct ulmk_dev_entry out;

	memset(tab, 0, sizeof(tab));
	CHECK(ulmk_dev_table_register(NULL, 1, "/a", (ulmk_ep_t)1u,
				      (ulmk_tid_t)1u, 0u, 0u) == ULMK_EINVAL,
	      "null tab");
	CHECK(ulmk_dev_table_register(tab, 1, "/a", ULMK_EP_INVALID,
				      (ulmk_tid_t)1u, 0u, 0u) == ULMK_EINVAL,
	      "bad ep");
	CHECK(ulmk_dev_table_lookup(tab, 1, NULL, &out) == ULMK_EINVAL,
	      "null path lookup");
}

static void test_display_protocol(void)
{
	struct proto_msg msg;
	struct ulmk_disp_rect rects[1];

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_OPEN;
	CHECK(proto_disp_handle(&msg) == ULMK_OK, "disp open");
	CHECK(msg.words[0] == (uint32_t)ULMK_OK, "disp open rc");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_INFO;
	CHECK(proto_disp_handle(&msg) == ULMK_OK, "disp info");
	CHECK(msg.words[1] == 1024u && msg.words[2] == 600u, "disp geom");
	CHECK(msg.words[3] == 16u && msg.words[5] == 2u, "disp bpp/nfb");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DISP_IOCTL_GET_FB;
	msg.words[0] = 0u;
	CHECK(proto_disp_handle(&msg) == ULMK_OK, "disp get_fb0");
	CHECK(msg.words[1] == (uint32_t)(uintptr_t)proto_disp_fb0(),
	      "disp fb0 ptr");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DISP_IOCTL_ON;
	msg.words[0] = 1u;
	CHECK(proto_disp_handle(&msg) == ULMK_OK, "disp on");
	CHECK(proto_disp_on_state() == 1, "disp on state");

	rects[0].x = 0;
	rects[0].y = 0;
	rects[0].w = 10;
	rects[0].h = 10;
	{
		uint8_t payload[ULMK_DEV_INLINE_BYTES];
		struct ulmk_disp_present_hdr *hdr =
			(struct ulmk_disp_present_hdr *)payload;
		size_t need = sizeof(*hdr) + sizeof(rects);

		memset(payload, 0, sizeof(payload));
		hdr->fb_ptr = 0x1000u;
		hdr->n_rects = 1u;
		memcpy(hdr + 1, rects, sizeof(rects));
		memset(&msg, 0, sizeof(msg));
		msg.label = ULMK_DEV_REQ_WRITE;
		msg.words[0] = (uint32_t)need | (ULMK_DEV_F_INLINE << 16);
		memcpy(&msg.words[1], payload, need);
		CHECK(proto_disp_handle(&msg) == ULMK_OK, "disp write present");
		CHECK(proto_disp_last_present() == 0x1000u, "present fb");
		CHECK(proto_disp_last_n_rects() == 1u, "present n_rects");
	}

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_READ;
	CHECK(proto_disp_handle(&msg) == ULMK_ENOTSUP, "disp read enotsup");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_CLOSE;
	CHECK(proto_disp_handle(&msg) == ULMK_OK, "disp close");
}

static void test_input_protocol(void)
{
	struct proto_msg msg;
	struct ulmk_input_event ev;

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_OPEN;
	CHECK(proto_input_handle(&msg) == ULMK_OK, "input open");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_READ;
	CHECK(proto_input_handle(&msg) == ULMK_OK, "input read");
	CHECK((msg.words[0] & 0xFFFFu) == sizeof(ev), "input size");
	CHECK(((msg.words[0] >> 16) & ULMK_DEV_F_INLINE) != 0u,
	      "input inline");
	ev.x = (int16_t)(msg.words[1] & 0xFFFFu);
	ev.y = (int16_t)((msg.words[1] >> 16) & 0xFFFFu);
	ev.state = (uint8_t)msg.words[2];
	CHECK(ev.x == 100 && ev.y == 200, "input xy");
	CHECK(ev.state == ULMK_INPUT_STATE_PRESSED, "input pressed");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_WRITE;
	CHECK(proto_input_handle(&msg) == ULMK_ENOTSUP, "input write enotsup");

	memset(&msg, 0, sizeof(msg));
	msg.label = ULMK_DEV_REQ_CLOSE;
	CHECK(proto_input_handle(&msg) == ULMK_OK, "input close");
}

int main(void)
{
	printf("--- device_manager unit tests ---\n");
	test_path_ok();
	test_register_lookup();
	test_dup_and_full();
	test_unregister();
	test_bad_args();
	test_display_protocol();
	test_input_protocol();
	printf("ran=%d failed=%d\n", tests_run, tests_failed);
	if (tests_failed)
		return 1;
	printf("DEVICE_MANAGER UNIT TEST: PASS\n");
	return 0;
}
