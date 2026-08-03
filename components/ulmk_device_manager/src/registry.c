/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "device_internal.h"

/*
 * On TriCore silicon, component .bss without a domain section lands in
 * kernel SRAM and traps under PRS1.  When built as ulmk_comp_* the
 * build defines ULMK_MODULE_NAME and ULMK_PRIVATE places state in the
 * component domain (user RAM).  sdk_suite / unit builds have no domain
 * fragment — plain BSS is fine there.
 */
#if defined(ULMK_MODULE_NAME) && !defined(UL_UNIT_TEST)
#include <ulmk/linker.h>
#define DEV_BSS		ULMK_PRIVATE
#else
#define DEV_BSS
#endif

DEV_BSS ulmk_ep_t g_ulmk_dev_mgr_ep;

#ifndef UL_UNIT_TEST
static DEV_BSS struct ulmk_dev_entry g_tab[ULMK_DEV_MAX];
static DEV_BSS uint8_t g_mgr_ready;
static DEV_BSS char g_pending_path[ULMK_DEV_PATH_MAX];
static DEV_BSS ulmk_tid_t g_pending_tid;
#endif

int ulmk_dev_path_ok(const char *path)
{
	uint32_t i;

	if (!path || path[0] != '/')
		return 0;
	/* Leave room for the trailing NUL inside the inline window. */
	for (i = 0u; i < ULMK_DEV_INLINE_BYTES; i++) {
		if (path[i] == '\0')
			return (i > 1u) ? 1 : 0;
	}
	return 0;
}

void ulmk_dev_msg_pack_path(ulmk_msg_t *msg, const char *path)
{
	uint32_t i;
	uint32_t w;

	msg->words[0] = 0u;
	msg->words[1] = 0u;
	msg->words[2] = 0u;
	msg->words[3] = 0u;
	msg->words[4] = 0u;
	msg->words[5] = 0u;
	for (i = 0u; i < ULMK_DEV_INLINE_BYTES; i++) {
		if (path[i] == '\0')
			break;
		w = i / 4u;
		msg->words[1u + w] |= ((uint32_t)(uint8_t)path[i]) <<
				     ((i % 4u) * 8u);
	}
}

void ulmk_dev_msg_unpack_path(const ulmk_msg_t *msg, char *dst)
{
	uint32_t i;
	uint32_t w;

	for (i = 0u; i < ULMK_DEV_INLINE_BYTES; i++) {
		w = i / 4u;
		dst[i] = (char)((msg->words[1u + w] >> ((i % 4u) * 8u)) &
				0xFFu);
		if (dst[i] == '\0')
			return;
	}
	dst[ULMK_DEV_INLINE_BYTES] = '\0';
}

