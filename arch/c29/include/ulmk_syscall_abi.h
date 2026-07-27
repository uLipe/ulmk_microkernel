/* SPDX-License-Identifier: MIT */
/*
 * Syscall ABI — TI C29x
 *
 * Userspace forces Supervisor software INT (INT_SW2) after loading:
 *   D4 = syscall number
 *   D0..D3 = args
 * Return value is written back into the saved D0 slot by the INT epilogue.
 */

#ifndef ULMK_SYSCALL_ABI_C29_H
#define ULMK_SYSCALL_ABI_C29_H

#include <stdint.h>

/* PIPE INT_CTL_H(254) = 0x30020000 + 0x2000 + 254*4 = 0x300223F8? 
 * Wait: 0x2000 + 254*4 = 0x2000 + 0x3F8 = 0x23F8 → 0x300223F8
 * Earlier macro said 0x30022278 — that would be channel 158. Fix to 254.
 */
#define ULMK_C29_PIPE_SWINT_CTL_ADDR	0x300223F8u

#define ULMK_C29_FORCE_SYSCALL_INT() \
	__asm__ volatile( \
		"MV	D7, #1\n\t" \
		"ST.W0	@%0, D7\n\t" \
		"NOP	#8\n\t" \
		"NOP	#5\n\t" \
		: \
		: "i"(ULMK_C29_PIPE_SWINT_CTL_ADDR) \
		: "d7", "memory")

#define ULMK_SYSCALL_0(nr, ret) \
	do { \
		register uint32_t _d0 __asm__("d0"); \
		register uint32_t _d4 __asm__("d4") = (uint32_t)(nr); \
		__asm__ volatile("" :: "r"(_d4) : "memory"); \
		ULMK_C29_FORCE_SYSCALL_INT(); \
		__asm__ volatile("" : "=r"(_d0) : : "memory"); \
		(ret) = _d0; \
	} while (0)

#define ULMK_SYSCALL_1(nr, a0v, ret) \
	do { \
		register uint32_t _d0 __asm__("d0") = (uint32_t)(uintptr_t)(a0v); \
		register uint32_t _d4 __asm__("d4") = (uint32_t)(nr); \
		__asm__ volatile("" : "+r"(_d0) : "r"(_d4) : "memory"); \
		ULMK_C29_FORCE_SYSCALL_INT(); \
		__asm__ volatile("" : "+r"(_d0) : : "memory"); \
		(ret) = _d0; \
	} while (0)

#define ULMK_SYSCALL_2(nr, a0v, a1v, ret) \
	do { \
		register uint32_t _d0 __asm__("d0") = (uint32_t)(uintptr_t)(a0v); \
		register uint32_t _d1 __asm__("d1") = (uint32_t)(uintptr_t)(a1v); \
		register uint32_t _d4 __asm__("d4") = (uint32_t)(nr); \
		__asm__ volatile("" : "+r"(_d0) : "r"(_d1), "r"(_d4) : "memory"); \
		ULMK_C29_FORCE_SYSCALL_INT(); \
		__asm__ volatile("" : "+r"(_d0) : : "memory"); \
		(ret) = _d0; \
	} while (0)

#define ULMK_SYSCALL_3(nr, a0v, a1v, a2v, ret) \
	do { \
		register uint32_t _d0 __asm__("d0") = (uint32_t)(uintptr_t)(a0v); \
		register uint32_t _d1 __asm__("d1") = (uint32_t)(uintptr_t)(a1v); \
		register uint32_t _d2 __asm__("d2") = (uint32_t)(uintptr_t)(a2v); \
		register uint32_t _d4 __asm__("d4") = (uint32_t)(nr); \
		__asm__ volatile("" : "+r"(_d0) \
			: "r"(_d1), "r"(_d2), "r"(_d4) : "memory"); \
		ULMK_C29_FORCE_SYSCALL_INT(); \
		__asm__ volatile("" : "+r"(_d0) : : "memory"); \
		(ret) = _d0; \
	} while (0)

#define ULMK_SYSCALL_4(nr, a0v, a1v, a2v, a3v, ret) \
	do { \
		register uint32_t _d0 __asm__("d0") = (uint32_t)(uintptr_t)(a0v); \
		register uint32_t _d1 __asm__("d1") = (uint32_t)(uintptr_t)(a1v); \
		register uint32_t _d2 __asm__("d2") = (uint32_t)(uintptr_t)(a2v); \
		register uint32_t _d3 __asm__("d3") = (uint32_t)(uintptr_t)(a3v); \
		register uint32_t _d4 __asm__("d4") = (uint32_t)(nr); \
		__asm__ volatile("" : "+r"(_d0) \
			: "r"(_d1), "r"(_d2), "r"(_d3), "r"(_d4) : "memory"); \
		ULMK_C29_FORCE_SYSCALL_INT(); \
		__asm__ volatile("" : "+r"(_d0) : : "memory"); \
		(ret) = _d0; \
	} while (0)

typedef struct {
	ulmk_msg_t msg;
	ulmk_tid_t sender;
	uint32_t notif_bits;
	int      is_notif;
} ulmk_recv_or_notif_result_t;

typedef struct {
	const ulmk_msg_t *reply;
	ulmk_msg_t       *next;
	ulmk_tid_t       *next_sender;
} ulmk_reply_recv_args_t;

#endif /* ULMK_SYSCALL_ABI_C29_H */
