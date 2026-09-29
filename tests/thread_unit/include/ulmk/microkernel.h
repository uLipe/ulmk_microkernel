/* Stub for unit tests */
#ifndef ULMK_MICROKERNEL_H
#define ULMK_MICROKERNEL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
	ULMK_PRIV_USER   = 0,
	ULMK_PRIV_DRIVER = 1,
	ULMK_PRIV_KERNEL = 2,
} ulmk_privilege_t;

typedef uintptr_t ulmk_tid_t;
typedef int32_t ulmk_ep_t;
typedef int32_t ulmk_notif_t;

#define ULMK_TID_INVALID   ((ulmk_tid_t)0)
#define ULMK_EP_INVALID    ((ulmk_ep_t)-1)
#define ULMK_NOTIF_INVALID ((ulmk_notif_t)-1)

#define ULMK_OK		 0
#define ULMK_EINVAL	-1
#define ULMK_ENOMEM	-2
#define ULMK_EPERM	-3
#define ULMK_ENOSPC	-4
#define ULMK_EDEADLK	-5
#define ULMK_ESRCH	-6
#define ULMK_ETIMEOUT	-7

/* Capabilities */
#define ULMK_CAP_SPAWN		(1u << 0)
#define ULMK_CAP_IRQ		(1u << 2)
#define ULMK_CAP_MAP_PERIPH	(1u << 3)
#define ULMK_CAP_ALL		0xFFu
#define ULMK_CAP_INHERIT	0u
#define ULMK_CAP_NONE		(1u << 31)

/* Memory permissions */
#define ULMK_PERM_READ	(1u << 0)
#define ULMK_PERM_WRITE	(1u << 1)
#define ULMK_PERM_EXEC	(1u << 2)
#define ULMK_PERM_USER	(1u << 3)

/* IPC message */
#define ULMK_MSG_WORDS	6
typedef struct {
	uint32_t label;
	uint32_t words[ULMK_MSG_WORDS];
} ulmk_msg_t;

typedef struct {
	ulmk_msg_t msg;
	ulmk_tid_t sender;
	uint32_t notif_bits;
	int      is_notif;
} ulmk_recv_or_notif_result_t;

typedef struct {
	const char	*name;
	void		(*entry)(void *arg);
	void		*arg;
	uint8_t		 priority;
	size_t		 stack_size;
	ulmk_privilege_t	 privilege;
	uint32_t	 caps;
	uint8_t		 cpu;
} ulmk_thread_attr_t;

#endif /* ULMK_MICROKERNEL_H */
