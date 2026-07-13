/*
 *  Driver for the Elgato 4k60 Pro MK.2 HDMI capture card.
 *
 *  Copyright (c) 2021-2022 Steven Toth <stoth@kernellabs.com>
 *  Modifications Copyright (c) 2025-2026 Nakildias <nakildiaspro@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/vmalloc.h>

#include "sc0710.h"

static int video_debug = 1;

/* Module parameter to override EOTF detection
 * 0 = auto-detect (default), 1 = force SDR, 2 = force HDR/PQ, 3 = force HLG
 */
int force_eotf = 0;
static int sc0710_param_set_force_eotf(const char *val,
				       const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret)
		sc0710_sync_hdr_deliver_all();
	return ret;
}
static const struct kernel_param_ops sc0710_force_eotf_ops = {
	.set = sc0710_param_set_force_eotf,
	.get = param_get_int,
};
module_param_cb(force_eotf, &sc0710_force_eotf_ops, &force_eotf, 0644);
MODULE_PARM_DESC(force_eotf, "Force EOTF: 0=auto, 1=SDR, 2=HDR-PQ, 3=HLG");

/* Module parameter to override quantization range
 * 0 = auto (default), 1 = force limited range (16-235), 2 = force full range (0-255)
 */
static int force_quantization = 0;
module_param(force_quantization, int, 0644);
MODULE_PARM_DESC(force_quantization, "Force quantization: 0=auto, 1=limited, 2=full");

/* Windows CustomAnalogVideoNativeColorDeepProperty:
 * 0 = 8-bit preference, 1 = request 10-bit from MCU, 2 = AUTO (when HDMI HDR).
 */
int color_deep = 2;
module_param(color_deep, int, 0644);
MODULE_PARM_DESC(color_deep, "MCU color depth prefer: 0=8-bit, 1=10-bit request, 2=auto");

/* Prefer native BGR24 4:4:4 while HDMI is HDR (when not tonemapping).
 * 0=off (leave format alone), 1=auto (BGR24 while HDR), 2=force when 10-bit prefer.
 */
int hdr_bgr24 = 1;
static int sc0710_param_set_hdr_bgr24(const char *val,
				      const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret)
		sc0710_sync_hdr_deliver_all();
	return ret;
}
static const struct kernel_param_ops sc0710_hdr_bgr24_ops = {
	.set = sc0710_param_set_hdr_bgr24,
	.get = param_get_int,
};
module_param_cb(hdr_bgr24, &sc0710_hdr_bgr24_ops, &hdr_bgr24, 0644);
MODULE_PARM_DESC(hdr_bgr24,
	"BGR24 while HDR: 0=off, 1=auto(HDR→BGR24 when not tonemapping), 2=force");

/*
 * MK.2 hardware HDR→SDR tonemap (MCU 0x32 sub 0x11).
 * Matches Windows KS property XET_HDMI_HDR_TO_SDR (722): uint32 on/off.
 * 0=off, 1=auto (HDR-PQ), 2=force. Takes priority over sw_tonemap.
 */
int hw_tonemap = 0;
static int sc0710_param_set_hw_tonemap(const char *val,
				       const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret)
		sc0710_sync_hdr_deliver_all();
	return ret;
}
static const struct kernel_param_ops sc0710_hw_tonemap_ops = {
	.set = sc0710_param_set_hw_tonemap,
	.get = param_get_int,
};
module_param_cb(hw_tonemap, &sc0710_hw_tonemap_ops, &hw_tonemap, 0644);
MODULE_PARM_DESC(hw_tonemap,
	"MK.2 MCU HDR→SDR tonemap: 0=off, 1=auto(HDR-PQ), 2=force (Windows prop 722)");

/*
 * Host-side HDR→SDR tonemap on the captured buffer (CPU).
 * 0=off, 1=auto (when HDMI HDR-PQ), 2=force on.
 * Ignored while hw_tonemap is active (card already mapped).
 */
int sw_tonemap = 1;
static int sc0710_param_set_sw_tonemap(const char *val,
				      const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret)
		sc0710_sync_hdr_deliver_all();
	return ret;
}
static const struct kernel_param_ops sc0710_sw_tonemap_ops = {
	.set = sc0710_param_set_sw_tonemap,
	.get = param_get_int,
};
module_param_cb(sw_tonemap, &sc0710_sw_tonemap_ops, &sw_tonemap, 0644);
MODULE_PARM_DESC(sw_tonemap,
	"SW HDR→SDR tonemap (CPU): 0=off, 1=auto(HDR-PQ), 2=force. Disabled while hw_tonemap active.");

/*
 * YUYV host-tonemap fine-tune (sc0710-hdr-config).
 * Defaults: T=0.80, paper 225, gain 100%, sat 300%.
 */
int tm_yuyv_target = 80;
module_param(tm_yuyv_target, int, 0644);
MODULE_PARM_DESC(tm_yuyv_target,
	"YUYV Reinhard target T×100 (10-80, default 80). Higher = brighter midtones.");

int tm_paper_nits = 225;
module_param(tm_paper_nits, int, 0644);
MODULE_PARM_DESC(tm_paper_nits,
	"YUYV tonemap paper-white nits (50-400, default 225). Higher = dimmer highlights.");

int tm_yuyv_gain = 100;
module_param(tm_yuyv_gain, int, 0644);
MODULE_PARM_DESC(tm_yuyv_gain,
	"YUYV post-tonemap Y gain percent (50-150, default 100).");

int tm_yuyv_chroma = 300;
module_param(tm_yuyv_chroma, int, 0644);
MODULE_PARM_DESC(tm_yuyv_chroma,
	"YUYV host-tonemap chroma percent (0-300, default 300). 100=unity, >100 boosts.");

int tm_yuyv_black = 0;
module_param(tm_yuyv_black, int, 0644);
MODULE_PARM_DESC(tm_yuyv_black,
	"YUYV output black lift above code 16 (0-40, default 0).");

int tm_yuyv_white = 235;
module_param(tm_yuyv_white, int, 0644);
MODULE_PARM_DESC(tm_yuyv_white,
	"YUYV output white clip (180-235, default 235).");

int tm_yuyv_u = 100;
module_param(tm_yuyv_u, int, 0644);
MODULE_PARM_DESC(tm_yuyv_u,
	"YUYV Cb/U scale percent after chroma retain (50-150, default 100).");

int tm_yuyv_v = 100;
module_param(tm_yuyv_v, int, 0644);
MODULE_PARM_DESC(tm_yuyv_v,
	"YUYV Cr/V scale percent after chroma retain (50-150, default 100).");

int tm_yuyv_pq_bias = 0;
module_param(tm_yuyv_pq_bias, int, 0644);
MODULE_PARM_DESC(tm_yuyv_pq_bias,
	"YUYV PQ index bias before nits lookup (-40..40, default 0).");

int tm_yuyv_oetf = 0;
module_param(tm_yuyv_oetf, int, 0644);
MODULE_PARM_DESC(tm_yuyv_oetf,
	"YUYV output OETF: 0=Rec.709 limited, 1=sRGB→limited, 2=linear→limited.");

/*
 * BGR24 host-tonemap fine-tune (sc0710-hdr-config 4:4:4 section).
 * Defaults: T=0.50, paper 400 nits, gain 150%, sat 150% (user-tuned look).
 * Baked gold path (T=0.58 / paper 203) still used when knobs match those.
 */
int tm_bgr_target = 50;
module_param(tm_bgr_target, int, 0644);
MODULE_PARM_DESC(tm_bgr_target,
	"BGR24 Reinhard target T×100 (10-80, default 50). Higher = brighter midtones.");

int tm_bgr_paper = 400;
module_param(tm_bgr_paper, int, 0644);
MODULE_PARM_DESC(tm_bgr_paper,
	"BGR24 tonemap paper-white nits (50-400, default 400). Higher = dimmer highlights.");

int tm_bgr_gain = 150;
module_param(tm_bgr_gain, int, 0644);
MODULE_PARM_DESC(tm_bgr_gain,
	"BGR24 post-tonemap RGB gain percent (50-150, default 150).");

int tm_bgr_chroma = 150;
module_param(tm_bgr_chroma, int, 0644);
MODULE_PARM_DESC(tm_bgr_chroma,
	"BGR24 post-tonemap saturation percent (0-300, default 150). 100=unity, >100 boosts.");

/* Module parameter to enable status images (No Signal/No Device BMP)
 * 1 = show BMP images (default), 0 = show colorbars
 */
int use_status_images = 1;
module_param(use_status_images, int, 0644);
MODULE_PARM_DESC(use_status_images, "Show status images (1) or colorbars (0)");

