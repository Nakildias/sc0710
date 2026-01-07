/*
 *  Driver for the Elgato 4k60 Pro mk.2 HDMI capture card.
 *
 *  Copyright (c) 2021-2022 Steven Toth <stoth@kernellabs.com>
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

#include "sc0710.h"

static int video_debug = 1;

/* Module parameter to override EOTF detection
 * 0 = auto-detect (default), 1 = force SDR, 2 = force HDR/PQ, 3 = force HLG
 */
static int force_eotf = 0;
module_param(force_eotf, int, 0644);
MODULE_PARM_DESC(force_eotf, "Force EOTF: 0=auto, 1=SDR, 2=HDR-PQ, 3=HLG");

/* Module parameter to override quantization range
 * 0 = auto (default), 1 = force limited range (16-235), 2 = force full range (0-255)
 */
static int force_quantization = 0;
module_param(force_quantization, int, 0644);
MODULE_PARM_DESC(force_quantization, "Force quantization: 0=auto, 1=limited, 2=full");

#define dprintk(level, fmt, arg...)\
        do { if (video_debug >= level)\
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
	switch (dev->colorimetry) {
	case BT_601:  return V4L2_COLORSPACE_SMPTE170M;
	case BT_709:  return V4L2_COLORSPACE_REC709;
	case BT_2020: return V4L2_COLORSPACE_BT2020;
	default:      return V4L2_COLORSPACE_SRGB;
	}
}

/* Map detected colorimetry to V4L2 transfer function.
 * BT.2020 can be SDR (gamma ~2.4), HDR10 (PQ/SMPTE 2084), or HLG.
 * Use detected EOTF from InfoFrame, or allow manual override via force_eotf.
 */
static enum v4l2_xfer_func sc0710_get_v4l2_xfer_func(struct sc0710_dev *dev)
{
	/* Allow manual override via module parameter */
	switch (force_eotf) {
	case 1: return V4L2_XFER_FUNC_DEFAULT;     /* Force SDR */
	case 2: return V4L2_XFER_FUNC_SMPTE2084;   /* Force HDR-PQ */
	case 3: return V4L2_XFER_FUNC_SMPTE2084;   /* HLG (V4L2 lacks specific HLG) */
	}

	/* Auto-detection based on detected EOTF from HDMI InfoFrame */
	switch (dev->eotf) {
	case EOTF_HDR_PQ:
		return V4L2_XFER_FUNC_SMPTE2084;
	case EOTF_HDR_HLG:
		return V4L2_XFER_FUNC_SMPTE2084;  /* Closest V4L2 approximation */
	case EOTF_SDR:
	case EOTF_UNKNOWN:
	default:
		/* SDR: use default gamma (~2.2/2.4) */
		return V4L2_XFER_FUNC_DEFAULT;
	}
}

/* Map detected colorimetry to V4L2 Y'CbCr encoding */
static enum v4l2_ycbcr_encoding sc0710_get_v4l2_ycbcr_enc(struct sc0710_dev *dev)
{
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

	/* Auto: BT.2020 typically uses limited range, sRGB uses full */
	if (dev->colorimetry == BT_2020)
		return V4L2_QUANTIZATION_LIM_RANGE;
	return V4L2_QUANTIZATION_DEFAULT;
}

#define FILL_MODE_COLORBARS 0
#define FILL_MODE_GREENSCREEN 1
#define FILL_MODE_BLUESCREEN 2
#define FILL_MODE_BLACKSCREEN 3
#define FILL_MODE_REDSCREEN 4

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

