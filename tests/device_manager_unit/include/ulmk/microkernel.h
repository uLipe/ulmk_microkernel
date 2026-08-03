/* SPDX-License-Identifier: MIT */
#ifndef ULMK_MICROKERNEL_H
#define ULMK_MICROKERNEL_H

#include <stdint.h>
#include <stddef.h>

#define ULMK_OK		  0
#define ULMK_EINVAL	 -1
#define ULMK_ENOMEM	 -2
#define ULMK_EPERM	 -3
#define ULMK_ENOSPC	 -4
#define ULMK_EDEADLK	 -5
#define ULMK_ESRCH	 -6
#define ULMK_ETIMEOUT	 -7
#define ULMK_ECANCELED	 -8
#define ULMK_ENOTSUP	 -9

typedef uint32_t ulmk_ep_t;
typedef uint32_t ulmk_tid_t;

#define ULMK_EP_INVALID		((ulmk_ep_t)0u)
#define ULMK_TID_INVALID	((ulmk_tid_t)0u)

#define ULMK_MSG_WORDS	6

typedef struct {
	uint32_t label;
	uint32_t words[ULMK_MSG_WORDS];
} ulmk_msg_t;

#endif /* ULMK_MICROKERNEL_H */