#define dprintk(level, fmt, arg...)\
        do { if (sc0710_debug_mode && video_debug >= level)\
                printk(KERN_DEBUG "%s: " fmt, dev->name, ## arg);\
        } while (0)

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
static void sc0710_vid_timeout(unsigned long data);
#else
static void sc0710_vid_timeout(struct timer_list *t);
#endif

const char *sc0710_colorimetry_ascii(enum sc0710_colorimetry_e val)
{
	switch (val) {
	case BT_601:       return "BT_601";
	case BT_709:       return "BT_709";
	case BT_2020:      return "BT_2020";
	default:           return "BT_UNDEFINED";
	}
}

const char *sc0710_colorspace_ascii(enum sc0710_colorspace_e val)
{
	switch (val) {
	case CS_YUV_YCRCB_422_420: return "YUV YCrCb 4:2:2 / 4:2:0";
	case CS_YUV_YCRCB_444:     return "YUV YCrCb 4:4:4";
	case CS_RGB_444:           return "RGB 4:4:4";
	default:                   return "UNDEFINED";
	}
}

/* Map detected colorimetry to V4L2 colorspace */
static enum v4l2_colorspace sc0710_get_v4l2_colorspace(struct sc0710_dev *dev)
{
	/* Host tonemap produces SDR Rec.709-looking pixels. Passthrough
	 * (tonemap off) keeps the source colorimetry — typically BT.2020. */
	if (sc0710_want_hw_tonemap(dev) || sc0710_want_sw_tonemap(dev))
		return V4L2_COLORSPACE_REC709;

	switch (dev->colorimetry) {
	case BT_601:  return V4L2_COLORSPACE_SMPTE170M;
	case BT_709:  return V4L2_COLORSPACE_REC709;
	case BT_2020: return V4L2_COLORSPACE_BT2020;
	default:      return V4L2_COLORSPACE_SRGB;
	}
}

/* Map detected colorimetry to V4L2 transfer function.
 * Tonemap path: pixels are SDR → advertise default transfer.
 * Passthrough: keep PQ/HLG from effective EOTF (force_eotf or detect).
 */
static enum v4l2_xfer_func sc0710_get_v4l2_xfer_func(struct sc0710_dev *dev)
{
	if (sc0710_want_hw_tonemap(dev) || sc0710_want_sw_tonemap(dev))
		return V4L2_XFER_FUNC_DEFAULT;

	switch (sc0710_effective_eotf(dev)) {
	case EOTF_HDR_PQ:
	case EOTF_HDR_HLG:
		return V4L2_XFER_FUNC_SMPTE2084;
	case EOTF_SDR:
	case EOTF_UNKNOWN:
	default:
		return V4L2_XFER_FUNC_DEFAULT;
	}
}

/* Map detected colorimetry to V4L2 Y'CbCr encoding */
static enum v4l2_ycbcr_encoding sc0710_get_v4l2_ycbcr_enc(struct sc0710_dev *dev)
{
	if (sc0710_want_hw_tonemap(dev) || sc0710_want_sw_tonemap(dev))
		return V4L2_YCBCR_ENC_709;

	switch (dev->colorimetry) {
	case BT_2020: return V4L2_YCBCR_ENC_BT2020;
	case BT_709:  return V4L2_YCBCR_ENC_709;
	case BT_601:  return V4L2_YCBCR_ENC_601;
	default:      return V4L2_YCBCR_ENC_DEFAULT;
	}
}

/* Get quantization range
 * Limited range (16-235) vs Full range (0-255) can cause washed-out appearance
 * if mismatched between source and sink.
 */
static enum v4l2_quantization sc0710_get_v4l2_quantization(struct sc0710_dev *dev)
{
	/* Allow manual override via module parameter */
	switch (force_quantization) {
	case 1: return V4L2_QUANTIZATION_LIM_RANGE;  /* Force limited (16-235) */
	case 2: return V4L2_QUANTIZATION_FULL_RANGE; /* Force full (0-255) */
	}

	if (sc0710_want_hw_tonemap(dev) || sc0710_want_sw_tonemap(dev))
		return V4L2_QUANTIZATION_LIM_RANGE;

	/* Auto: BT.2020 typically uses limited range, sRGB uses full */
	if (dev->colorimetry == BT_2020)
		return V4L2_QUANTIZATION_LIM_RANGE;
	return V4L2_QUANTIZATION_DEFAULT;
}

/* The selectable capture formats. 0xD0 bit 6 selects the packed 4:4:4
 * capture format (BGR byte order); clear = YUYV 4:2:2. */
const struct sc0710_pixfmt sc0710_pixfmts[] = {
	{
		.fourcc      = V4L2_PIX_FMT_YUYV,  /* packed 4:2:2, default */
		.bpp         = 2,
		.pipeline_d0 = 0x4100,
		.weave_ok    = true,
		.tear_ok     = true,
	}, {
		.fourcc      = V4L2_PIX_FMT_BGR24, /* packed 4:4:4 */
		.bpp         = 3,
		.pipeline_d0 = 0x4140,
		.rgb         = true,
	},
};

const unsigned int sc0710_pixfmts_count = ARRAY_SIZE(sc0710_pixfmts);

/* The table row for fourcc, or NULL when unknown. */
static const struct sc0710_pixfmt *sc0710_pixfmt_find(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < sc0710_pixfmts_count; i++) {
		if (sc0710_pixfmts[i].fourcc == fourcc)
			return &sc0710_pixfmts[i];
	}
	return NULL;
}

/* Fill the colorimetry fields for the negotiated pixel format.
 * RGB + HDR passthrough (no host tonemap): BT.2020 / PQ / limited.
 * RGB otherwise: full-range sRGB (measured black 0, white 254-255).
 * YUYV + tonemap: Rec.709 / default transfer / limited.
 * Else: detected path via effective EOTF. */
static void sc0710_fill_colorimetry(struct sc0710_dev *dev,
	const struct sc0710_pixfmt *pf, struct v4l2_pix_format *pix)
{
	if (pf->rgb) {
		if (sc0710_hdmi_is_hdr(dev) &&
		    !sc0710_want_hw_tonemap(dev) &&
		    !sc0710_want_sw_tonemap(dev)) {
			pix->colorspace = V4L2_COLORSPACE_BT2020;
			pix->xfer_func = V4L2_XFER_FUNC_SMPTE2084;
			pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
			pix->quantization = force_quantization == 2 ?
				V4L2_QUANTIZATION_FULL_RANGE :
				V4L2_QUANTIZATION_LIM_RANGE;
			return;
		}
		pix->colorspace = V4L2_COLORSPACE_SRGB;
		pix->xfer_func = V4L2_XFER_FUNC_SRGB;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		pix->quantization = force_quantization == 1 ?
			V4L2_QUANTIZATION_LIM_RANGE :
			V4L2_QUANTIZATION_FULL_RANGE;
		return;
	}
	pix->colorspace = sc0710_get_v4l2_colorspace(dev);
	pix->xfer_func = sc0710_get_v4l2_xfer_func(dev);
	pix->ycbcr_enc = sc0710_get_v4l2_ycbcr_enc(dev);
	pix->quantization = sc0710_get_v4l2_quantization(dev);
}

#define FILL_MODE_COLORBARS 0
#define FILL_MODE_GREENSCREEN 1
#define FILL_MODE_BLUESCREEN 2
#define FILL_MODE_BLACKSCREEN 3
#define FILL_MODE_REDSCREEN 4
#define FILL_MODE_NOSIGNAL 5
#define FILL_MODE_NODEVICE 6

/* Include hybrid-optimized status image header (gradient + sparse overlays) */
#include "sc0710-img-hybrid-optimized.h"

/* Static buffers for generated status images */
static unsigned char *nosignal_frame_buffer = NULL;
static unsigned char *nodevice_frame_buffer = NULL;
static int status_frames_generated = 0;
/* Guards generation: two clients' first STREAMONs run under distinct vb2
 * locks, so nothing else serializes the lazy init. */
static DEFINE_MUTEX(status_frames_lock);

/* 75% IRE colorbars */
static unsigned char colorbars[7][4] =
{
	{ 0xc0, 0x80, 0xc0, 0x80 },
	{ 0xaa, 0x20, 0xaa, 0x8f },
	{ 0x86, 0xa0, 0x86, 0x20 },
	{ 0x70, 0x40, 0x70, 0x2f },
	{ 0x4f, 0xbf, 0x4f, 0xd0 },
	{ 0x39, 0x5f, 0x39, 0xe0 },
	{ 0x15, 0xe0, 0x15, 0x70 }
};
static unsigned char blackscreen[4] = { 0x00, 0x80, 0x00, 0x80 };
static unsigned char bluescreen[4] = { 0x1d, 0xff, 0x1d, 0x6b };
static unsigned char redscreen[4] = { 0x39, 0x5f, 0x39, 0xe0 };

/* Helper function to scale and copy a status image to the destination buffer.
 * Uses nearest-neighbor scaling to convert source image to target size.
 * Source is in YUYV format (2 bytes per pixel).
 */
static void fill_frame_from_image(unsigned char *dest_frame,
	unsigned int dest_width, unsigned int dest_height,
	const unsigned char *src_data,
	unsigned int src_width, unsigned int src_height)
{
	unsigned int dest_y, dest_x;
	unsigned int dest_row_bytes = dest_width * 2;
	unsigned int src_row_bytes = src_width * 2;

	if (!dest_frame || !src_data || src_width == 0 || src_height == 0 || dest_width == 0 || dest_height == 0) {
		printk_ratelimited(KERN_ERR "sc0710: fill_frame_from_image invalid params\n");
		return;
	}

	for (dest_y = 0; dest_y < dest_height; dest_y++) {
		/* Calculate source Y coordinate (nearest neighbor) */
		unsigned int src_y = (dest_y * src_height) / dest_height;
		const unsigned char *src_row = src_data + (src_y * src_row_bytes);
		unsigned char *dest_row = dest_frame + (dest_y * dest_row_bytes);

		for (dest_x = 0; dest_x < dest_width; dest_x += 2) {
			/* Calculate source X coordinate (YUYV is 2 pixels per 4 bytes) */
			unsigned int src_x = ((dest_x * src_width) / dest_width) & ~1;
			const unsigned char *src_pixel = src_row + (src_x * 2);
			unsigned char *dest_pixel = dest_row + (dest_x * 2);

			/* Copy YUYV macropixel (4 bytes = 2 pixels) */
			memcpy(dest_pixel, src_pixel, 4);
		}
	}
}

/* Generate a complete YUYV frame from 1bpp sprites */
static void generate_status_frame(unsigned char *dest, const struct overlay_sprite *msg_sprite)
{
	unsigned int r, c;
	unsigned int dest_w = STATUS_IMAGE_WIDTH;
	unsigned int dest_h = STATUS_IMAGE_HEIGHT;
	const struct overlay_sprite *sprites[2];
	int i;

	sprites[0] = &shared_logo_sprite;
	sprites[1] = msg_sprite;

	/* 1. Fill background with dark gray (Y=0x20, U=0x80, V=0x80) */
	for (r = 0; r < dest_h; r++) {
		for (c = 0; c < dest_w; c += 2) {
			unsigned int offset = (r * dest_w * 2) + (c * 2);
			dest[offset]   = 0x20; /* Y0 */
			dest[offset+1] = 0x80; /* U  */
			dest[offset+2] = 0x20; /* Y1 */
			dest[offset+3] = 0x80; /* V  */
		}
	}

	/* 2. Draw sprites (1bpp packed, where 1 = white) */
	for (i = 0; i < 2; i++) {
		const struct overlay_sprite *sprite = sprites[i];
		if (sprite && sprite->data) {
			unsigned int stride = sprite->w / 8;
			for (r = 0; r < sprite->h; r++) {
				unsigned int dy = sprite->y + r;
				if (dy >= dest_h) break;

				for (c = 0; c < sprite->w; c++) {
					unsigned int dx = sprite->x + c;
					if (dx >= dest_w) continue;

					if (sprite->data[r * stride + (c / 8)] & (1 << (c % 8))) {
						/* Set pixel to white (Y=0xEB, U=0x80, V=0x80) */
						unsigned int offset = (dy * dest_w * 2) + (dx * 2);
						dest[offset] = 0xEB; /* Y */
						dest[offset+1] = 0x80; /* U for even dx, V for odd dx */
					}
				}
			}
		}
	}
}

/* Generate status frames from hybrid-optimized gradient + overlay data.
 * Called once lazily on first use.
 */
