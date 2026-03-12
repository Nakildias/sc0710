/*
 *  Software scaler for the Elgato 4K60 Pro MK.2 capture card.
 *
 *  The MK.2 lacks the hardware scaler present in the 4K Pro.
 *  This module provides YUYV frame scaling with nearest-neighbor
 *  interpolation to upscale (to 3840x2160) or downscale (to 1920x1080).
 *
 *  Copyright (c) 2021-2022 Steven Toth <stoth@kernellabs.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sc0710.h"

/* Scaler output dimensions */
#define SCALER_4K_WIDTH   3840
#define SCALER_4K_HEIGHT  2160
#define SCALER_1080_WIDTH 1920
#define SCALER_1080_HEIGHT 1080

const char *sc0710_scaler_mode_name(enum sc0710_scaler_mode mode)
{
	switch (mode) {
	case SCALER_MODE_DISABLED:  return "disabled";
	case SCALER_MODE_UPSCALE:   return "upscale (to 4K)";
	case SCALER_MODE_DOWNSCALE: return "downscale (to 1080P)";
	default:                    return "unknown";
	}
}

bool sc0710_software_scaler_allowed(const struct sc0710_dev *dev)
{
	/* Default policy: MK.2 only. force_software_scaling expands this for testing. */
	return dev->board == SC0710_BOARD_ELGATEO_4KP60_MK2 || force_software_scaling;
}

/* Determine the output resolution based on the current scaler mode.
 * If the scaler is disabled or not allowed on this card, output = input.
 */
void sc0710_scaler_get_output_size(struct sc0710_dev *dev,
	u32 src_width, u32 src_height, u32 *out_width, u32 *out_height)
{
	if (!sc0710_software_scaler_allowed(dev) ||
	    dev->scaler_mode == SCALER_MODE_DISABLED) {
		*out_width = src_width;
		*out_height = src_height;
		return;
	}

	switch (dev->scaler_mode) {
	case SCALER_MODE_UPSCALE:
		*out_width  = SCALER_4K_WIDTH;
		*out_height = SCALER_4K_HEIGHT;
		break;
	case SCALER_MODE_DOWNSCALE:
		*out_width  = SCALER_1080_WIDTH;
		*out_height = SCALER_1080_HEIGHT;
		break;
	default:
		*out_width = src_width;
		*out_height = src_height;
		break;
	}
}

/* Scale a YUYV frame using Nearest-Neighbor interpolation (pixel duplication).
 *
 * YUYV layout (4:2:2 packed):
 *   Byte 0: Y0  (luma for pixel 0)
 *   Byte 1: Cb  (chroma blue, shared between pixel 0 and 1)
 *   Byte 2: Y1  (luma for pixel 1)
 *   Byte 3: Cr  (chroma red,  shared between pixel 0 and 1)
 *
 * This implementation uses macropixel duplication to minimize CPU overhead
 * and avoid blocking the kernel thread, ensuring 60FPS performance.
 *
 * Returns 0 on success, -EINVAL on bad parameters.
 */
int sc0710_scaler_scale_frame(const u8 *src, u32 src_w, u32 src_h,
	u8 *dst, u32 dst_w, u32 dst_h)
{
	u32 dst_y;
	u32 src_row_bytes = src_w * 2;
	u32 dst_row_bytes = dst_w * 2;
	u32 prev_src_y = (u32)-1;

	if (!src || !dst || src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2)
		return -EINVAL;

	if (src_w == dst_w && src_h == dst_h) {
		memcpy(dst, src, (size_t)src_row_bytes * src_h);
		return 0;
	}

	if (src_w == dst_w) {
		/* Height-only scaling: bulk row copy, no per-pixel work */
		for (dst_y = 0; dst_y < dst_h; dst_y++) {
			u32 src_y = (dst_y * src_h) / dst_h;
			u8 *dst_row = dst + ((size_t)dst_y * dst_row_bytes);

			if (src_y == prev_src_y) {
				memcpy(dst_row, dst_row - dst_row_bytes,
				       dst_row_bytes);
			} else {
				memcpy(dst_row,
				       src + ((size_t)src_y * src_row_bytes),
				       src_row_bytes);
			}
			prev_src_y = src_y;
		}
		return 0;
	}

	/* General case: width (and possibly height) scaling.
	 * When consecutive dst rows map to the same src row (upscaling),
	 * we duplicate the already-scaled dst row instead of re-running
	 * the inner loop.  For a 1080p->4K upscale this halves the work.
	 */
	for (dst_y = 0; dst_y < dst_h; dst_y++) {
		u32 src_y = (dst_y * src_h) / dst_h;
		u8 *dst_row = dst + ((size_t)dst_y * dst_row_bytes);

		if (src_y == prev_src_y && dst_y > 0) {
			memcpy(dst_row, dst_row - dst_row_bytes, dst_row_bytes);
		} else {
			const u8 *src_row = src + ((size_t)src_y * src_row_bytes);
			u32 *d32 = (u32 *)dst_row;
			u32 dst_x;

			for (dst_x = 0; dst_x < dst_w; dst_x += 2) {
				u32 src_x = ((dst_x * src_w) / dst_w) & ~1u;
				d32[dst_x >> 1] =
					*(const u32 *)(src_row + src_x * 2);
			}
		}
		prev_src_y = src_y;
	}

	return 0;
}
