/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Board console service — boards/qemu_tc3xx/board_console.c
 *
 * IPC server owns the VIRT MMIO mapping.  Clients use putc/puts/printf.
 * WRITE(ptr,len) prints a whole buffer under one ep_call (SMP-atomic).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <ulmk/microkernel.h>
#include "board_console.h"

#define VIRT_BASE        0xBF000000UL
#define VIRT_REGION_SIZE 0x40U
#define VIRT_PUTCHAR_OFF 0x20U

#define CONSOLE_MSG_PUTC	1u
#define CONSOLE_MSG_WRITE	2u
#define CONSOLE_WRITE_MAX	256u
/* Bytes travel inline: the server never dereferences a client pointer. */
#define CONSOLE_CHUNK_MAX	((ULMK_MSG_WORDS - 1u) * 4u)
#define CONSOLE_FMT_BUF		160u

static ulmk_ep_t g_ep __attribute__((section(".user_bss")));
static volatile uint32_t *g_putchar __attribute__((section(".user_bss")));

static void console_putc_hw(char c)
{
	if (g_putchar)
		*g_putchar = (uint32_t)(uint8_t)c;
}

static void console_server(void *arg)
{
	ulmk_msg_t msg;
	ulmk_tid_t sender;
	const char *buf;
	uint32_t len;
	uint32_t i;

	(void)arg;

	g_putchar = (volatile uint32_t *)((uintptr_t)ulmk_mem_map(
		(void *)VIRT_BASE,
		VIRT_REGION_SIZE,
		ULMK_PERM_READ | ULMK_PERM_WRITE,
		ULMK_MMAP_PERIPH) + VIRT_PUTCHAR_OFF);

	for (;;) {
		ulmk_ep_recv(g_ep, &msg, &sender);
		if (msg.label == CONSOLE_MSG_PUTC) {
			console_putc_hw((char)(uint8_t)msg.words[0]);
		} else if (msg.label == CONSOLE_MSG_WRITE) {
			buf = (const char *)&msg.words[1];
			len = msg.words[0];
			if (len > CONSOLE_CHUNK_MAX)
				len = CONSOLE_CHUNK_MAX;
			if (buf && len > 0u) {
				if (len > CONSOLE_WRITE_MAX)
					len = CONSOLE_WRITE_MAX;
				for (i = 0u; i < len; i++)
					console_putc_hw(buf[i]);
			}
		}
		ulmk_ep_reply(sender, &(ulmk_msg_t){0});
	}
}

static void console_write(const char *buf, uint32_t len)
{
	ulmk_msg_t msg;
	uint8_t *dst = (uint8_t *)&msg.words[1];
	uint32_t n;
	uint32_t i;

	if (!buf || len == 0u || g_ep == ULMK_EP_INVALID)
		return;
	if (len > CONSOLE_WRITE_MAX)
		len = CONSOLE_WRITE_MAX;
	while (len > 0u) {
		n = len < CONSOLE_CHUNK_MAX ? len : CONSOLE_CHUNK_MAX;
		for (i = 0u; i < n; i++)
			dst[i] = (uint8_t)buf[i];
		msg.label    = CONSOLE_MSG_WRITE;
		msg.words[0] = n;
		(void)ulmk_ep_call(g_ep, &msg);
		buf += n;
		len -= n;
	}
}

void board_console_putc(char c)
{
	ulmk_msg_t msg;

	if (g_ep == ULMK_EP_INVALID)
		return;
	msg.label    = CONSOLE_MSG_PUTC;
	msg.words[0] = (uint32_t)(uint8_t)c;
	(void)ulmk_ep_call(g_ep, &msg);
}

void board_console_puts(const char *s)
{
	uint32_t len;

	if (!s)
		return;
	len = 0u;
	while (s[len] != '\0' && len < CONSOLE_WRITE_MAX)
		len++;
	console_write(s, len);
}

static void fmt_u32(char *out, uint32_t *pos, uint32_t cap, uint32_t v)
{
	char tmp[10];
	uint32_t n = 0u;

	if (v == 0u) {
		if (*pos < cap)
			out[(*pos)++] = '0';
		return;
	}
	while (v > 0u && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (n > 0u) {
		if (*pos >= cap)
			return;
		out[(*pos)++] = tmp[--n];
	}
}

static void fmt_i32(char *out, uint32_t *pos, uint32_t cap, int32_t v)
{
	if (v < 0) {
		if (*pos < cap)
			out[(*pos)++] = '-';
		fmt_u32(out, pos, cap, (uint32_t)(-(v + 1)) + 1u);
	} else {
		fmt_u32(out, pos, cap, (uint32_t)v);
	}
}

static void fmt_ulong(char *out, uint32_t *pos, uint32_t cap, unsigned long v)
{
	char tmp[20];
	uint32_t n = 0u;

	if (v == 0ul) {
		if (*pos < cap)
			out[(*pos)++] = '0';
		return;
	}
	while (v > 0ul && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (int)(v % 10ul));
		v /= 10ul;
	}
	while (n > 0u) {
		if (*pos >= cap)
			return;
		out[(*pos)++] = tmp[--n];
	}
}

