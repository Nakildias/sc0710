/*
 *  Software scaler for the Elgato 4K60 Pro MK.2 capture card.
 *
 *  The MK.2 lacks the hardware scaler present in the 4K Pro.
 *  This module provides YUYV frame scaling with bilinear interpolation
 *  to upscale (to 3840x2160) or downscale (to 1920x1080) captured frames.
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

/* Determine the output resolution based on the current scaler mode.
 * If the scaler is disabled or the board is not MK.2, output = input.
 */
void sc0710_scaler_get_output_size(struct sc0710_dev *dev,
	u32 src_width, u32 src_height, u32 *out_width, u32 *out_height)
{
	/* Only MK.2 supports software scaling */
	if (dev->board != SC0710_BOARD_ELGATEO_4KP60_MK2 ||
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

/* Scale a YUYV frame using bilinear interpolation.
 *
 * YUYV layout (4:2:2 packed):
 *   Byte 0: Y0  (luma for pixel 0)
 *   Byte 1: Cb  (chroma blue, shared between pixel 0 and 1)
 *   Byte 2: Y1  (luma for pixel 1)
 *   Byte 3: Cr  (chroma red,  shared between pixel 0 and 1)
 *
 * The scaler interpolates luma (Y) per-pixel and chroma (Cb/Cr) per
 * macropixel pair.  This gives quality comparable to the 4K Capture
 * Utility's software scaler.
 *
 * Returns 0 on success, -EINVAL on bad parameters.
 */
int sc0710_scaler_scale_frame(const u8 *src, u32 src_w, u32 src_h,
	u8 *dst, u32 dst_w, u32 dst_h)
{
	u32 dst_y, dst_x;
	u32 src_row_bytes = src_w * 2;
	u32 dst_row_bytes = dst_w * 2;

	/* Fixed-point scaling factors (16.16) for accuracy without floats */
	u32 x_ratio, y_ratio;

	if (!src || !dst || src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2)
		return -EINVAL;

	/* Same resolution — just copy */
	if (src_w == dst_w && src_h == dst_h) {
		memcpy(dst, src, src_w * 2 * src_h);
		return 0;
	}

	x_ratio = ((src_w - 1) << 16) / (dst_w - 1);
	y_ratio = ((src_h - 1) << 16) / (dst_h - 1);

	for (dst_y = 0; dst_y < dst_h; dst_y++) {
		u32 y_fp  = dst_y * y_ratio;
		u32 y_int = y_fp >> 16;
		u32 y_frac = y_fp & 0xFFFF;

		/* Clamp to valid source row range */
		u32 y0 = y_int;
		u32 y1 = (y_int + 1 < src_h) ? y_int + 1 : y_int;

		const u8 *row0 = src + y0 * src_row_bytes;
		const u8 *row1 = src + y1 * src_row_bytes;
		u8 *dst_row = dst + dst_y * dst_row_bytes;

		for (dst_x = 0; dst_x < dst_w; dst_x += 2) {
			u32 x_fp, x_int, x_frac;
			u32 sx0, sx1;
			u32 wx0, wx1, wy0, wy1;

			/* --- Luma for pixel 0 --- */
			x_fp   = dst_x * x_ratio;
			x_int  = x_fp >> 16;
			x_frac = x_fp & 0xFFFF;

			/* Source pixel offsets (Y lives at even byte positions) */
			sx0 = x_int;
			sx1 = (sx0 + 1 < src_w) ? sx0 + 1 : sx0;

			wx0 = 0x10000 - x_frac;
			wx1 = x_frac;
			wy0 = 0x10000 - y_frac;
			wy1 = y_frac;

			{
				/* Bilinear on Y0 */
				u32 y_tl = row0[sx0 * 2];
				u32 y_tr = row0[sx1 * 2];
				u32 y_bl = row1[sx0 * 2];
				u32 y_br = row1[sx1 * 2];

				u32 top = (y_tl * wx0 + y_tr * wx1) >> 16;
				u32 bot = (y_bl * wx0 + y_br * wx1) >> 16;
				dst_row[dst_x * 2] = (u8)((top * wy0 + bot * wy1) >> 16);
			}

			/* --- Chroma (Cb) shared between pixel 0 and 1 ---
			 * In YUYV, Cb is at the byte offset of the macropixel:
			 *   macropixel_start = (pixel & ~1) * 2
			 *   Cb = macropixel_start + 1
			 * We sample chroma at the midpoint between pixel 0 and 1.
			 */
			{
				u32 cx = (dst_x + dst_x + 1) * x_ratio / 2;
				u32 cx_int = cx >> 16;
				u32 cx_frac = cx & 0xFFFF;
				u32 csx0 = cx_int & ~1u;
				u32 csx1 = csx0 + 2;
				u32 cwx0, cwx1;

				if (csx1 >= src_w)
					csx1 = csx0;

				cwx0 = 0x10000 - cx_frac;
				cwx1 = cx_frac;

				{
					/* Cb */
					u32 cb_tl = row0[csx0 * 2 + 1];
					u32 cb_tr = row0[csx1 * 2 + 1];
					u32 cb_bl = row1[csx0 * 2 + 1];
					u32 cb_br = row1[csx1 * 2 + 1];
					u32 top = (cb_tl * cwx0 + cb_tr * cwx1) >> 16;
					u32 bot = (cb_bl * cwx0 + cb_br * cwx1) >> 16;
					dst_row[dst_x * 2 + 1] = (u8)((top * wy0 + bot * wy1) >> 16);
				}

				{
					/* Cr */
					u32 cr_tl = row0[csx0 * 2 + 3];
					u32 cr_tr = row0[csx1 * 2 + 3];
					u32 cr_bl = row1[csx0 * 2 + 3];
					u32 cr_br = row1[csx1 * 2 + 3];
					u32 top = (cr_tl * cwx0 + cr_tr * cwx1) >> 16;
					u32 bot = (cr_bl * cwx0 + cr_br * cwx1) >> 16;
					dst_row[dst_x * 2 + 3] = (u8)((top * wy0 + bot * wy1) >> 16);
				}
			}

			/* --- Luma for pixel 1 --- */
			x_fp   = (dst_x + 1) * x_ratio;
			x_int  = x_fp >> 16;
			x_frac = x_fp & 0xFFFF;

			sx0 = x_int;
			sx1 = (sx0 + 1 < src_w) ? sx0 + 1 : sx0;

			wx0 = 0x10000 - x_frac;
			wx1 = x_frac;

			{
				u32 y_tl = row0[sx0 * 2];
				u32 y_tr = row0[sx1 * 2];
				u32 y_bl = row1[sx0 * 2];
				u32 y_br = row1[sx1 * 2];

				u32 top = (y_tl * wx0 + y_tr * wx1) >> 16;
				u32 bot = (y_bl * wx0 + y_br * wx1) >> 16;
				dst_row[dst_x * 2 + 2] = (u8)((top * wy0 + bot * wy1) >> 16);
			}
		}
	}

	return 0;
}