static void generate_status_frames_if_needed(void)
{
	unsigned int frame_size = STATUS_IMAGE_WIDTH * STATUS_IMAGE_HEIGHT * 2;
	unsigned char *nosignal, *nodevice;

	mutex_lock(&status_frames_lock);
	if (status_frames_generated) {
		mutex_unlock(&status_frames_lock);
		return;
	}

	/* Build both frames completely before publishing the pointers: the
	 * timer path reads them without the lock, so a half-drawn frame or an
	 * error-path vfree must never be visible through them. */
	nosignal = vmalloc(frame_size);
	nodevice = vmalloc(frame_size);
	if (!nosignal || !nodevice) {
		vfree(nosignal);
		vfree(nodevice);
		printk(KERN_WARNING "sc0710: Failed to allocate status frame buffers\n");
		mutex_unlock(&status_frames_lock);
		return;
	}

	/* Generate frames from overlays */
	generate_status_frame(nosignal, &no_signal_sprite);
	generate_status_frame(nodevice, &no_device_sprite);

	nosignal_frame_buffer = nosignal;
	nodevice_frame_buffer = nodevice;
	status_frames_generated = 1;
	printk(KERN_INFO "sc0710: Generated status frames from hybrid-optimized data\n");
	mutex_unlock(&status_frames_lock);
}

/* Called at module exit, after every device (and so every timer that could
 * read these) is gone. */
void sc0710_video_free_status_frames(void)
{
	mutex_lock(&status_frames_lock);
	vfree(nosignal_frame_buffer);
	nosignal_frame_buffer = NULL;
	vfree(nodevice_frame_buffer);
	nodevice_frame_buffer = NULL;
	status_frames_generated = 0;
	mutex_unlock(&status_frames_lock);
}

/* Compute the output dimensions and frame size for a detected format. */
static void sc0710_get_effective_size(struct sc0710_dev *dev,
	const struct sc0710_format *fmt, u32 *width, u32 *height, u32 *framesize)
{
	*width = fmt->width;
	*height = fmt->height;
	*framesize = sc0710_framesize(dev, fmt);
}

static void fill_frame(struct sc0710_dma_channel *ch,
	unsigned char *dest_frame, unsigned int width,
	unsigned int height, unsigned int fillmode)
{
	unsigned int width_bytes = width * 2;
	unsigned int i, divider;

	/* Handle status images with scaling */
	if (fillmode == FILL_MODE_NOSIGNAL && use_status_images) {
		if (nosignal_frame_buffer) {
			fill_frame_from_image(dest_frame, width, height,
				nosignal_frame_buffer,
				STATUS_IMAGE_WIDTH,
				STATUS_IMAGE_HEIGHT);
			return;
		}
		/* Fall through to colorbars if generation failed */
	}

	if (fillmode == FILL_MODE_NODEVICE && use_status_images) {
		if (nodevice_frame_buffer) {
			fill_frame_from_image(dest_frame, width, height,
				nodevice_frame_buffer,
				STATUS_IMAGE_WIDTH,
				STATUS_IMAGE_HEIGHT);
			return;
		}
		/* Fall through to colorbars if generation failed */
	}

	/* Fall back to colorbars if status images disabled */
	if ((fillmode == FILL_MODE_NOSIGNAL || fillmode == FILL_MODE_NODEVICE)
		&& !use_status_images)
		fillmode = FILL_MODE_COLORBARS;

	if (fillmode > FILL_MODE_REDSCREEN)
		fillmode = FILL_MODE_BLACKSCREEN;

	switch (fillmode) {
	case FILL_MODE_COLORBARS:
		divider = (width_bytes / 7) + 1;
		for (i = 0; i < width_bytes; i += 4)
			memcpy(&dest_frame[i], &colorbars[i / divider], 4);
		break;
	case FILL_MODE_GREENSCREEN:
		memset(dest_frame, 0, width_bytes);
		break;
	case FILL_MODE_BLUESCREEN:
		for (i = 0; i < width_bytes; i += 4)
			memcpy(&dest_frame[i], bluescreen, 4);
		break;
	case FILL_MODE_REDSCREEN:
		for (i = 0; i < width_bytes; i += 4)
			memcpy(&dest_frame[i], redscreen, 4);
		break;
	case FILL_MODE_BLACKSCREEN:
		for (i = 0; i < width_bytes; i += 4)
			memcpy(&dest_frame[i], blackscreen, 4);
	}

	for (i = 1; i < height; i++) {
		memcpy(dest_frame + width_bytes, dest_frame, width_bytes);
		dest_frame += width_bytes;
	}
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 0, 0)
/* Let's assume these appeared in v4.0 */

#define V4L2_DV_FL_IS_CE_VIDEO			(1 << 4)
#define V4L2_DV_FL_HAS_CEA861_VIC		(1 << 7)
#define V4L2_DV_FL_HAS_HDMI_VIC			(1 << 8)

#define V4L2_DV_BT_CEA_3840X2160P24 { \
	.type = V4L2_DV_BT_656_1120, \
	V4L2_INIT_BT_TIMINGS(3840, 2160, 0, \
		V4L2_DV_HSYNC_POS_POL | V4L2_DV_VSYNC_POS_POL, \
		297000000, 1276, 88, 296, 8, 10, 72, 0, 0, 0, \
		V4L2_DV_FL_CAN_REDUCE_FPS | V4L2_DV_FL_IS_CE_VIDEO | \
		V4L2_DV_FL_HAS_CEA861_VIC | V4L2_DV_FL_HAS_HDMI_VIC), \
}

#define V4L2_DV_BT_CEA_3840X2160P25 { \
	.type = V4L2_DV_BT_656_1120, \
	V4L2_INIT_BT_TIMINGS(3840, 2160, 0, \
		V4L2_DV_HSYNC_POS_POL | V4L2_DV_VSYNC_POS_POL, \
		297000000, 1056, 88, 296, 8, 10, 72, 0, 0, 0, \
		V4L2_DV_FL_IS_CE_VIDEO | V4L2_DV_FL_HAS_CEA861_VIC | \
		V4L2_DV_FL_HAS_HDMI_VIC), \
}

#define V4L2_DV_BT_CEA_3840X2160P30 { \
	.type = V4L2_DV_BT_656_1120, \
	V4L2_INIT_BT_TIMINGS(3840, 2160, 0, \
		V4L2_DV_HSYNC_POS_POL | V4L2_DV_VSYNC_POS_POL, \
		297000000, 176, 88, 296, 8, 10, 72, 0, 0, 0, \
		V4L2_DV_FL_CAN_REDUCE_FPS | V4L2_DV_FL_IS_CE_VIDEO | \
		V4L2_DV_FL_HAS_CEA861_VIC | V4L2_DV_FL_HAS_HDMI_VIC, \
		) \
}

#define V4L2_DV_BT_CEA_3840X2160P50 { \
	.type = V4L2_DV_BT_656_1120, \
	V4L2_INIT_BT_TIMINGS(3840, 2160, 0, \
		V4L2_DV_HSYNC_POS_POL | V4L2_DV_VSYNC_POS_POL, \
		594000000, 1056, 88, 296, 8, 10, 72, 0, 0, 0, \
		V4L2_DV_FL_IS_CE_VIDEO | V4L2_DV_FL_HAS_CEA861_VIC, ) \
}

#define V4L2_DV_BT_CEA_3840X2160P60 { \
	.type = V4L2_DV_BT_656_1120, \
	V4L2_INIT_BT_TIMINGS(3840, 2160, 0, \
		V4L2_DV_HSYNC_POS_POL | V4L2_DV_VSYNC_POS_POL, \
		594000000, 176, 88, 296, 8, 10, 72, 0, 0, 0, \
		V4L2_DV_FL_CAN_REDUCE_FPS | V4L2_DV_FL_IS_CE_VIDEO | \
		V4L2_DV_FL_HAS_CEA861_VIC,) \
}
#endif /* #if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 0, 0) */

#define SUPPORT_INTERLACED 1
static struct sc0710_format formats[] =
{
	/* 640x480 - VGA */
	{  800,  525,  640,  480, 0, 6000, 60000, 1000, 8, 0, "640x480p60",      V4L2_DV_BT_DMT_640X480P60 },
	{  832,  520,  640,  480, 0, 7500, 75000, 1000, 8, 0, "640x480p75",      V4L2_DV_BT_DMT_640X480P75 },

	/* 720x480 - SD NTSC */
#if SUPPORT_INTERLACED
	{  858,  262,  720,  480, 1, 2997, 30000, 1001, 8, 0, "720x480i29.97",   V4L2_DV_BT_CEA_720X480I59_94 },
#endif
	{  858,  525,  720,  480, 0, 5994, 60000, 1001, 8, 0, "720x480p59.94",   V4L2_DV_BT_CEA_720X480P59_94 },

	/* 720x576 - SD PAL */
#if SUPPORT_INTERLACED
	{  864,  312,  720,  576, 1, 2500, 25000, 1000, 8, 0, "720x576i25",      V4L2_DV_BT_CEA_720X576I50 },
#endif
	{  864,  625,  720,  576, 0, 5000, 50000, 1000, 8, 0, "720x576p50",      V4L2_DV_BT_CEA_720X576P50 },

	/* 800x600 - SVGA */
	{ 1056,  628,  800,  600, 0, 6000, 60000, 1000, 8, 0, "800x600p60",      V4L2_DV_BT_DMT_800X600P60 },
	{ 1040,  666,  800,  600, 0, 7500, 75000, 1000, 8, 0, "800x600p75",      V4L2_DV_BT_DMT_800X600P75 },
	{  960,  636,  800,  600, 0, 11997, 120000, 1001, 8, 0, "800x600p119.97", V4L2_DV_BT_DMT_800X600P75 },
	{ 1056,  636,  800,  600, 0, 11988, 120000, 1001, 8, 0, "800x600p119.88", V4L2_DV_BT_DMT_800X600P75 },
	{ 1056,  636,  800,  600, 0, 12000, 120000, 1000, 8, 0, "800x600p120",    V4L2_DV_BT_DMT_800X600P75 },

	/* 1024x768 - XGA */
	{ 1344,  806, 1024,  768, 0, 6000, 60000, 1000, 8, 0, "1024x768p60",     V4L2_DV_BT_DMT_1024X768P60 },
	{ 1312,  800, 1024,  768, 0, 7500, 75000, 1000, 8, 0, "1024x768p75",     V4L2_DV_BT_DMT_1024X768P75 },

	/* 1280x720 - HD 720p */
	{ 1980,  750, 1280,  720, 0, 5000, 50000, 1000, 8, 0, "1280x720p50",     V4L2_DV_BT_CEA_1280X720P50 },
	{ 1650,  750, 1280,  720, 0, 5994, 60000, 1001, 8, 0, "1280x720p59.94",  V4L2_DV_BT_CEA_1280X720P60 },
	{ 1650,  750, 1280,  720, 0, 6000, 60000, 1000, 8, 0, "1280x720p60",     V4L2_DV_BT_CEA_1280X720P60 },

	/* 1280x1024 - SXGA */
	{ 1688, 1066, 1280, 1024, 0, 6000, 60000, 1000, 8, 0, "1280x1024p60",    V4L2_DV_BT_DMT_1280X1024P60 },
	{ 1688, 1066, 1280, 1024, 0, 7500, 75000, 1000, 8, 0, "1280x1024p75",    V4L2_DV_BT_DMT_1280X1024P75 },