static void fill_frame(struct sc0710_dma_channel *ch,
	unsigned char *dest_frame, unsigned int width,
	unsigned int height, unsigned int fillmode)
{
	unsigned int width_bytes = width * 2;
	unsigned int i, divider;

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

#define SUPPORT_INTERLACED 0
static struct sc0710_format formats[] =
{
#if SUPPORT_INTERLACED
	{  858,  262,  720,  240, 1, 2997, 30000, 1001, 8, 0, "720x480i29.97",   V4L2_DV_BT_CEA_720X480I59_94 },
#endif
	{  858,  525,  720,  480, 0, 5994, 60000, 1001, 8, 0, "720x480p59.94",   V4L2_DV_BT_CEA_720X480P59_94 },

#if SUPPORT_INTERLACED
	{  864,  312,  720,  288, 1, 2500, 25000, 1000, 8, 0, "720x576i25",      V4L2_DV_BT_CEA_720X576I50 },
#endif

	{ 1980,  750, 1280,  720, 0, 5000, 50000, 1000, 8, 0, "1280x720p50",     V4L2_DV_BT_CEA_1280X720P50 },
	{ 1650,  750, 1280,  720, 0, 5994, 60000, 1001, 8, 0, "1280x720p59.94",  V4L2_DV_BT_CEA_1280X720P60 },
	{ 1650,  750, 1280,  720, 0, 6000, 60000, 1000, 8, 0, "1280x720p60",     V4L2_DV_BT_CEA_1280X720P60 },

#if SUPPORT_INTERLACED
	{ 2640,  562, 1920,  540, 1, 2500, 25000, 1000, 8, 0, "1920x1080i25",    V4L2_DV_BT_CEA_1920X1080I50 },
	{ 2200,  562, 1920,  540, 1, 2997, 30000, 1001, 8, 0, "1920x1080i29.97", V4L2_DV_BT_CEA_1920X1080I60 },
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

	/* 1440p formats - 2560x1440 */
	/* Multiple timing variants detected from different sources */
	{ 2720, 1481, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120a",  V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2720, 1524, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120b",  V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2720, 1525, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120c",  V4L2_DV_BT_CEA_1920X1080P60 },
	/* CVT and alternate timings */
	{ 2720, 1510, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120alt", V4L2_DV_BT_CEA_1920X1080P60 },
	{ 2640, 1490, 2560, 1440, 0, 12000, 120000, 1000, 8, 0, "2560x1440p120cvt", V4L2_DV_BT_CEA_1920X1080P60 },
	/* 144Hz variants */
	{ 2720, 1527, 2560, 1440, 0, 14400, 144000, 1000, 8, 0, "2560x1440p144",   V4L2_DV_BT_CEA_1920X1080P60 },

	{ 4400, 2250, 3840, 2160, 0, 5994, 60000, 1001, 8, 0, "3840x2160p59.94", V4L2_DV_BT_CEA_3840X2160P60 },
	{ 4400, 2250, 3840, 2160, 0, 6000, 60000, 1000, 8, 0, "3840x2160p60",    V4L2_DV_BT_CEA_3840X2160P60 },
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
		fmt = &formats[i];

		/* Assuming YUV 8-bit */
		fmt->framesize = fmt->width * 2 * fmt->height;
	}
}

const struct sc0710_format *sc0710_format_find_by_timing(u32 timingH, u32 timingV)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if ((formats[i].timingH == timingH) && (formats[i].timingV == timingV)) {
			return &formats[i];
		}
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
	if (f->index != 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	const struct sc0710_format *fmt;

	/* Use real format if available, otherwise use default */
	fmt = dev->fmt ? dev->fmt : sc0710_get_default_format();

	f->fmt.pix.width = fmt->width;
	f->fmt.pix.height = fmt->height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = fmt->width * 2;
	f->fmt.pix.sizeimage = fmt->framesize;
	f->fmt.pix.colorspace = sc0710_get_v4l2_colorspace(dev);
	f->fmt.pix.xfer_func = sc0710_get_v4l2_xfer_func(dev);
	f->fmt.pix.ycbcr_enc = sc0710_get_v4l2_ycbcr_enc(dev);
	f->fmt.pix.quantization = sc0710_get_v4l2_quantization(dev);

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;
	const struct sc0710_format *fmt;

	/* Use real format if available, otherwise use default */
	fmt = dev->fmt ? dev->fmt : sc0710_get_default_format();

	f->fmt.pix.width = fmt->width;
	f->fmt.pix.height = fmt->height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = fmt->width * 2;
	f->fmt.pix.sizeimage = fmt->framesize;
	f->fmt.pix.colorspace = sc0710_get_v4l2_colorspace(dev);
	f->fmt.pix.xfer_func = sc0710_get_v4l2_xfer_func(dev);
	f->fmt.pix.ycbcr_enc = sc0710_get_v4l2_ycbcr_enc(dev);
	f->fmt.pix.quantization = sc0710_get_v4l2_quantization(dev);

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	return vidioc_try_fmt_vid_cap(file, priv, f);
}

static int vidioc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fsize)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	if (fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	/* Only support the currently detected resolution */
	if (fsize->index != 0)
		return -EINVAL;

	if (dev->fmt == NULL)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = dev->fmt->width;
	fsize->discrete.height = dev->fmt->height;

	return 0;
}

static int vidioc_enum_frameintervals(struct file *file, void *priv, struct v4l2_frmivalenum *fival)
{
	struct sc0710_dma_channel *ch = video_drvdata(file);
	struct sc0710_dev *dev = ch->dev;

	if (fival->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	if (fival->index != 0)
		return -EINVAL;

	if (dev->fmt == NULL)
		return -EINVAL;

	if (fival->width != dev->fmt->width || fival->height != dev->fmt->height)
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

	/* Use real format if available, otherwise use default */
	fmt = dev->fmt ? dev->fmt : sc0710_get_default_format();

	if (*num_buffers < 2)
		*num_buffers = 2;

	*num_planes = 1;
	sizes[0] = fmt->framesize;

	dprintk(2, "%s() buffer count=%d, size=%d\n", __func__, *num_buffers, sizes[0]);

	return 0;
}

static int sc0710_buf_prepare(struct vb2_buffer *vb)
{
	struct sc0710_client *client = vb2_get_drv_priv(vb->vb2_queue);
	struct sc0710_dma_channel *ch = client->fh->ch;
	struct sc0710_dev *dev = ch->dev;
	const struct sc0710_format *fmt;

	/* Use real format if available, otherwise use default */
	fmt = dev->fmt ? dev->fmt : sc0710_get_default_format();

	if (vb2_plane_size(vb, 0) < fmt->framesize) {
		dprintk(0, "%s() buffer too small (%lu < %u)\n",
			__func__, vb2_plane_size(vb, 0), fmt->framesize);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, fmt->framesize);

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

static int sc0710_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sc0710_client *client = vb2_get_drv_priv(q);
	struct sc0710_dma_channel *ch = client->fh->ch;
	struct sc0710_dev *dev = ch->dev;
	int refcount;
	int ret;

	dprintk(1, "%s(ch#%d)\\n", __func__, ch->nr);

	/* Mark this client as streaming */
	client->streaming = true;

	/* Increment streaming reference count */
	refcount = atomic_inc_return(&ch->streaming_refcount);
	dprintk(1, "%s() streaming refcount now %d\\n", __func__, refcount);

	/* Only start DMA if we're the first streaming client AND have signal */
	if (refcount == 1 && dev->fmt != NULL) {
		sc0710_dma_channels_resize(dev);

		ret = sc0710_dma_channels_start(dev);
		if (ret < 0) {
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
			return ret;
		}
	} else if (dev->fmt == NULL) {
		dprintk(1, "%s() No signal - will deliver placeholder frames\\n", __func__);
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

	/* Only stop DMA if we're the last streaming client */
	if (refcount <= 0) {
		atomic_set(&ch->streaming_refcount, 0); /* Clamp to 0 */
		timer_delete_sync(&ch->timeout);
		sc0710_dma_channels_stop(dev);
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
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
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
	mutex_init(&fh->client->vb2_lock);

	/* Initialize per-client VB2 queue */
	q = &fh->client->vb2_queue;
	memset(q, 0, sizeof(*q));
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = fh->client;  /* Point to client, not channel */
	q->buf_struct_size = sizeof(struct sc0710_buffer);
	q->ops = &sc0710_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock = &fh->client->vb2_lock;  /* Use client's lock */
	q->dev = &dev->pci->dev;

	err = vb2_queue_init(q);
	if (err) {
		printk(KERN_ERR "%s: vb2_queue_init failed for client\n", dev->name);
		kfree(fh->client);
		kfree(fh);
		return err;
	}

	/* Add to channel's client list */
	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_add_tail(&fh->client->list, &ch->client_list);
	spin_unlock_irqrestore(&ch->client_list_lock, flags);

	/* Track video users */
	mutex_lock(&ch->lock);
	ch->videousers++;
	mutex_unlock(&ch->lock);

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

	dprintk(0, "%s() new client opened, videousers=%d\n", __func__, ch->videousers);

	return 0;
}

static int sc0710_video_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct sc0710_fh *fh = file->private_data;
	struct sc0710_dma_channel *ch = fh->ch;
	struct sc0710_dev *dev = ch->dev;
	unsigned long flags;

	dprintk(1, "%s() dev=%s\n", __func__, video_device_node_name(vdev));

	/* Release the per-client VB2 queue */
	if (fh->client) {
		/* Stop streaming if this client was streaming */
		if (fh->client->streaming) {
			vb2_queue_release(&fh->client->vb2_queue);
		}

		/* Remove from client list */
		spin_lock_irqsave(&ch->client_list_lock, flags);
		list_del(&fh->client->list);
		spin_unlock_irqrestore(&ch->client_list_lock, flags);

		/* Release the queue */
		vb2_queue_release(&fh->client->vb2_queue);

		kfree(fh->client);
		fh->client = NULL;
	}

	mutex_lock(&ch->lock);
	ch->videousers--;
	dprintk(0, "%s() videousers=%d\n", __func__, ch->videousers);
	mutex_unlock(&ch->lock);

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
	if (!fh || !fh->client)
		return -EINVAL;
	return vb2_read(&fh->client->vb2_queue, buf, count, ppos,
			file->f_flags & O_NONBLOCK);
}

static __poll_t sc0710_fop_poll(struct file *file, poll_table *wait)
{
	struct sc0710_fh *fh = file->private_data;
	if (!fh || !fh->client)
		return EPOLLERR;
	return vb2_poll(&fh->client->vb2_queue, file, wait);
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

static const struct v4l2_ioctl_ops video_ioctl_ops =
{
	.vidioc_querycap         = vidioc_querycap,

	.vidioc_s_dv_timings     = vidioc_s_dv_timings,
	.vidioc_g_dv_timings     = vidioc_g_dv_timings,
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
	.vidioc_qbuf             = sc0710_vidioc_qbuf,
	.vidioc_dqbuf            = sc0710_vidioc_dqbuf,
	.vidioc_streamon         = sc0710_vidioc_streamon,
	.vidioc_streamoff        = sc0710_vidioc_streamoff,
};

static struct video_device sc0710_video_template =
{
	.name      = "sc0710-video",
	.fops      = &video_fops,
	.ioctl_ops = &video_ioctl_ops,
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
	unsigned long flags, buf_flags;
	int any_streaming = 0;

	/* Use last known format for placeholder, or default if never had signal.
	 * This ensures colorbars fill the entire buffer (e.g., 4K buffers when
	 * returning from 4K signal).
	 */
	fmt = dev->last_fmt ? dev->last_fmt : sc0710_get_default_format();

	/* If we have real signal, DMA is handling frame delivery, just reschedule */
	if (dev->fmt != NULL && dev->locked) {
		/* Re-set the buffer timeout for DMA monitoring */
		if (atomic_read(&ch->streaming_refcount) > 0)
			mod_timer(&ch->timeout, jiffies + VBUF_TIMEOUT);
		return;
	}

	/* No signal - deliver placeholder frames to all streaming clients */
	dprintk(0, "%s(ch#%d) - delivering placeholder frames\n", __func__, ch->nr);

	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_for_each_entry(client, &ch->client_list, list) {
		struct sc0710_buffer *buf;
		u8 *dst;

		if (!client->streaming)
			continue;

		any_streaming = 1;

		spin_lock_irqsave(&client->buffer_lock, buf_flags);

		/* Deliver one placeholder frame per timeout */
		if (!list_empty(&client->buffer_list)) {
			buf = list_first_entry(&client->buffer_list, struct sc0710_buffer, list);

		dst = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
			if (dst) {
				fill_frame(ch, dst, fmt->width, fmt->height, FILL_MODE_COLORBARS);
				vb2_set_plane_payload(&buf->vb.vb2_buf, 0, fmt->framesize);
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

void sc0710_video_unregister(struct sc0710_dma_channel *ch)
{
	struct sc0710_dev *dev = ch->dev;

	dprintk(1, "%s()\n", __func__);

	if (video_is_registered(&ch->vdev))
		video_unregister_device(&ch->vdev);
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
	q->lock = &ch->lock;
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
	ch->vdev.lock = &ch->lock;
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
		return -1;
	}

	printk(KERN_INFO "%s: registered device %s [v4l2]\n",
	       dev->name, video_device_node_name(&ch->vdev));

	return 0; /* Success */
}