static void fmt_hex(char *out, uint32_t *pos, uint32_t cap,
		    unsigned long v, int upper)
{
	static const char lo[] = "0123456789abcdef";
	static const char hi[] = "0123456789ABCDEF";
	const char *digits = upper ? hi : lo;
	char tmp[16];
	uint32_t n = 0u;

	if (v == 0ul) {
		if (*pos < cap)
			out[(*pos)++] = '0';
		return;
	}
	while (v > 0ul && n < sizeof(tmp)) {
		tmp[n++] = digits[v & 0xful];
		v >>= 4;
	}
	while (n > 0u) {
		if (*pos >= cap)
			return;
		out[(*pos)++] = tmp[--n];
	}
}

static void fmt_str(char *out, uint32_t *pos, uint32_t cap, const char *s)
{
	if (!s)
		s = "(null)";
	while (*s) {
		if (*pos >= cap)
			return;
		out[(*pos)++] = *s++;
	}
}

void board_console_printf(const char *fmt, ...)
{
	char buf[CONSOLE_FMT_BUF];
	uint32_t pos = 0u;
	va_list ap;
	int is_long;

	if (!fmt)
		return;

	va_start(ap, fmt);
	while (*fmt && pos < CONSOLE_FMT_BUF) {
		if (*fmt != '%') {
			buf[pos++] = *fmt++;
			continue;
		}
		fmt++;
		is_long = 0;
		while (*fmt == '0')
			fmt++;
		while (*fmt >= '1' && *fmt <= '9')
			fmt++;
		if (*fmt == 'l') {
			is_long = 1;
			fmt++;
		} else if (*fmt == 'z') {
			is_long = (sizeof(size_t) > sizeof(uint32_t)) ? 1 : 0;
			fmt++;
		}

		switch (*fmt++) {
		case 'c':
			if (pos < CONSOLE_FMT_BUF)
				buf[pos++] = (char)va_arg(ap, int);
			break;
		case 's':
			fmt_str(buf, &pos, CONSOLE_FMT_BUF,
				va_arg(ap, const char *));
			break;
		case 'd':
		case 'i':
			if (is_long)
				fmt_i32(buf, &pos, CONSOLE_FMT_BUF,
					(int32_t)va_arg(ap, long));
			else
				fmt_i32(buf, &pos, CONSOLE_FMT_BUF,
					va_arg(ap, int32_t));
			break;
		case 'u':
			if (is_long)
				fmt_ulong(buf, &pos, CONSOLE_FMT_BUF,
					  va_arg(ap, unsigned long));
			else
				fmt_u32(buf, &pos, CONSOLE_FMT_BUF,
					va_arg(ap, uint32_t));
			break;
		case 'x':
			if (is_long)
				fmt_hex(buf, &pos, CONSOLE_FMT_BUF,
					va_arg(ap, unsigned long), 0);
			else
				fmt_hex(buf, &pos, CONSOLE_FMT_BUF,
					(unsigned long)va_arg(ap, uint32_t), 0);
			break;
		case 'X':
			if (is_long)
				fmt_hex(buf, &pos, CONSOLE_FMT_BUF,
					va_arg(ap, unsigned long), 1);
			else
				fmt_hex(buf, &pos, CONSOLE_FMT_BUF,
					(unsigned long)va_arg(ap, uint32_t), 1);
			break;
		case 'p':
			if (pos < CONSOLE_FMT_BUF)
				buf[pos++] = '0';
			if (pos < CONSOLE_FMT_BUF)
				buf[pos++] = 'x';
			fmt_hex(buf, &pos, CONSOLE_FMT_BUF,
				(unsigned long)(uintptr_t)va_arg(ap, void *),
				0);
			break;
		case '%':
			if (pos < CONSOLE_FMT_BUF)
				buf[pos++] = '%';
			break;
		default:
			if (pos < CONSOLE_FMT_BUF)
				buf[pos++] = '?';
			break;
		}
	}
	va_end(ap);
	console_write(buf, pos);
}

ulmk_tid_t board_console_start(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	ulmk_tid_t         tid;

	(void)info;

	g_ep = ulmk_ep_create();
	if (g_ep == ULMK_EP_INVALID)
		return ULMK_TID_INVALID;

	attr.name       = "bcon";
	attr.entry      = console_server;
	attr.arg        = NULL;
	attr.priority   = 1u;
	attr.stack_size = 1024u;
	attr.privilege  = ULMK_PRIV_DRIVER;

	tid = ulmk_thread_create(&attr);
	if (tid == ULMK_TID_INVALID)
		return ULMK_TID_INVALID;
	ulmk_cap_grant(tid, ULMK_CAP_MAP_PERIPH);
	return tid;
}