	/* 1920x1080 - Full HD */
#if SUPPORT_INTERLACED
	{ 2640,  562, 1920, 1080, 1, 2500, 25000, 1000, 8, 0, "1920x1080i25",    V4L2_DV_BT_CEA_1920X1080I50 },
	{ 2200,  562, 1920, 1080, 1, 2997, 30000, 1001, 8, 0, "1920x1080i29.97", V4L2_DV_BT_CEA_1920X1080I60 },
#endif
	{ 2750, 1125, 1920, 1080, 0, 2400, 24000, 1000, 8, 0, "1920x1080p24",    V4L2_DV_BT_CEA_1920X1080P24 },
	{ 2640, 1125, 1920, 1080, 0, 2500, 25000, 1000, 8, 0, "1920x1080p25",    V4L2_DV_BT_CEA_1920X1080P25 },
	{ 2200, 1125, 1920, 1080, 0, 3000, 30000, 1000, 8, 0, "1920x1080p30",    V4L2_DV_BT_CEA_1920X1080P30 },
	{ 2640, 1125, 1920, 1080, 0, 5000, 50000, 1000, 8, 0, "1920x1080p50",    V4L2_DV_BT_CEA_1920X1080P50 },
	{ 2200, 1125, 1920, 1080, 0, 6000, 60000, 1000, 8, 0, "1920x1080p60",    V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2200, 1125, 1920, 1080, 0, 11988, 120000, 1001, 8, 0, "1920x1080p119.88", V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2200, 1125, 1920, 1080, 0, 12000, 120000, 1000, 8, 0, "1920x1080p120",   V4L2_DV_BT_CEA_1920X1080P60 },
	/* CVT Reduced Blanking - common on laptops/monitors for high refresh rates */
	{ 2000, 1144, 1920, 1080, 0, 12000, 120000, 1000, 8, 0, "1920x1080p120cvt", V4L2_DV_BT_CEA_1920X1080P60 },
	/* 1080p 240Hz - CVT-RB timing (2080x1310 total) */
	{ 2080, 1310, 1920, 1080, 0, 24000, 240000, 1000, 8, 0, "1920x1080p240",   V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2080, 1310, 1920, 1080, 0, 23976, 240000, 1001, 8, 0, "1920x1080p239.76", V4L2_DV_BT_CEA_1920X1080P60 },

	/* 1920x1200 - WUXGA */
	{ 2592, 1245, 1920, 1200, 0, 6000, 60000, 1000, 8, 0, "1920x1200p60",    V4L2_DV_BT_DMT_1920X1200P60 },
	/* CVT Reduced Blanking variant */
	{ 2080, 1235, 1920, 1200, 0, 6000, 60000, 1000, 8, 0, "1920x1200p60rb",  V4L2_DV_BT_DMT_1920X1200P60 },

	/* 2560x1440 - QHD/WQHD */
	/* Multiple timing variants detected from different sources */
	{ 2720, 1481, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120a",  V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2720, 1524, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120b",  V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2720, 1525, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120c",  V4L2_DV_BT_CEA_1920X1080P60 },
	/* CVT and alternate timings */
	{ 2720, 1510, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120alt", V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2640, 1490, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120cvt", V4L2_DV_BT_CEA_1920X1080P60 },
	/* 60Hz variants */
	{ 2720, 1481, 2560, 1440, 0, 6000, 60000, 1000, 8, 0, "2560x1440p60",     V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2720, 1500, 2560, 1440, 0, 6000, 60000, 1000, 8, 0, "2560x1440p60alt",  V4L2_DV_BT_CEA_1920X1080P60 },
	/* 144Hz variants */
	{ 2720, 1527, 2560, 1440, 0, 14400, 144000, 1000, 8, 0, "2560x1440p144",   V4L2_DV_BT_CEA_1920X1080P60 },

	/* 3840x2160 - 4K UHD */
	{ 5500, 2250, 3840, 2160, 0, 2400, 24000, 1000, 8, 0, "3840x2160p24",    V4L2_DV_BT_CEA_3840X2160P24 },
	{ 5280, 2250, 3840, 2160, 0, 2500, 25000, 1000, 8, 0, "3840x2160p25",    V4L2_DV_BT_CEA_3840X2160P25 },
	{ 4400, 2250, 3840, 2160, 0, 3000, 30000, 1000, 8, 0, "3840x2160p30",    V4L2_DV_BT_CEA_3840X2160P30 },
	{ 5280, 2250, 3840, 2160, 0, 5000, 50000, 1000, 8, 0, "3840x2160p50",    V4L2_DV_BT_CEA_3840X2160P50 },
	{ 4400, 2250, 3840, 2160, 0, 5994, 60000, 1001, 8, 0, "3840x2160p59.94", V4L2_DV_BT_CEA_3840X2160P60 },
	{ 4400, 2250, 3840, 2160, 0, 6000, 60000, 1000, 8, 0, "3840x2160p60",    V4L2_DV_BT_CEA_3840X2160P60 },
	/* Alternate 4K timings with larger blanking */
	{ 5500, 2250, 3840, 2160, 0, 4800, 48000, 1000, 8, 0, "3840x2160p48",    V4L2_DV_BT_CEA_3840X2160P60 },

	/* 4096x2160 - DCI 4K */
	{ 4400, 2250, 4096, 2160, 0, 2400, 24000, 1000, 8, 0, "4096x2160p24",    V4L2_DV_BT_CEA_3840X2160P24 },
	{ 4400, 2250, 4096, 2160, 0, 2500, 25000, 1000, 8, 0, "4096x2160p25",    V4L2_DV_BT_CEA_3840X2160P25 },
	{ 4400, 2250, 4096, 2160, 0, 3000, 30000, 1000, 8, 0, "4096x2160p30",    V4L2_DV_BT_CEA_3840X2160P30 },
	{ 4400, 2250, 4096, 2160, 0, 5000, 50000, 1000, 8, 0, "4096x2160p50",    V4L2_DV_BT_CEA_3840X2160P50 },
	{ 4400, 2250, 4096, 2160, 0, 6000, 60000, 1000, 8, 0, "4096x2160p60",    V4L2_DV_BT_CEA_3840X2160P60 },
};

/* Default format for no-signal mode (1920x1080p60) */
static struct sc0710_format default_no_signal_format = {
	.timingH = 2200,
	.timingV = 1125,
	.width = 1920,
	.height = 1080,
	.interlaced = 0,
	.fpsX100 = 6000,
	.fpsnum = 60000,
	.fpsden = 1000,
	.depth = 8,
	.framesize = 1920 * 2 * 1080,  /* YUV 4:2:2 */
	.name = "No Signal (1920x1080)",
	.dv_timings = V4L2_DV_BT_CEA_1920X1080P60,
};

/* Get the default format for no-signal mode */
const struct sc0710_format *sc0710_get_default_format(void)
{
	return &default_no_signal_format;
}

void sc0710_format_initialize(void)
{
	struct sc0710_format *fmt;
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		struct v4l2_bt_timings *bt;
		u64 pixelclock, clk_delta;

		fmt = &formats[i];

		/* Assuming YUV 8-bit */
		fmt->framesize = fmt->width * 2 * fmt->height;

		/* Entries with no matching kernel timing macro carry a
		 * nearest-neighbour placeholder in the static table (the
		 * 2560x1440 modes point at the 1080p60 macro), which
		 * QUERY/G/ENUM_DV_TIMINGS must not report. Where the macro
		 * disagrees with the entry's own numbers, synthesize the
		 * timing from those numbers; the blanking split is unknown,
		 * so it lands in the back porches (clients consume
		 * width/height/totals/pixelclock).
		 *
		 * The pixelclock comparison is 1%-tolerant: several table
		 * entries round the field rate (60 for DMT/CVT 59.94/60.317,
		 * 59.94 stored as 60000/1001 against a nominal CEA clock), and
		 * an exact compare would discard their correct macros with all
		 * the standards flags. Real placeholders differ by dims or by
		 * 2x+ clock (the >60Hz rows carry a 60Hz macro), far past 1%.
		 */
		if (fmt->interlaced)
			continue; /* both interlaced entries carry their exact CEA macro */
		bt = &fmt->dv_timings.bt;
		pixelclock = div_u64((u64)fmt->timingH * fmt->timingV * fmt->fpsnum,
			fmt->fpsden);
		clk_delta = bt->pixelclock > pixelclock ?
			bt->pixelclock - pixelclock : pixelclock - bt->pixelclock;
		if (bt->width == fmt->width && bt->height == fmt->height &&
		    V4L2_DV_BT_FRAME_WIDTH(bt) == fmt->timingH &&
		    V4L2_DV_BT_FRAME_HEIGHT(bt) == fmt->timingV &&
		    clk_delta * 100 <= pixelclock)
			continue;
		memset(bt, 0, sizeof(*bt));
		bt->width = fmt->width;
		bt->height = fmt->height;
		bt->pixelclock = pixelclock;
		bt->hbackporch = fmt->timingH - fmt->width;
		bt->vbackporch = fmt->timingV - fmt->height;
	}
}

const struct sc0710_format *sc0710_format_find_by_timing(u32 timingH, u32 timingV)
{
	unsigned int i;

	/* First try matching against total timing (htotal/vtotal).
	 * Most sources report total timing in pixelLineH/V.
	 */
	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if ((formats[i].timingH == timingH) && (formats[i].timingV == timingV)) {
			return &formats[i];
		}
	}

	/* Fall back to matching against active resolution (width/height).
	 * Some sources (e.g. Nintendo Switch 2) report active pixel
	 * dimensions in pixelLineH/V instead of total timing.
	 */
	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if ((formats[i].width == timingH) && (formats[i].height == timingV)) {
			return &formats[i];
		}
	}

	return NULL;
}

const struct sc0710_format *sc0710_format_find_by_timing_and_rate(u32 timingH, u32 timingV, u32 target_fps)
{
	unsigned int i;
	const struct sc0710_format *best_fmt = NULL;
	u32 best_diff = 0xFFFFFFFF;
	int pass;

	if (sc0710_debug_mode)
		printk(KERN_INFO "sc0710: Match %ux%u TargetFPS=%u\n", timingH, timingV, target_fps);

	/* Pass 0: match against total timing (timingH/timingV).
	 * Pass 1: fall back to active resolution (width/height).
	 * Most sources report total timing (e.g. 2200x1125 for 1080p).
	 * Some sources report active resolution (e.g. 1920x1080).
	 */
	for (pass = 0; pass < 2; pass++) {
		best_fmt = NULL;
		best_diff = 0xFFFFFFFF;

		for (i = 0; i < ARRAY_SIZE(formats); i++) {
			int match;
			u32 fps, diff;

			if (pass == 0)
				match = (formats[i].timingH == timingH) && (formats[i].timingV == timingV);
			else
				match = (formats[i].width == timingH) && (formats[i].height == timingV);

			if (!match)
				continue;

			fps = formats[i].fpsX100 / 100;

			/* If no hint, return first match (legacy behavior) */
			if (target_fps == 0) {
				printk(KERN_INFO "sc0710: No FPS Hint -> Pick %s\n", formats[i].name);
				return &formats[i];
			}

			/* Calculate difference between format FPS and target */
			if (fps > target_fps)
				diff = fps - target_fps;
			else
				diff = target_fps - fps;

			if (sc0710_debug_mode)
				printk(KERN_INFO "sc0710: Cand %s FPS=%u Diff=%u (pass=%d)\n", formats[i].name, fps, diff, pass);

			if (diff < best_diff) {
				best_diff = diff;
				best_fmt = &formats[i];
			}

			/* Exact match optimization */
			if (diff == 0)
				return &formats[i];
		}

		/* If we found a match in this pass, return it */
		if (best_fmt)
			return best_fmt;
	}

	return NULL;
}





