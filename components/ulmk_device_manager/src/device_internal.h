/* SPDX-License-Identifier: MIT */
#ifndef ULMK_DEVICE_INTERNAL_H
#define ULMK_DEVICE_INTERNAL_H

#include "ulmk_device.h"

enum {
	ULMK_DEV_MGR_REGISTER	= 1u,	/* ep/server/class/instance; path from PATH */
	ULMK_DEV_MGR_UNREGISTER	= 2u,	/* path inline in words[1..] */
	ULMK_DEV_MGR_LOOKUP	= 3u,	/* path inline in words[1..] */
	ULMK_DEV_MGR_PATH	= 4u,	/* set pending path for this sender */
};

/*
 * Pathnames cross domain boundaries in the message — never as a raw
 * pointer (MPU would fault the manager on the caller's .rodata).
 */
void ulmk_dev_msg_pack_path(ulmk_msg_t *msg, const char *path);
void ulmk_dev_msg_unpack_path(const ulmk_msg_t *msg, char *dst);

struct ulmk_dev_entry {
	char       path[ULMK_DEV_PATH_MAX];
	ulmk_ep_t  ep;
	ulmk_tid_t server;
	uint16_t   class;
	uint8_t    instance;
	uint8_t    used;
};

int ulmk_dev_path_ok(const char *path);
int ulmk_dev_path_eq(const char *a, const char *b);
void ulmk_dev_path_copy(char *dst, const char *src);

/* Table helpers — unit-tested with a caller-owned array. */
int ulmk_dev_table_register(struct ulmk_dev_entry *tab, uint32_t max,
			    const char *path, ulmk_ep_t ep, ulmk_tid_t server,
			    uint16_t class, uint8_t instance);
int ulmk_dev_table_unregister(struct ulmk_dev_entry *tab, uint32_t max,
			      const char *path);
int ulmk_dev_table_lookup(const struct ulmk_dev_entry *tab, uint32_t max,
			  const char *path, struct ulmk_dev_entry *out);

extern ulmk_ep_t g_ulmk_dev_mgr_ep;

#endif /* ULMK_DEVICE_INTERNAL_H */
