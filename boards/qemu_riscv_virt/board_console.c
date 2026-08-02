/* SPDX-License-Identifier: MIT */
/* boards/qemu_riscv_virt/board_console.c — client over board_service_ep */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <ulmk/microkernel.h>
#include "board_config.h"
#include "board_console.h"
#include "board_internal.h"

#define CONSOLE_MSG_PUTC	1u
#define CONSOLE_MSG_WRITE	2u
#define CONSOLE_WRITE_MAX	256u
#define CONSOLE_FMT_BUF		160u

static char g_console_bounce[ULMK_ARCH_NUM_CPU][CONSOLE_WRITE_MAX]
	__attribute__((section(".user_bss")));

static void console_write(const char *buf, uint32_t len)
{
	ulmk_msg_t msg;
	ulmk_ep_t ep = board_service_ep();
	uint32_t cpu;
	uint32_t i;
	char *dst;

	if (!buf || len == 0u || ep == ULMK_EP_INVALID)
		return;
	if (len > CONSOLE_WRITE_MAX)
		len = CONSOLE_WRITE_MAX;
	cpu = ulmk_cpu_id();
	if (cpu >= (uint32_t)ULMK_ARCH_NUM_CPU)
		cpu = 0u;
	dst = g_console_bounce[cpu];
	for (i = 0u; i < len; i++)
		dst[i] = buf[i];
	msg.label    = CONSOLE_MSG_WRITE;
	msg.words[0] = (uint32_t)(uintptr_t)dst;
	msg.words[1] = len;
	(void)ulmk_ep_call(ep, &msg);
}

void board_console_putc(char c)
{
	ulmk_msg_t msg;

	msg.label    = CONSOLE_MSG_PUTC;
	msg.words[0] = (uint32_t)(uint8_t)c;
	(void)ulmk_ep_call(board_service_ep(), &msg);
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
	(void)info;
	return ULMK_TID_INVALID;
}