static int vidioc_s_dv_timings(struct file *file, void *_fh, struct v4l2_dv_timings *timings)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	dprintk(1, "%s()\n", __func__);

	return -EINVAL; /* No support for setting DV Timings */
}

static int vidioc_g_dv_timings(struct file *file, void *_fh, struct v4l2_dv_timings *timings)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	dprintk(0, "%s()\n", __func__);

	if (dev->fmt == NULL)
		return -EINVAL;

	/* Return the current detected timings. */
	*timings = dev->fmt->dv_timings;

	return 0;
}

static int vidioc_query_dv_timings(struct file *file, void *_fh, struct v4l2_dv_timings *timings)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	if (dev->fmt == NULL)
		return -ENODATA;

	*timings = dev->fmt->dv_timings;
	return 0;
}

/* Enum all possible timings we could support. */
static int vidioc_enum_dv_timings(struct file *file, void *_fh, struct v4l2_enum_dv_timings *timings)
{
	memset(timings->reserved, 0, sizeof(timings->reserved));

	if (timings->index >= ARRAY_SIZE(formats))
		return -EINVAL;

	timings->timings = formats[timings->index].dv_timings;

	return 0;
}

static int vidioc_dv_timings_cap(struct file *file, void *_fh, struct v4l2_dv_timings_cap *cap)
{
	cap->type = V4L2_DV_BT_656_1120;
	cap->bt.min_width = 720;
	cap->bt.max_width = 3840;
	cap->bt.min_height = 480;
	cap->bt.max_height = 2160;
	cap->bt.min_pixelclock = 27000000;
	cap->bt.max_pixelclock = 594000000;
	cap->bt.standards = V4L2_DV_BT_STD_CEA861;
	cap->bt.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE;
#if SUPPORT_INTERLACED
	cap->bt.capabilities |= V4L2_DV_BT_CAP_INTERLACED;
#endif

	return 0;
}

static int vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	strscpy(cap->driver, "sc0710", sizeof(cap->driver));
	strscpy(cap->card, sc0710_boards[dev->board].name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCIe:%s", pci_name(dev->pci));

	return 0;
}

static int vidioc_enum_input(struct file *file, void *priv, struct v4l2_input *i)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	dprintk(1, "%s()\n", __func__);

	if (i->index != 0)
		return -EINVAL;

	i->type  = V4L2_INPUT_TYPE_CAMERA;
	strscpy(i->name, "HDMI", sizeof(i->name));

	return 0;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	dprintk(1, "%s(%d)\n", __func__, i);

	if (i != 0)
		return -EINVAL;

	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	dprintk(1, "%s()\n", __func__);

	*i = 0;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= sc0710_pixfmts_count)
		return -EINVAL;
	f->pixelformat = sc0710_pixfmts[f->index].fourcc;
	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	const struct sc0710_format *fmt;
	u32 eff_w, eff_h, eff_fs;

	/* Use real format if available, otherwise use lastfmt, then default */
	fmt = dev->fmt ? dev->fmt : (dev->last_fmt ? dev->last_fmt : sc0710_get_default_format());

	sc0710_get_effective_size(dev, fmt, &eff_w, &eff_h, &eff_fs);

	f->fmt.pix.width = eff_w;
	f->fmt.pix.height = eff_h;
	f->fmt.pix.pixelformat = dev->pixfmt->fourcc;
	f->fmt.pix.field = fmt->interlaced ?
		V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = eff_w * sc0710_bpp(dev);
	f->fmt.pix.sizeimage = eff_fs;
	sc0710_fill_colorimetry(dev, dev->pixfmt, &f->fmt.pix);

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	const struct sc0710_format *fmt;
	const struct sc0710_pixfmt *pf;
	u32 eff_w, eff_h, eff_fs;

	/* Use real format if available, otherwise use lastfmt, then default */
	fmt = dev->fmt ? dev->fmt : (dev->last_fmt ? dev->last_fmt : sc0710_get_default_format());

	sc0710_get_effective_size(dev, fmt, &eff_w, &eff_h, &eff_fs);

	/* Unknown formats clamp to YUYV, as do formats the field weave can't
	 * produce for an interlaced source. Size for the requested format,
	 * not the currently active one. */
	pf = sc0710_pixfmt_find(f->fmt.pix.pixelformat);
	if (!pf || (fmt->interlaced && !pf->weave_ok))
		pf = &sc0710_pixfmts[0];

	f->fmt.pix.width = eff_w;
	f->fmt.pix.height = eff_h;
	f->fmt.pix.pixelformat = pf->fourcc;
	f->fmt.pix.field = fmt->interlaced ?
		V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = eff_w * pf->bpp;
	f->fmt.pix.sizeimage = eff_w * pf->bpp * eff_h;
	sc0710_fill_colorimetry(dev, pf, &f->fmt.pix);

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_client *client;
	unsigned long flags;
	bool busy = false;
	int ret = vidioc_try_fmt_vid_cap(file, priv, f);

	if (ret)
		return ret;

	/* Format is device-wide (one card, one DMA pipeline). Only change it while
	 * capture is stopped, so the DMA sizing and the 0xD0 format bit stay
	 * consistent for the whole session; a change request during capture is
	 * rejected unless it matches the active format. */
	if (ch->state == STATE_RUNNING)
		return f->fmt.pix.pixelformat == dev->pixfmt->fourcc ? 0 : -EBUSY;

	/* Same rule while any client holds buffers: they were negotiated and
	 * mapped at the current format's size. The buffer ioctls serialize on
	 * the node's ioctl lock, so the snapshot can't race an allocation. */
	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_for_each_entry(client, &ch->client_list, list) {
		if (vb2_is_busy(&client->vb2_queue)) {
			busy = true;
			break;
		}
	}
	spin_unlock_irqrestore(&ch->client_list_lock, flags);
	if (busy)
		return f->fmt.pix.pixelformat == dev->pixfmt->fourcc ? 0 : -EBUSY;

	dev->pixfmt = sc0710_pixfmt_find(f->fmt.pix.pixelformat);
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fsize)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	u32 eff_w, eff_h, eff_fs;

	if (!sc0710_pixfmt_find(fsize->pixel_format))
		return -EINVAL;

	/* Only support the currently detected resolution */
	if (fsize->index != 0)
		return -EINVAL;

	if (dev->fmt == NULL)
		return -EINVAL;

	sc0710_get_effective_size(dev, dev->fmt, &eff_w, &eff_h, &eff_fs);

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = eff_w;
	fsize->discrete.height = eff_h;

	return 0;
}

static int vidioc_enum_frameintervals(struct file *file, void *priv, struct v4l2_frmivalenum *fival)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	u32 eff_w, eff_h, eff_fs;

	if (!sc0710_pixfmt_find(fival->pixel_format))
		return -EINVAL;

	if (fival->index != 0)
		return -EINVAL;

	if (dev->fmt == NULL)
		return -EINVAL;

	sc0710_get_effective_size(dev, dev->fmt, &eff_w, &eff_h, &eff_fs);

	if (fival->width != eff_w || fival->height != eff_h)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = dev->fmt->fpsden;
	fival->discrete.denominator = dev->fmt->fpsnum;

	return 0;
}

static int vidioc_g_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&parm->parm.capture, 0, sizeof(parm->parm.capture));
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.readbuffers = 2;

	if (dev->fmt) {
		parm->parm.capture.timeperframe.numerator = dev->fmt->fpsden;
		parm->parm.capture.timeperframe.denominator = dev->fmt->fpsnum;
	} else {
		parm->parm.capture.timeperframe.numerator = 1;
		parm->parm.capture.timeperframe.denominator = 30;
	}

	return 0;
}

static int vidioc_s_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	/* We don't support changing frame rate, just return current */
	return vidioc_g_parm(file, priv, parm);
}

static int vidioc_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

/* ----------------------------------------------------------- */
/* VB2 buffer operations                                       */
/* ----------------------------------------------------------- */

static int sc0710_queue_setup(struct vb2_queue *q,
	unsigned int *num_buffers, unsigned int *num_planes,
	unsigned int sizes[], struct device *alloc_devs[])
{
	struct sc0710_client *client = vb2_get_drv_priv(q);
	struct sc0710_dma_channel *ch = client->fh->ch;
	struct sc0710_dev *dev = ch->dev;
	const struct sc0710_format *fmt;
	u32 eff_w, eff_h, eff_fs;

	/* Use real format if available, otherwise use lastfmt, then default */
	fmt = dev->fmt ? dev->fmt : (dev->last_fmt ? dev->last_fmt : sc0710_get_default_format());

	sc0710_get_effective_size(dev, fmt, &eff_w, &eff_h, &eff_fs);

	if (*num_buffers < 2)
		*num_buffers = 2;

	*num_planes = 1;

	/* Negotiated size only. A mid-stream resolution change routes
	 * through V4L2_EVENT_SOURCE_CHANGE and renegotiation, so buffers
	 * never need to hold more than the negotiated frame. */
	sizes[0] = eff_fs;
	dprintk(2, "%s() buffer count=%d, size=%d\n",
		__func__, *num_buffers, sizes[0]);

	return 0;
}

