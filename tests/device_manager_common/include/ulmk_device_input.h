/* SPDX-License-Identifier: MIT */
#ifndef ULMK_DEVICE_INPUT_H
#define ULMK_DEVICE_INPUT_H

#include <stdint.h>
#include <ulmk_device.h>

/*
 * Pointer/input-class contract (policy — ulmk_device_classes in ulmk_apps).
 *
 * Pathname: /dev/inputN
 * Class:    ULMK_DEV_CLASS_INPUT
 *
 * READ (non-blocking): returns sizeof(ulmk_input_event) or 0 if idle,
 *   or a negative errno.  Event is inline for the 8-byte struct.
 * WAIT / SUBMIT: optional; boards without an IRQ may return ENOTSUP.
 */
#define ULMK_DEV_CLASS_INPUT		2u

#define ULMK_INPUT_STATE_RELEASED	0u
#define ULMK_INPUT_STATE_PRESSED	1u

struct ulmk_input_event {
	int16_t x;
	int16_t y;
	uint8_t state;
	uint8_t _pad[3];
};

static inline int ulmk_input_read(ulmk_dev_t *dev,
				  struct ulmk_input_event *ev)
{
	int n;

	if (!ev)
		return ULMK_EINVAL;
	n = ulmk_read(dev, ev, sizeof(*ev));
	return n;
}

#endif /* ULMK_DEVICE_INPUT_H */
