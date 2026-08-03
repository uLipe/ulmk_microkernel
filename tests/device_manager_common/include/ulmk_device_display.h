/* SPDX-License-Identifier: MIT */
#ifndef ULMK_DEVICE_DISPLAY_H
#define ULMK_DEVICE_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <ulmk_device.h>

/*
 * Display-class contract (policy — ulmk_device_classes in ulmk_apps).
 * Mechanism: ulmk_device.h / ulmk_device_manager in the kernel tree.
 *
 * Path: /dev/dispN   Class: ULMK_DEV_CLASS_DISPLAY
 *
 * INFO out[0..4]: w, h, bpp, stride_bytes, n_fb
 * ioctl GET_FB: in[0]=idx → out[0]=rc, out[1]=fb_ptr
 * ioctl ON:     in[0]=0|1 → out[0]=rc
 * write: present payload (see ulmk_disp_present_hdr) — board cleans cache
 */
#define ULMK_DEV_CLASS_DISPLAY		1u

#define ULMK_DISP_IOCTL_GET_FB		(ULMK_DEV_REQ_IOCTL + 1u) /* 0x41 */
#define ULMK_DISP_IOCTL_ON		(ULMK_DEV_REQ_IOCTL + 2u) /* 0x42 */

struct ulmk_disp_rect {
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
};

struct ulmk_disp_present_hdr {
	uint32_t fb_ptr;
	uint32_t n_rects;
	/* followed by n_rects × struct ulmk_disp_rect when n_rects > 0 */
};

static inline int ulmk_disp_info(ulmk_dev_t *dev, uint32_t *w, uint32_t *h,
				 uint32_t *bpp, uint32_t *stride,
				 uint32_t *n_fb)
{
	uint32_t out[5];
	int rc;

	rc = ulmk_dev_info(dev, out, 5u);
	if (rc != ULMK_OK)
		return rc;
	if (w)
		*w = out[0];
	if (h)
		*h = out[1];
	if (bpp)
		*bpp = out[2];
	if (stride)
		*stride = out[3];
	if (n_fb)
		*n_fb = out[4];
	return ULMK_OK;
}

static inline int ulmk_disp_get_fb(ulmk_dev_t *dev, uint32_t idx, void **fb)
{
	uint32_t args[2];
	int rc;

	args[0] = idx;
	args[1] = 0u;
	rc = ulmk_ioctl(dev, ULMK_DISP_IOCTL_GET_FB, args, 2u);
	if (rc != ULMK_OK)
		return rc;
	if (fb)
		*fb = (void *)(uintptr_t)args[1];
	return ULMK_OK;
}

static inline int ulmk_disp_on(ulmk_dev_t *dev, int on)
{
	uint32_t args[1];

	args[0] = on ? 1u : 0u;
	return ulmk_ioctl(dev, ULMK_DISP_IOCTL_ON, args, 1u);
}

/*
 * Present @fb with optional dirty rects via ulmk_write.
 * Small n_rects fit inline; larger payloads use the grant path.
 */
static inline int ulmk_disp_write_present(ulmk_dev_t *dev, const void *fb,
					  const struct ulmk_disp_rect *rects,
					  uint32_t n_rects)
{
	uint8_t stack[ULMK_DEV_INLINE_BYTES];
	struct ulmk_disp_present_hdr *hdr;
	size_t need;
	size_t rect_bytes;

	if (!fb)
		return ULMK_EINVAL;
	rect_bytes = (size_t)n_rects * sizeof(struct ulmk_disp_rect);
	need = sizeof(*hdr) + rect_bytes;
	if (need <= ULMK_DEV_INLINE_BYTES) {
		uint8_t *dst;
		const uint8_t *src;
		size_t i;

		hdr = (struct ulmk_disp_present_hdr *)stack;
		hdr->fb_ptr = (uint32_t)(uintptr_t)fb;
		hdr->n_rects = n_rects;
		if (n_rects && rects) {
			dst = (uint8_t *)(hdr + 1);
			src = (const uint8_t *)rects;
			for (i = 0u; i < rect_bytes; i++)
				dst[i] = src[i];
		}
		return ulmk_write(dev, stack, need);
	}
	/*
	 * Oversized dirty list: present full frame (n_rects=0) — still one
	 * write; board may clean the whole FB.
	 */
	{
		struct ulmk_disp_present_hdr full;

		full.fb_ptr = (uint32_t)(uintptr_t)fb;
		full.n_rects = 0u;
		return ulmk_write(dev, &full, sizeof(full));
	}
}

#endif /* ULMK_DEVICE_DISPLAY_H */