static int sc0710_buf_prepare(struct vb2_buffer *vb)
{
	struct sc0710_client *client = vb2_get_drv_priv(vb->vb2_queue);
	struct sc0710_dma_channel *ch = client->fh->ch;
	struct sc0710_dev *dev = ch->dev;
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sc0710_buffer *buf = container_of(vbuf, struct sc0710_buffer, vb);
	const struct sc0710_format *fmt;
	u32 eff_w, eff_h, eff_fs;

	/* Use real format if available, otherwise use lastfmt, then default */
	fmt = dev->fmt ? dev->fmt : (dev->last_fmt ? dev->last_fmt : sc0710_get_default_format());

	sc0710_get_effective_size(dev, fmt, &eff_w, &eff_h, &eff_fs);

	/* While streaming, delivery writes at most the locked stream size
	 * (mismatched sources are skipped and placeholders clamp to the
	 * buffer), so requeues validate against that; checking a newer,
	 * larger detected format would break the client's QBUF loop
	 * mid-stream. */
	if (client->streaming && client->stream_framesize)
		eff_fs = client->stream_framesize;

	if (vb2_plane_size(vb, 0) < eff_fs) {
		dprintk(0, "%s() buffer too small (%lu < %u)\n",
			__func__, vb2_plane_size(vb, 0), eff_fs);
		return -EINVAL;
	}

	if (zero_copy) {
		struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, 0);
		struct scatterlist *sg;
		unsigned int i;

		/* Snapshot the plane's DMA segments for descriptor targeting.
		 * A plane scattered across more segments than the frame chain
		 * has descriptors can't be DMA'd into; refuse it rather than
		 * silently degrade (strict mode). With an IOMMU the whole
		 * plane typically maps as one segment. */
		buf->zc_nsegs = 0;
		for_each_sgtable_dma_sg(sgt, sg, i) {
			if (i >= SC0710_MAX_CHAIN_DESCRIPTORS) {
				buf->zc_nsegs = 0;
				printk_ratelimited(KERN_WARNING
					"%s: zero-copy: buffer memory too fragmented for direct DMA (more than %u segments), refusing buffer\n",
					dev->name, SC0710_MAX_CHAIN_DESCRIPTORS);
				return -EINVAL;
			}
			buf->zc_seg[i].addr = sg_dma_address(sg);
			buf->zc_seg[i].len  = sg_dma_len(sg);
			buf->zc_nsegs = i + 1;
		}

		dprintk(2, "%s() zero-copy buffer: %u DMA segment(s)\n",
			__func__, buf->zc_nsegs);

		/* Map the kernel view now, in process context: the copy
		 * fallback and placeholder paths read the vaddr under
		 * spinlocks/timer context, where dma-sg's lazy first
		 * mapping must not happen. */
		if (!vb2_plane_vaddr(vb, 0))
			return -ENOMEM;
	}

	vb2_set_plane_payload(vb, 0, eff_fs);
	buf->expected_framesize = eff_fs;
	buf->fmt = fmt;

	return 0;
}

static void sc0710_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sc0710_client *client = vb2_get_drv_priv(vb->vb2_queue);
	struct sc0710_buffer *buf = container_of(vbuf, struct sc0710_buffer, vb);
	unsigned long flags;

	/* Add buffer to this client's buffer list */
	spin_lock_irqsave(&client->buffer_lock, flags);
	list_add_tail(&buf->list, &client->buffer_list);
	spin_unlock_irqrestore(&client->buffer_lock, flags);
}

/* Unwind a failed STREAMON: undo the streaming markers and hand every
 * queued buffer back to vb2 (the start_streaming failure contract). */
static void sc0710_start_streaming_unwind(struct sc0710_dma_channel *ch,
	struct sc0710_client *client)
{
	struct sc0710_buffer *buf, *tmp;
	unsigned long flags;

	client->streaming = false;
	atomic_dec(&ch->streaming_refcount);

	spin_lock_irqsave(&client->buffer_lock, flags);
	list_for_each_entry_safe(buf, tmp, &client->buffer_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_QUEUED);
	}
	spin_unlock_irqrestore(&client->buffer_lock, flags);
}

static int sc0710_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sc0710_client *client = vb2_get_drv_priv(q);
	struct sc0710_dma_channel *ch = client->fh->ch;
	struct sc0710_dev *dev = ch->dev;
	int refcount;
	int ret;

	dprintk(1, "%s(ch#%d)\n", __func__, ch->nr);

	/* Ensure status images are generated (safe process context here) */
	if (use_status_images)
		generate_status_frames_if_needed();

	/* Record the resolution this client expects for its entire stream
	 * lifetime.  Frames are delivered only while the source still
	 * matches it; after a source change the client is expected to
	 * restart streaming (V4L2_EVENT_SOURCE_CHANGE).
	 */
	{
		const struct sc0710_format *sfmt;
		u32 sw, sh, sfs;

		sfmt = dev->fmt ? dev->fmt :
			(dev->last_fmt ? dev->last_fmt : sc0710_get_default_format());
		sc0710_get_effective_size(dev, sfmt, &sw, &sh, &sfs);
		client->stream_width = sw;
		client->stream_height = sh;
		client->stream_framesize = sfs;
		dprintk(1, "%s() client locked to %ux%u (%u bytes)\n",
			__func__, sw, sh, sfs);
	}

	/* Mark this client as streaming */
	client->streaming = true;

	/* Increment streaming reference count */
	refcount = atomic_inc_return(&ch->streaming_refcount);
	dprintk(1, "%s() streaming refcount now %d\n", __func__, refcount);

	/* Zero-copy is strict single-client: frames DMA into one client's
	 * buffers, and there is no copy pass to fan out to a second. The
	 * refcount doubles as the race-free exclusivity check. */
	if (zero_copy && refcount > 1) {
		printk_ratelimited(KERN_WARNING
			"%s: zero-copy: a client is already streaming, refusing a second\n",
			dev->name);
		sc0710_start_streaming_unwind(ch, client);
		return -EBUSY;
	}

	/* Only start DMA if we're the first streaming client AND have signal.
	 * kthread_dma_lock serializes this against the HDMI thread's resync:
	 * without it a resync between the refcount increment and the resize
	 * could start the channel first, and the resize would then skip a
	 * running channel and stream from a stale ring. */
	if (refcount == 1 && dev->fmt != NULL) {
		mutex_lock(&dev->kthread_dma_lock);
		if (READ_ONCE(dev->disconnected)) {
			ret = -ENODEV;
		} else {
			ret = sc0710_dma_channels_resize(dev);
			if (ret == 0)
				ret = sc0710_dma_channels_start(dev);
		}
		mutex_unlock(&dev->kthread_dma_lock);
		if (ret < 0) {
			sc0710_start_streaming_unwind(ch, client);
			return ret;
		}
	} else if (dev->fmt == NULL) {
		dprintk(1, "%s() No signal - will deliver placeholder frames\n", __func__);
	}

	/* Start timer for delivering frames (real or placeholder) */
	mod_timer(&ch->timeout, jiffies + VBUF_TIMEOUT);

	return 0;
}

static void sc0710_stop_streaming(struct vb2_queue *q)
{
	struct sc0710_client *client = vb2_get_drv_priv(q);
	struct sc0710_dma_channel *ch = client->fh->ch;
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_buffer *buf, *tmp;
	unsigned long flags;
	int refcount;

	dprintk(1, "%s()\n", __func__);

	/* Mark this client as not streaming */
	client->streaming = false;

	/* Decrement streaming reference count */
	refcount = atomic_dec_return(&ch->streaming_refcount);
	dprintk(1, "%s() streaming refcount now %d\n", __func__, refcount);

	/* Only stop DMA if we're the last streaming client.
	 * kthread_dma_lock serializes against an in-flight resync, whose
	 * client snapshot would otherwise go stale here and restart DMA with
	 * no clients left.  Stop the channels first (serialized against the
	 * service thread via ch->lock), then delete the timer, so a service
	 * pass can't re-arm the timer after timer_delete_sync(). */
	if (refcount <= 0) {
		mutex_lock(&dev->kthread_dma_lock);
		atomic_set(&ch->streaming_refcount, 0); /* Clamp to 0 */
		/* After a disconnect the remove path owns the hardware (the
		 * engines are stopped, the BARs may be unmapped): only the
		 * software teardown below remains ours. */
		if (!READ_ONCE(dev->disconnected)) {
			sc0710_dma_channels_stop(dev);
			/* Point the chains back at the scratch ring: vb2 is
			 * about to unmap the client's buffers, and no
			 * descriptor may retain their DMA addresses (the stop
			 * above quiesced the engine). */
			if (zero_copy)
				sc0710_dma_channel_untarget_all(ch);
		}
		timer_delete_sync(&ch->timeout);
		mutex_unlock(&dev->kthread_dma_lock);
	}

	/* Release all active buffers for this client */
	spin_lock_irqsave(&client->buffer_lock, flags);
	list_for_each_entry_safe(buf, tmp, &client->buffer_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&client->buffer_lock, flags);
}

static const struct vb2_ops sc0710_video_qops = {
	.queue_setup     = sc0710_queue_setup,
	.buf_prepare     = sc0710_buf_prepare,
	.buf_queue       = sc0710_buf_queue,
	.start_streaming = sc0710_start_streaming,
	.stop_streaming  = sc0710_stop_streaming,
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
#endif
};

/* ----------------------------------------------------------- */
/* File operations                                             */
/* ----------------------------------------------------------- */

static int sc0710_video_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_fh *fh;
	struct vb2_queue *q;
	unsigned long flags;
	u32 users;
	int err;

	dprintk(0, "%s() dev=%s\n", __func__, video_device_node_name(vdev));

	fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	if (fh == NULL)
		return -ENOMEM;

	fh->ch   = ch;
	fh->fp   = file;
	fh->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	/* Initialize multi-client tracking with per-client VB2 queue */
	fh->client = kzalloc(sizeof(*fh->client), GFP_KERNEL);
	if (!fh->client) {
		kfree(fh);
		return -ENOMEM;
	}
	fh->client->fh = fh;
	fh->client->streaming = false;
	INIT_LIST_HEAD(&fh->client->buffer_list);
	spin_lock_init(&fh->client->buffer_lock);

	/* Initialize per-client VB2 queue */
	q = &fh->client->vb2_queue;
	memset(q, 0, sizeof(*q));
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = fh->client;  /* Point to client, not channel */
	q->buf_struct_size = sizeof(struct sc0710_buffer);
	q->ops = &sc0710_video_qops;
	/* Zero-copy needs DMA-mappable buffers the descriptors can target;
	 * GFP_DMA32 matches the device's 32-bit DMA mask so driver-allocated
	 * pages map without bounce buffering. */
	if (zero_copy) {
		q->mem_ops = &vb2_dma_sg_memops;
		q->gfp_flags = GFP_DMA32;
	} else {
		q->mem_ops = &vb2_vmalloc_memops;
	}
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	/* The node's ioctl lock: the v4l2 core holds it across every ioctl, so
	 * vb2's blocking waits (DQBUF, read) drop the mutex the caller actually
	 * holds, and other clients' ioctls proceed during the wait. */
	q->lock = &ch->v4l2_lock;
	q->dev = &dev->pci->dev;

	err = vb2_queue_init(q);
	if (err) {
		printk(KERN_ERR "%s: vb2_queue_init failed for client\n", dev->name);
		kfree(fh->client);
		kfree(fh);
		return err;
	}

	/* Add to the channel's client list; videousers rides the same lock
	 * (the v4l2 core does not hold vdev->lock for open/release). */
	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_add_tail(&fh->client->list, &ch->client_list);
	users = ++ch->videousers;
	spin_unlock_irqrestore(&ch->client_list_lock, flags);

	v4l2_fh_init(&fh->fh, vdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
	/* New API: v4l2_fh_add() sets file->private_data automatically */
	v4l2_fh_add(&fh->fh, file);
	/* But we use our own fh struct, so override private_data */
	file->private_data = fh;
#else
	v4l2_fh_add(&fh->fh);
	file->private_data = fh;
#endif

	dprintk(2, "%s() new client opened, videousers=%d\n", __func__, users);

	return 0;
}

