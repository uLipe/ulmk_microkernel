/* host stub — only the constants kernel/mem/area.c needs */
#ifndef ULMK_MICROKERNEL_H
#define ULMK_MICROKERNEL_H
#define ULMK_OK		  0
#define ULMK_EINVAL	 -1
#define ULMK_ENOMEM	 -2
#define ULMK_PERM_READ	(1u << 0)
#define ULMK_PERM_WRITE	(1u << 1)
#endif