int ulmk_dev_path_eq(const char *a, const char *b)
{
	uint32_t i;

	for (i = 0u; i < ULMK_DEV_PATH_MAX; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
	return 1;
}

void ulmk_dev_path_copy(char *dst, const char *src)
{
	uint32_t i;

	for (i = 0u; i < ULMK_DEV_PATH_MAX - 1u; i++) {
		dst[i] = src[i];
		if (src[i] == '\0')
			return;
	}
	dst[ULMK_DEV_PATH_MAX - 1u] = '\0';
}

int ulmk_dev_table_register(struct ulmk_dev_entry *tab, uint32_t max,
			    const char *path, ulmk_ep_t ep, ulmk_tid_t server,
			    uint16_t class, uint8_t instance)
{
	uint32_t i;
	uint32_t free_slot = max;

	if (!tab || !ulmk_dev_path_ok(path) || ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;

	for (i = 0u; i < max; i++) {
		if (tab[i].used) {
			if (ulmk_dev_path_eq(tab[i].path, path))
				return ULMK_EINVAL;
		} else if (free_slot == max) {
			free_slot = i;
		}
	}
	if (free_slot >= max)
		return ULMK_ENOSPC;

	ulmk_dev_path_copy(tab[free_slot].path, path);
	tab[free_slot].ep = ep;
	tab[free_slot].server = server;
	tab[free_slot].class = class;
	tab[free_slot].instance = instance;
	tab[free_slot].used = 1u;
	return ULMK_OK;
}

int ulmk_dev_table_unregister(struct ulmk_dev_entry *tab, uint32_t max,
			      const char *path)
{
	uint32_t i;

	if (!tab || !ulmk_dev_path_ok(path))
		return ULMK_EINVAL;

	for (i = 0u; i < max; i++) {
		if (!tab[i].used)
			continue;
		if (!ulmk_dev_path_eq(tab[i].path, path))
			continue;
		tab[i].used = 0u;
		tab[i].ep = ULMK_EP_INVALID;
		tab[i].server = ULMK_TID_INVALID;
		tab[i].path[0] = '\0';
		return ULMK_OK;
	}
	return ULMK_ESRCH;
}

int ulmk_dev_table_lookup(const struct ulmk_dev_entry *tab, uint32_t max,
			  const char *path, struct ulmk_dev_entry *out)
{
	uint32_t i;

	if (!tab || !out || !ulmk_dev_path_ok(path))
		return ULMK_EINVAL;

	for (i = 0u; i < max; i++) {
		if (!tab[i].used)
			continue;
		if (!ulmk_dev_path_eq(tab[i].path, path))
			continue;
		*out = tab[i];
		return ULMK_OK;
	}
	return ULMK_ESRCH;
}

#ifndef UL_UNIT_TEST

static void mgr_server(void *arg)
{
	ulmk_msg_t msg;
	ulmk_msg_t reply;
	ulmk_tid_t sender;
	struct ulmk_dev_entry ent;
	char path[ULMK_DEV_PATH_MAX];
	int rc;

	(void)arg;
	for (;;) {
		if (ulmk_ep_recv(g_ulmk_dev_mgr_ep, &msg, &sender) != ULMK_OK)
			continue;

		reply.label = 0u;
		reply.words[0] = (uint32_t)(int32_t)ULMK_EINVAL;
		reply.words[1] = 0u;
		reply.words[2] = 0u;
		reply.words[3] = 0u;
		reply.words[4] = 0u;
		reply.words[5] = 0u;

		switch (msg.label) {
		case ULMK_DEV_MGR_PATH:
			ulmk_dev_msg_unpack_path(&msg, path);
			if (!ulmk_dev_path_ok(path)) {
				rc = ULMK_EINVAL;
			} else {
				ulmk_dev_path_copy(g_pending_path, path);
				g_pending_tid = sender;
				rc = ULMK_OK;
			}
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_MGR_REGISTER:
			if (g_pending_tid != sender ||
			    !ulmk_dev_path_ok(g_pending_path)) {
				rc = ULMK_EINVAL;
			} else {
				rc = ulmk_dev_table_register(
					g_tab, ULMK_DEV_MAX, g_pending_path,
					(ulmk_ep_t)msg.words[0],
					(ulmk_tid_t)msg.words[1],
					(uint16_t)msg.words[2],
					(uint8_t)msg.words[3]);
				g_pending_tid = ULMK_TID_INVALID;
				g_pending_path[0] = '\0';
			}
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_MGR_UNREGISTER:
			ulmk_dev_msg_unpack_path(&msg, path);
			rc = ulmk_dev_table_unregister(g_tab, ULMK_DEV_MAX,
						       path);
			reply.words[0] = (uint32_t)(int32_t)rc;
			break;

		case ULMK_DEV_MGR_LOOKUP:
			ulmk_dev_msg_unpack_path(&msg, path);
			rc = ulmk_dev_table_lookup(g_tab, ULMK_DEV_MAX, path,
						   &ent);
			reply.words[0] = (uint32_t)(int32_t)rc;
			if (rc == ULMK_OK) {
				reply.words[1] = (uint32_t)ent.ep;
				reply.words[2] = (uint32_t)ent.server;
				reply.words[3] = ent.class;
				reply.words[4] = ent.instance;
			}
			break;

		default:
			break;
		}
		ulmk_ep_reply(sender, &reply);
	}
}

int ulmk_dev_manager_init(void)
{
	ulmk_thread_attr_t attr = { 0 };
	ulmk_tid_t tid;

	if (g_mgr_ready)
		return ULMK_OK;

	g_ulmk_dev_mgr_ep = ulmk_ep_create();
	if (g_ulmk_dev_mgr_ep == ULMK_EP_INVALID)
		return ULMK_ENOMEM;

	attr.name = "dev_mgr";
	attr.entry = mgr_server;
	attr.arg = NULL;
	attr.priority = 1u;
	attr.stack_size = 2048u;
	attr.privilege = ULMK_PRIV_DRIVER;
	tid = ulmk_thread_create(&attr);
	if (tid == ULMK_TID_INVALID)
		return ULMK_ENOMEM;

	g_mgr_ready = 1u;
	return ULMK_OK;
}

int ulmk_dev_register(const char *path, ulmk_ep_t ep, ulmk_tid_t server,
		      uint16_t class, uint8_t instance)
{
	ulmk_msg_t msg;
	int rc;

	if (g_ulmk_dev_mgr_ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;
	if (!ulmk_dev_path_ok(path) || ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;

	msg.label = ULMK_DEV_MGR_PATH;
	ulmk_dev_msg_pack_path(&msg, path);
	rc = ulmk_ep_call(g_ulmk_dev_mgr_ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	if ((int)(int32_t)msg.words[0] != ULMK_OK)
		return (int)(int32_t)msg.words[0];

	msg.label = ULMK_DEV_MGR_REGISTER;
	msg.words[0] = (uint32_t)ep;
	msg.words[1] = (uint32_t)server;
	msg.words[2] = class;
	msg.words[3] = instance;
	msg.words[4] = 0u;
	msg.words[5] = 0u;
	rc = ulmk_ep_call(g_ulmk_dev_mgr_ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	return (int)(int32_t)msg.words[0];
}

int ulmk_dev_unregister(const char *path)
{
	ulmk_msg_t msg;
	int rc;

	if (g_ulmk_dev_mgr_ep == ULMK_EP_INVALID)
		return ULMK_EINVAL;
	if (!ulmk_dev_path_ok(path))
		return ULMK_EINVAL;

	msg.label = ULMK_DEV_MGR_UNREGISTER;
	ulmk_dev_msg_pack_path(&msg, path);
	rc = ulmk_ep_call(g_ulmk_dev_mgr_ep, &msg);
	if (rc != ULMK_OK)
		return rc;
	return (int)(int32_t)msg.words[0];
}

#endif /* !UL_UNIT_TEST */