static int sc0710_video_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct sc0710_fh *fh = file->private_data;
	struct sc0710_dma_channel *ch = fh->ch;
	struct sc0710_dev *dev = ch->dev;
	unsigned long flags;
	u32 users;

	dprintk(2, "%s() dev=%s\n", __func__, video_device_node_name(vdev));

	/* Release the per-client VB2 queue */
	if (fh->client) {
		/* Remove from the client list; videousers rides the same lock
		 * (the v4l2 core does not hold vdev->lock for open/release). */
		spin_lock_irqsave(&ch->client_list_lock, flags);
		list_del(&fh->client->list);
		users = --ch->videousers;
		spin_unlock_irqrestore(&ch->client_list_lock, flags);
		dprintk(2, "%s() videousers=%d\n", __func__, users);

		/* Release the queue (handles stopping streaming if needed).
		 * The core does not hold vdev->lock for release; take the queue
		 * lock ourselves like the vb2 fop helpers do. */
		mutex_lock(&ch->v4l2_lock);
		vb2_queue_release(&fh->client->vb2_queue);
		mutex_unlock(&ch->v4l2_lock);

		kfree(fh->client);
		fh->client = NULL;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
	v4l2_fh_del(&fh->fh, file);
#else
	v4l2_fh_del(&fh->fh);
#endif
	v4l2_fh_exit(&fh->fh);

	file->private_data = NULL;
	kfree(fh);

	return 0;
}

/* Custom VB2 wrappers that use per-client queue from file handle */
static ssize_t sc0710_fop_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct sc0710_fh *fh = file->private_data;
	ssize_t ret;

	if (!fh || !fh->client)
		return -EINVAL;
	/* The core does not hold vdev->lock for fops; read-fileio mutates
	 * queue state and its blocking wait drops the queue lock, so take it
	 * here the way vb2_fop_read does. */
	if (mutex_lock_interruptible(&fh->ch->v4l2_lock))
		return -ERESTARTSYS;
	ret = vb2_read(&fh->client->vb2_queue, buf, count, ppos,
		       file->f_flags & O_NONBLOCK);
	mutex_unlock(&fh->ch->v4l2_lock);
	return ret;
}

static __poll_t sc0710_fop_poll(struct file *file, poll_table *wait)
{
	struct sc0710_fh *fh = file->private_data;
	__poll_t rc;

	if (!fh || !fh->client)
		return EPOLLERR;

	/* vb2_poll can start read-fileio; serialize it like vb2_fop_poll. */
	if (mutex_lock_interruptible(&fh->ch->v4l2_lock))
		return EPOLLERR;
	rc = vb2_poll(&fh->client->vb2_queue, file, wait);
	mutex_unlock(&fh->ch->v4l2_lock);

	/* Also wake callers waiting for V4L2 events (SOURCE_CHANGE) */
	poll_wait(file, &fh->fh.wait, wait);
	if (v4l2_event_pending(&fh->fh))
		rc |= EPOLLPRI;

	return rc;
}

static int sc0710_fop_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_mmap(&fh->client->vb2_queue, vma);
}

/* Custom ioctl wrappers for buffer operations using per-client queue */
static int sc0710_vidioc_reqbufs(struct file *file, void *priv,
				 struct v4l2_requestbuffers *p)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_reqbufs(&fh->client->vb2_queue, p);
}

static int sc0710_vidioc_querybuf(struct file *file, void *priv,
				  struct v4l2_buffer *p)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_querybuf(&fh->client->vb2_queue, p);
}

static int sc0710_vidioc_expbuf(struct file *file, void *priv,
				struct v4l2_exportbuffer *p)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_expbuf(&fh->client->vb2_queue, p);
}

static int sc0710_vidioc_qbuf(struct file *file, void *priv,
			      struct v4l2_buffer *p)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_qbuf(&fh->client->vb2_queue, NULL, p);
}

static int sc0710_vidioc_dqbuf(struct file *file, void *priv,
			       struct v4l2_buffer *p)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_dqbuf(&fh->client->vb2_queue, p,
			 file->f_flags & O_NONBLOCK);
}

static int sc0710_vidioc_streamon(struct file *file, void *priv,
				  enum v4l2_buf_type type)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_streamon(&fh->client->vb2_queue, type);
}

static int sc0710_vidioc_streamoff(struct file *file, void *priv,
				   enum v4l2_buf_type type)
{
	struct sc0710_fh *fh = file->private_data;

	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_streamoff(&fh->client->vb2_queue, type);
}

static const struct v4l2_file_operations video_fops = {
	.owner	        = THIS_MODULE,
	.open           = sc0710_video_open,
	.release        = sc0710_video_release,
	.read           = sc0710_fop_read,
	.poll		    = sc0710_fop_poll,
	.mmap           = sc0710_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* VIDIOC_G/S_EDID: expose the card's source-facing EDID (the EEPROM image
 * the MCU presents to the HDMI source in Internal mode) through the standard
 * V4L2 ioctls, complementing the load-time edid= param. The v4l2 core copies
 * the edid->edid payload for us. The 4K Pro reads its EEPROM; the MK2 reads
 * the MCU-served internal image (see sc0710_mk2_read_edid in sc0710-i2c.c). */
static int vidioc_g_edid(struct file *file, void *_fh, struct v4l2_edid *edid)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	u8 buf[512];
	u32 total_blocks;
	int ret;

	if (edid->pad)
		return -EINVAL;

	/* The base block declares how much else there is; each 16-byte page is
	 * a full I2C transaction, so read only the blocks actually needed. */
	ret = sc0710_i2c_get_edid(dev, buf, 0, 128);
	if (ret < 0)
		return ret;

	/* Actual image length: base block plus its declared extensions,
	 * bounded by the 512-byte EEPROM. No header magic (blank or
	 * never-programmed EEPROM) = no EDID. */
	if (sc0710_edid_header_valid(buf))
		total_blocks = min_t(u32, (u32)buf[126] + 1, 4);
	else
		total_blocks = 0;

	if (edid->start_block == 0 && edid->blocks == 0) {
		edid->blocks = total_blocks;
		return 0;
	}
	if (total_blocks == 0)
		return -ENODATA;
	if (edid->start_block >= total_blocks)
		return -EINVAL;
	if (edid->blocks > total_blocks - edid->start_block)
		edid->blocks = total_blocks - edid->start_block;

	if (edid->start_block + edid->blocks > 1) {
		ret = sc0710_i2c_get_edid(dev, buf + 128, 128,
			(edid->start_block + edid->blocks - 1) * 128);
		if (ret < 0)
			return ret;
	}

	memcpy(edid->edid, buf + edid->start_block * 128, edid->blocks * 128);
	return 0;
}

static int vidioc_s_edid(struct file *file, void *_fh, struct v4l2_edid *edid)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	if (edid->pad)
		return -EINVAL;
	/* blocks == 0 ("erase the EDID") isn't supported: the MCU always
	 * presents something in Internal mode; set a real profile instead. */
	if (edid->blocks == 0)
		return -EINVAL;
	if (edid->blocks > 4) {
		edid->blocks = 4;
		return -E2BIG;
	}

	return sc0710_i2c_set_edid(dev, edid->edid, edid->blocks * 128);
}

static const struct v4l2_ioctl_ops video_ioctl_ops =
{
	.vidioc_querycap         = vidioc_querycap,

	.vidioc_s_dv_timings     = vidioc_s_dv_timings,
	.vidioc_g_dv_timings     = vidioc_g_dv_timings,
	.vidioc_g_edid           = vidioc_g_edid,
	.vidioc_s_edid           = vidioc_s_edid,
	.vidioc_query_dv_timings = vidioc_query_dv_timings,
	.vidioc_enum_dv_timings  = vidioc_enum_dv_timings,
	.vidioc_dv_timings_cap   = vidioc_dv_timings_cap,

	.vidioc_enum_input       = vidioc_enum_input,
	.vidioc_g_input          = vidioc_g_input,
	.vidioc_s_input          = vidioc_s_input,

	.vidioc_enum_fmt_vid_cap    = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap       = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap     = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap       = vidioc_s_fmt_vid_cap,
	.vidioc_enum_framesizes     = vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = vidioc_enum_frameintervals,
	.vidioc_g_parm              = vidioc_g_parm,
	.vidioc_s_parm              = vidioc_s_parm,

	.vidioc_reqbufs          = sc0710_vidioc_reqbufs,
	.vidioc_querybuf         = sc0710_vidioc_querybuf,
	.vidioc_expbuf           = sc0710_vidioc_expbuf,
	.vidioc_qbuf             = sc0710_vidioc_qbuf,
	.vidioc_dqbuf            = sc0710_vidioc_dqbuf,
	.vidioc_streamon         = sc0710_vidioc_streamon,
	.vidioc_streamoff        = sc0710_vidioc_streamoff,

	.vidioc_subscribe_event   = vidioc_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static struct video_device sc0710_video_template =
{
	.name      = "sc0710-video",
	.fops      = &video_fops,
	.ioctl_ops = &video_ioctl_ops,
};

static int sc0710_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc0710_dev *dev =
		container_of(ctrl->handler, struct sc0710_dev, ctrl_handler);

	switch (ctrl->id) {
	case SC0710_CID_EDID_SOURCE:
		return sc0710_set_edid_source(dev, ctrl->val);
	}
	return -EINVAL;
}

static const struct v4l2_ctrl_ops sc0710_ctrl_ops = {
	.s_ctrl = sc0710_s_ctrl,
};

static const char * const sc0710_edid_source_menu[] = {
	"Internal",
	"Display",
	"Merged",
	NULL
};

/* The MCU offers no readback for the EDID source, so the reported value is
 * the last one set through this control. Both boards: the MK2 driver
 * forces internal at probe, so the default is truthful there too.
 * EXECUTE_ON_WRITE lets a rewrite of the current value resend the call to
 * force a known state. */
static const struct v4l2_ctrl_config sc0710_edid_source_ctrl = {
	.ops   = &sc0710_ctrl_ops,
	.id    = SC0710_CID_EDID_SOURCE,
	.name  = "EDID Source",
	.type  = V4L2_CTRL_TYPE_MENU,
	.min   = SC0710_EDID_SOURCE_INTERNAL,
	.max   = SC0710_EDID_SOURCE_MERGED,
	.def   = SC0710_EDID_SOURCE_INTERNAL,
	.qmenu = sc0710_edid_source_menu,
	.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
};

static const struct v4l2_file_operations cobalt_empty_fops = {
        .owner = THIS_MODULE,
        .open = v4l2_fh_open,
        .unlocked_ioctl = video_ioctl2,
        .release = v4l2_fh_release,
};

static const struct v4l2_ioctl_ops cobalt_ioctl_empty_ops = {
#ifdef CONFIG_VIDEO_ADV_DEBUG
        .vidioc_g_register              = NULL,
        .vidioc_s_register              = NULL,
#endif
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
static void sc0710_vid_timeout(unsigned long data)
{
	struct sc0710_dma_channel *ch = (struct sc0710_dma_channel *)data;
#else
static void sc0710_vid_timeout(struct timer_list *t)
{
	struct sc0710_dma_channel *ch = container_of(t, struct sc0710_dma_channel, timeout);
#endif
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_client *client;
	const struct sc0710_format *fmt;
	const struct sc0710_format *live_fmt;
	unsigned long flags, buf_flags;
	int any_streaming = 0;
	int dma_active;
	u32 live_w = 0, live_h = 0;
	u32 eff_w, eff_h, eff_fs;

	/* Use lastfmt for placeholder frames to render at last known resolution */
	fmt = dev->last_fmt ? dev->last_fmt : sc0710_get_default_format();

	sc0710_get_effective_size(dev, fmt, &eff_w, &eff_h, &eff_fs);

	/* With a live signal and a running channel, DMA is delivering to
	 * every client whose locked resolution matches the detected format;
	 * those are skipped below, and placeholders go only to clients
	 * pending renegotiation after a source change.
	 * The state check keeps a stopped channel (failed DMA reconfigure with
	 * signal still locked) on the placeholder path.
	 * dev->fmt, dev->locked and ch->state are read unlocked; staleness
	 * costs one placeholder frame. The fmt pointer is snapshotted once:
	 * the HDMI thread NULLs it on signal loss, and a stale pointer stays
	 * valid (static tables, double-buffered dynamic slots) where a second
	 * read would not. */
	live_fmt = READ_ONCE(dev->fmt);
	dma_active = (live_fmt != NULL && dev->locked && ch->state == STATE_RUNNING);
	if (dma_active) {
		live_w = live_fmt->width;
		live_h = live_fmt->height;
	}

	dprintk(0, "%s(ch#%d) - delivering placeholder frames\n", __func__, ch->nr);

	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_for_each_entry(client, &ch->client_list, list) {
		struct sc0710_buffer *buf;
		u8 *dst;

		if (!client->streaming)
			continue;

		any_streaming = 1;

		if (dma_active &&
		    client->stream_width == live_w &&
		    client->stream_height == live_h)
			continue;

		spin_lock_irqsave(&client->buffer_lock, buf_flags);

		/* Deliver one placeholder frame per timeout */
		if (!list_empty(&client->buffer_list)) {
			buf = list_first_entry(&client->buffer_list, struct sc0710_buffer, list);

			dst = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
			if (!dst) {
				if (sc0710_debug_mode)
					printk_ratelimited(KERN_ERR "%s: vb2_plane_vaddr returned NULL\n", dev->name);
				spin_unlock_irqrestore(&client->buffer_lock, buf_flags);
				continue;
			}

			if (dst) {
				unsigned long buf_sz = vb2_plane_size(&buf->vb.vb2_buf, 0);
				u32 fill_w = eff_w, fill_h = eff_h, fill_fs = eff_fs;

				if (dev->pixfmt->rgb) {
					/* The pattern renderer and the dims
					 * fallbacks below are YUYV-only;
					 * RGB black is all zeros. */
					if (fill_fs > buf_sz)
						fill_fs = buf_sz;
					memset(dst, 0, fill_fs);
					vb2_set_plane_payload(&buf->vb.vb2_buf, 0, fill_fs);
				} else {
					int fillmode = dev->cable_connected ?
						FILL_MODE_NOSIGNAL : FILL_MODE_NODEVICE;

					/* Clamp to buffer capacity to prevent
					 * overflow when the detected format is
					 * larger than what the client allocated
					 * (e.g. 4K placeholder into 1080p buffer).
					 */
					if (fill_fs > buf_sz && fill_w && fill_h) {
						sc0710_guess_dims_from_framesize((u32)buf_sz,
									&fill_w, &fill_h);
						fill_fs = fill_w * 2 * fill_h;
					}
					if (fill_fs > buf_sz) {
						/* Last resort: the largest
						 * 1920-wide strip that fits.
						 * Never write a hardcoded
						 * geometry past the client's
						 * buffer. */
						fill_w = 1920;
						fill_h = min_t(u32, 1080, buf_sz / (1920 * 2));
						fill_fs = fill_w * 2 * fill_h;
					}

					fill_frame(ch, dst, fill_w, fill_h, fillmode);
					vb2_set_plane_payload(&buf->vb.vb2_buf, 0, fill_fs);
				}
			}

			buf->vb.vb2_buf.timestamp = ktime_get_ns();
			buf->vb.sequence = ch->frame_sequence;
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}

		spin_unlock_irqrestore(&client->buffer_lock, buf_flags);
	}
	ch->frame_sequence++;
	spin_unlock_irqrestore(&ch->client_list_lock, flags);

	/* Re-set the buffer timeout if any clients are still streaming */
	if (any_streaming)
		mod_timer(&ch->timeout, jiffies + VBUF_TIMEOUT);
}

void sc0710_video_notify_source_change(struct sc0710_dev *dev)
{
	struct sc0710_dma_channel *ch;
	struct v4l2_event ev = {};
	int ch_idx;

	ev.type = V4L2_EVENT_SOURCE_CHANGE;
	ev.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;

	for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
		ch = &dev->channel[ch_idx];
		if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
			continue;
		if (!video_is_registered(&ch->vdev))
			continue;

		v4l2_event_queue(&ch->vdev, &ev);
	}

	printk(KERN_INFO "%s: SOURCE_CHANGE event queued\n", dev->name);
}

void sc0710_video_unregister(struct sc0710_dma_channel *ch)
{
	struct sc0710_dev *dev = ch->dev;

	dprintk(1, "%s()\n", __func__);

	if (video_is_registered(&ch->vdev))
		video_unregister_device(&ch->vdev);

	/* The control handler stays alive: open file handles unsubscribe
	 * their control events through it at close. It is freed with dev,
	 * after the last handle is gone. */
}

/* Remove-path disconnect, after the node is unregistered and in-flight
 * ioctls drained: shut the frame timer down (later mod_timer calls are
 * silently ignored) and error the client queues so blocked DQBUF/poll/read
 * waiters wake with -EIO - nothing will complete their buffers again. New
 * clients can't appear (open is refused once the node is unregistered). */
void sc0710_video_disconnect(struct sc0710_dma_channel *ch)
{
	struct sc0710_client *client;
	unsigned long flags;

	timer_shutdown_sync(&ch->timeout);

	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_for_each_entry(client, &ch->client_list, list)
		vb2_queue_error(&client->vb2_queue);
	spin_unlock_irqrestore(&ch->client_list_lock, flags);
}

int sc0710_video_register(struct sc0710_dma_channel *ch)
{
	struct sc0710_dev *dev = ch->dev;
	int err;
	struct vb2_queue *q = &ch->vb2_queue;

	/* Initialize vb2 queue */
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = ch;
	q->buf_struct_size = sizeof(struct sc0710_buffer);
	q->ops = &sc0710_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock = &ch->v4l2_lock;
	q->dev = &dev->pci->dev;

	err = vb2_queue_init(q);
	if (err) {
		printk(KERN_ERR "%s: vb2_queue_init failed\n", dev->name);
		return err;
	}

	spin_lock_init(&ch->slock);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
	init_timer(&ch->timeout);
	ch->timeout.function = sc0710_vid_timeout;
	ch->timeout.data     = (unsigned long)ch;
#else
	timer_setup(&ch->timeout, sc0710_vid_timeout, 0);
#endif

	memcpy(&ch->vdev, &sc0710_video_template, sizeof(sc0710_video_template));
	ch->vdev.lock = &ch->v4l2_lock;
	ch->vdev.release = video_device_release_empty;
	ch->vdev.vfl_dir = VFL_DIR_RX;
	ch->vdev.queue = q;
	ch->vdev.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_READWRITE | V4L2_CAP_VIDEO_CAPTURE;
	ch->vdev.v4l2_dev = &dev->v4l2_dev;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
	ch->v4l_device->parent = &dev->pci->dev;
#else
	ch->vdev.dev_parent = &dev->pci->dev;
#endif
	strscpy(ch->vdev.name, "sc0710 video", sizeof(ch->vdev.name));

	/* The EDID source selector works on both supported boards: the MK2 MCU
	 * speaks the same fn-call protocol with the same ack signatures,
	 * verified on real hardware (see the MK2 notes in sc0710-i2c.c). */
	if (dev->board == SC0710_BOARD_ELGATEO_4KP ||
	    dev->board == SC0710_BOARD_ELGATEO_4KP60_MK2) {
		v4l2_ctrl_handler_init(&dev->ctrl_handler, 1);
		v4l2_ctrl_new_custom(&dev->ctrl_handler, &sc0710_edid_source_ctrl, NULL);
		if (dev->ctrl_handler.error) {
			err = dev->ctrl_handler.error;
			printk(KERN_ERR "%s: can't register the EDID source control\n", dev->name);
			v4l2_ctrl_handler_free(&dev->ctrl_handler);
			return err;
		}
		ch->vdev.ctrl_handler = &dev->ctrl_handler;
	}
	/* EDID: the 4K Pro reads/writes its EEPROM directly; the MK2 reads the
	 * MCU-served internal image (ReadEDID protocol, all on the safe 0x33
	 * port) and gets S_EDID too. Both supported boards get G_EDID and
	 * S_EDID; other boards get neither. */
	if (dev->board != SC0710_BOARD_ELGATEO_4KP &&
	    dev->board != SC0710_BOARD_ELGATEO_4KP60_MK2) {
		v4l2_disable_ioctl(&ch->vdev, VIDIOC_G_EDID);
		v4l2_disable_ioctl(&ch->vdev, VIDIOC_S_EDID);
	}

	video_set_drvdata(&ch->vdev, ch);

	err = video_register_device(&ch->vdev,
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
		VFL_TYPE_GRABBER,
#else
		VFL_TYPE_VIDEO,
#endif
		-1);
	if (err < 0) {
		printk(KERN_INFO "%s: can't register video device\n", dev->name);
		return -EIO;
	}

	if (sc0710_debug_mode)
		printk(KERN_INFO "%s: registered device %s [v4l2]\n",
	       dev->name, video_device_node_name(&ch->vdev));

	return 0; /* Success */
}
