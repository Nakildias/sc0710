// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for the Elgato Cam Link Pro (12ab:0710, subsystem 1cfa:0011):
 *  four HDMI inputs behind one SC0710 / Xilinx XDMA bridge.
 *
 *  Everything here was taken from VFIO traces of the vendor Windows driver:
 *
 *  - The card streams all four inputs at all times through XDMA C2H channel
 *    0, as fixed 3856-byte lines in arrival order:
 *        ff x11, 00 | u16le (line << 3 | port) | flags[2] | 3840 B pixels
 *    flags 00 4f = NV12 luma line (1..H), 00 6f = NV12 chroma line (odd
 *    line numbers 1..H-1), 80 07 = packed YUY2 line (1..H). The vendor
 *    driver sends 4K as NV12 and 1080p as YUY2.
 *  - C2H channel 1 carries audio for all four inputs: 16-byte frames of
 *    4 x (L, R) S16_LE, 480 frames (10 ms at 48 kHz) per 7680-byte block.
 *  - Each input's timing is an MCU status block at I2C 0x32, sub-address
 *    0x04 + 0x3c * port: vtotal, htotal, vactive, hactive (u16le), fps.
 *  - Nothing interrupts: the vendor driver polls the completed-descriptor
 *    count, and so does this file.
 *
 *  One V4L2 capture node and one ALSA capture PCM per input; video is
 *  demultiplexed into vmalloc vb2 buffers by the line headers, audio by
 *  frame slot.
 */

#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <media/v4l2-dv-timings.h>
#include "sc0710.h"

#define FP_PORTS        4
#define FP_LINE         3856
#define FP_HDR          16
#define FP_PAYLOAD      (FP_LINE - FP_HDR)
/* Whole lines per descriptor, so an aligned stream never straddles. */
#define FP_CHUNK        (FP_LINE * 256)
#define FP_AUDIO_DESCS  8
#define FP_AUDIO_CHUNK  7680
#define FP_AUDIO_STRIDE 8192

/* Header bytes 14-15, big-endian */
#define FP_FLAGS_Y      0x004f
#define FP_FLAGS_UV     0x006f
#define FP_FLAGS_YUY2   0x8007

/* XDMA (BAR1) */
#define FP_C2H(ch)          (0x1000 + (ch) * 0x100)
#define FP_C2H_CTRL_W1S(ch) (FP_C2H(ch) + 0x08)
#define FP_C2H_CTRL_W1C(ch) (FP_C2H(ch) + 0x0c)
#define FP_C2H_DONE(ch)     (FP_C2H(ch) + 0x48)
#define FP_C2H_SG(ch)       (0x5000 + (ch) * 0x100)
#define FP_H2C_SG(ch)       (0x4000 + (ch) * 0x100)
#define FP_SG_LO            0x80
#define FP_SG_HI            0x84
#define FP_SG_ADJ           0x88

/* XDMA descriptor: magic 0xad4b, "completed" flag as the vendor sets it */
#define FP_DESC_CTRL        0xad4b0002

static unsigned int fourport_ring_chunks = 64;
module_param(fourport_ring_chunks, uint, 0444);
MODULE_PARM_DESC(fourport_ring_chunks,
	"Cam Link Pro: video ring size in ~1 MiB descriptors (default 64, ~45 ms at full load)");

static unsigned int fourport_debug;
module_param(fourport_debug, uint, 0644);
MODULE_PARM_DESC(fourport_debug, "Cam Link Pro: log stream resyncs with context (1 = on)");

/* Grace period before an idle session is stopped, so probes that open and
 * close the nodes (udev, PipeWire) don't restart the hardware each time. */
#define FP_IDLE_STOP_MS 2000

static unsigned int fourport_status_ms = 250;
module_param(fourport_status_ms, uint, 0644);
MODULE_PARM_DESC(fourport_status_ms, "Cam Link Pro: input status poll interval in ms");

enum fp_kind { FP_KIND_NONE, FP_KIND_Y, FP_KIND_UV, FP_KIND_YUY2 };

struct fp_buffer {
	struct vb2_v4l2_buffer vb;	/* must be first */
	struct list_head       list;
};

struct fp_status {
	u16 vtotal, htotal, vactive, hactive;
	u8  fps;
};

struct sc0710_4port;

struct fp_port {
	struct sc0710_4port *fp;
	int                  idx;

	struct video_device  vdev;
	struct vb2_queue     q;
	struct mutex         lock;		/* vdev/vb2 ioctl serialization */

	/* Format, changed only while the queue is idle (under lock) */
	u32 pixfmt, width, height, bpl, sizeimage, fps;

	/* Last MCU status and the line type the stream last carried */
	struct fp_status st;
	bool             locked;
	enum fp_kind     seen_kind;		/* written by the DMA thread */

	/* Buffers queued by userspace; cur is owned by the DMA thread */
	spinlock_t        qlock;
	struct list_head  bufs;
	struct fp_buffer *cur;
	u32               cur_lines;
	u32               sequence;
	bool              streaming;
	u64               frames, dropped;

	/* ALSA capture: asub is the running substream (NULL when stopped),
	 * apos/aperiod its buffer and period positions, all under alock. */
	spinlock_t                alock;
	struct snd_pcm_substream *asub;
	snd_pcm_uframes_t         apos, aperiod;
};

struct sc0710_4port {
	struct sc0710_dev *dev;
	struct fp_port     port[FP_PORTS];

	/* Video ring: n chunks, one descriptor each, linked in a circle */
	u32                         nchunks;
	struct sc0710_dma_descriptor *desc;
	dma_addr_t                  desc_dma;
	u8                         *chunk_cpu[256];
	dma_addr_t                  chunk_dma[256];
	/* Audio ring (discarded) */
	struct sc0710_dma_descriptor *adesc;
	dma_addr_t                  adesc_dma;
	u8                         *abuf;
	dma_addr_t                  abuf_dma;
	u32                         adone_seen;
	u64                         ablocks, aoverruns;
	unsigned long               session_start;
	struct snd_card            *card;

	/* Hardware session, running while any node is open */
	struct mutex  hw_lock;
	struct delayed_work stop_work;
	u32           users;
	bool          running;
	bool          disconnected;

	/* DMA service: dma_lock is held while lines are consumed, so
	 * stop paths can wait out an in-flight pass. */
	struct task_struct *dma_thread;
	struct task_struct *status_thread;
	struct mutex        dma_lock;
	wait_queue_head_t   wq;
	u32                 done_seen;	/* completed-descriptor count consumed */
	u8                  carry[FP_LINE];
	u32                 carry_len;
	bool                synced;
	bool                locked_once;	/* first sync of the session seen */
	u64                 overruns, resyncs;
	/* Last line accepted, for resync diagnostics */
	u8                  last_hdr[FP_HDR];
	u32                 chunk_idx;
};

static const u8 fp_sync[12] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

/* ----------------------------------------------------------- */
/* Format bookkeeping                                           */

static void fp_port_set_format(struct fp_port *p, u32 pixfmt, u32 width, u32 height, u32 fps)
{
	p->pixfmt = pixfmt;
	p->width = width;
	p->height = height;
	p->fps = fps ? fps : 60;
	if (pixfmt == V4L2_PIX_FMT_NV12) {
		p->bpl = width;
		p->sizeimage = width * height * 3 / 2;
	} else {
		p->bpl = width * 2;
		p->sizeimage = p->bpl * height;
	}
}

/* What the card will send for this input: whatever its lines last were,
 * else the vendor's observed split (NV12 above 1080p, YUY2 otherwise). */
static u32 fp_port_pick_pixfmt(struct fp_port *p, u32 width, u32 height)
{
	switch (READ_ONCE(p->seen_kind)) {
	case FP_KIND_Y:
	case FP_KIND_UV:
		return V4L2_PIX_FMT_NV12;
	case FP_KIND_YUY2:
		return V4L2_PIX_FMT_YUYV;
	default:
		return width * height > 1920 * 1080 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_YUYV;
	}
}

/* Bring the advertised format in line with the input. Idle queues are
 * updated in place; a busy one keeps streaming its negotiated format and
 * gets a source-change event on any timing change, rate included, so the
 * client can renegotiate. Called with p->lock held. */
static void fp_port_update_format(struct fp_port *p)
{
	u32 width, height, pixfmt;
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	if (!p->locked)
		return;

	width = p->st.hactive;
	height = p->st.vactive;
	pixfmt = fp_port_pick_pixfmt(p, width, height);
	/* A line carries 3840 bytes: 3840 NV12 or 1920 YUY2 pixels at most */
	if (width > (pixfmt == V4L2_PIX_FMT_NV12 ? FP_PAYLOAD : FP_PAYLOAD / 2) || !height)
		return;

	if (width != p->width || height != p->height || pixfmt != p->pixfmt ||
	    p->st.fps != p->fps) {
		if (!vb2_is_busy(&p->q))
			fp_port_set_format(p, pixfmt, width, height, p->st.fps);
		else if (p->st.fps)
			v4l2_event_queue(&p->vdev, &ev);
	}
}

/* ----------------------------------------------------------- */
/* Demultiplexer (DMA thread context)                           */

static void fp_buffer_finish(struct fp_port *p, enum vb2_buffer_state state)
{
	struct fp_buffer *buf = p->cur;

	p->cur = NULL;
	if (!buf)
		return;
	if (state == VB2_BUF_STATE_DONE) {
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, p->sizeimage);
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.field = V4L2_FIELD_NONE;
		buf->vb.sequence = p->sequence++;
		p->frames++;
	} else {
		p->dropped++;
	}
	vb2_buffer_done(&buf->vb.vb2_buf, state);
}

static void fp_buffer_next(struct fp_port *p)
{
	unsigned long flags;

	spin_lock_irqsave(&p->qlock, flags);
	p->cur = list_first_entry_or_null(&p->bufs, struct fp_buffer, list);
	if (p->cur)
		list_del(&p->cur->list);
	spin_unlock_irqrestore(&p->qlock, flags);
	p->cur_lines = 0;
	if (!p->cur)
		p->dropped++;
}

static void fp_line(struct sc0710_4port *fp, const u8 *hdr, const u8 *pix)
{
	u16 word = hdr[12] | hdr[13] << 8;
	u16 flags = hdr[14] << 8 | hdr[15];
	struct fp_port *p;
	enum fp_kind kind;
	u32 line = word >> 3, off;
	u8 *dst;

	if ((word & 7) >= FP_PORTS || !line)
		return;
	p = &fp->port[word & 7];

	switch (flags) {
	case FP_FLAGS_Y:    kind = FP_KIND_Y;    break;
	case FP_FLAGS_UV:   kind = FP_KIND_UV;   break;
	case FP_FLAGS_YUY2: kind = FP_KIND_YUY2; break;
	default:
		return;
	}
	WRITE_ONCE(p->seen_kind, kind);

	if (!READ_ONCE(p->streaming))
		return;
	/* Lines of a type the queue wasn't set up for (the input changed
	 * format mid-stream) are dropped; the source-change event covers it. */
	if ((kind == FP_KIND_YUY2) != (p->pixfmt == V4L2_PIX_FMT_YUYV))
		return;

	/* Line 1 of the luma/packed plane starts a frame */
	if (line == 1 && kind != FP_KIND_UV) {
		if (p->cur)
			fp_buffer_finish(p, VB2_BUF_STATE_ERROR);
		fp_buffer_next(p);
	}
	if (!p->cur)
		return;

	switch (kind) {
	case FP_KIND_UV:
		if (line >= p->height)
			return;
		off = p->height * p->bpl + (line - 1) / 2 * p->bpl;
		break;
	default:
		if (line > p->height)
			return;
		off = (line - 1) * p->bpl;
		break;
	}
	if (off + p->bpl > p->sizeimage)
		return;

	dst = vb2_plane_vaddr(&p->cur->vb.vb2_buf, 0);
	memcpy(dst + off, pix, min_t(u32, p->bpl, FP_PAYLOAD));
	p->cur_lines++;

	/* Last line of the frame: YUY2 line H, or NV12 chroma line H-1 */
	if ((kind == FP_KIND_YUY2 && line == p->height) ||
	    (kind == FP_KIND_UV && line + 1 >= p->height)) {
		u32 want = kind == FP_KIND_UV ? p->height + p->height / 2 : p->height;

		fp_buffer_finish(p, p->cur_lines >= want ? VB2_BUF_STATE_DONE
							 : VB2_BUF_STATE_ERROR);
	}
}

/* Position of the next line header at or after pos, or -1. */
static long fp_find_sync(const u8 *buf, size_t len, size_t pos)
{
	const u8 *q;

	while (pos + sizeof(fp_sync) <= len) {
		q = memchr(buf + pos + 11, 0x00, len - pos - 11);
		if (!q)
			return -1;
		pos = q - buf - 11;
		if (!memcmp(buf + pos, fp_sync, sizeof(fp_sync)))
			return pos;
		pos++;
	}
	return -1;
}

/* Consume one completed chunk. Lines may straddle chunks (the stream
 * isn't guaranteed to start line-aligned), so a partial tail is carried
 * into the next call. */
static void fp_feed(struct sc0710_4port *fp, const u8 *buf, size_t len)
{
	size_t pos = 0;
	long s;

	/* Every chunk completes the carried line */
	BUILD_BUG_ON(FP_CHUNK < FP_LINE);

	if (fp->carry_len) {
		size_t need = FP_LINE - fp->carry_len;

		memcpy(fp->carry + fp->carry_len, buf, need);
		fp->carry_len = 0;
		pos = need;
		if (!memcmp(fp->carry, fp_sync, sizeof(fp_sync))) {
			fp_line(fp, fp->carry, fp->carry + FP_HDR);
			memcpy(fp->last_hdr, fp->carry, FP_HDR);
		} else
			fp->synced = false;
	}

	while (pos + FP_LINE <= len) {
		if (!fp->synced || memcmp(buf + pos, fp_sync, sizeof(fp_sync))) {
			size_t start = pos, from = pos;

			/* An input dropping out can cut its last lines short, which
			 * leaves the next header inside the line just consumed. Look
			 * there first, or the line after it (usually another
			 * input's) is skipped and that input loses a frame. */
			if (fp->synced)
				from = pos >= FP_LINE ? pos - FP_LINE + FP_HDR : 0;
			s = fp_find_sync(buf, len, from);
			fp->synced = false;
			if (s < 0) {
				/* Keep enough tail to catch a header split across chunks */
				pos = len > sizeof(fp_sync) ? len - (sizeof(fp_sync) - 1) : 0;
				break;
			}
			pos = s;
			fp->synced = true;
			/* The stream doesn't start on a line boundary (the first
			 * header lands at 3848), so the first lock isn't a resync. */
			if (!fp->locked_once) {
				fp->locked_once = true;
				continue;
			}
			fp->resyncs++;
			if (fourport_debug)
				printk_ratelimited(KERN_INFO "%s: resync in chunk %u at %zu (%+ld): last hdr %*phN, expected %*phN, found %*phN\n",
					fp->dev->name, fp->chunk_idx, (size_t)s, (long)s - (long)start,
					4, fp->last_hdr + 12, 16, buf + start, 16, buf + s);
			continue;
		}
		fp_line(fp, buf + pos, buf + pos + FP_HDR);
		memcpy(fp->last_hdr, buf + pos, FP_HDR);
		pos += FP_LINE;
	}

	if (pos < len) {
		fp->carry_len = len - pos;
		memcpy(fp->carry, buf + pos, fp->carry_len);
		/* An unsynced tail only needs a header search on the next pass */
		if (!fp->synced) {
			s = fp_find_sync(fp->carry, fp->carry_len, 0);
			if (s < 0 && fp->carry_len >= sizeof(fp_sync)) {
				memmove(fp->carry, fp->carry + fp->carry_len - (sizeof(fp_sync) - 1),
					sizeof(fp_sync) - 1);
				fp->carry_len = sizeof(fp_sync) - 1;
			} else if (s > 0) {
				memmove(fp->carry, fp->carry + s, fp->carry_len - s);
				fp->carry_len -= s;
				fp->synced = true;
			} else if (s == 0) {
				fp->synced = true;
			}
		}
	}
}

/* Partial frames in flight are useless after a gap in the stream. */
static void fp_drop_partials(struct sc0710_4port *fp)
{
	int i;

	for (i = 0; i < FP_PORTS; i++)
		if (fp->port[i].cur)
			fp_buffer_finish(&fp->port[i], VB2_BUF_STATE_ERROR);
	fp->carry_len = 0;
	fp->synced = false;
}

static void fp_service(struct sc0710_4port *fp)
{
	struct sc0710_dev *dev = fp->dev;
	u32 done, pending;

	done = sc_read(dev, 1, FP_C2H_DONE(0));
	pending = done - fp->done_seen;
	if (!pending)
		return;

	/* The engine laps the ring after nchunks completions; anything that
	 * close to a lap may already be overwritten. */
	if (pending >= fp->nchunks - 1) {
		fp->overruns++;
		fp_drop_partials(fp);
		fp->done_seen = done;
		return;
	}

	while (fp->done_seen != done) {
		u32 i = fp->done_seen % fp->nchunks;

		fp->chunk_idx = i;
		fp_feed(fp, fp->chunk_cpu[i], FP_CHUNK);
		fp->done_seen++;
	}
}

/* Copy one 10 ms block's slot for this input into its ALSA buffer. */
static void fp_audio_port(struct fp_port *p, const u8 *blk, u32 frames)
{
	struct snd_pcm_substream *sub;
	struct snd_pcm_runtime *rt;
	bool elapsed = false;
	unsigned long flags;
	u32 f;

	spin_lock_irqsave(&p->alock, flags);
	sub = p->asub;
	if (sub) {
		rt = sub->runtime;
		for (f = 0; f < frames; f++) {
			memcpy(rt->dma_area + frames_to_bytes(rt, p->apos),
			       blk + f * 16 + p->idx * 4, 4);
			if (++p->apos >= rt->buffer_size)
				p->apos = 0;
			if (++p->aperiod >= rt->period_size) {
				p->aperiod = 0;
				elapsed = true;
			}
		}
	}
	spin_unlock_irqrestore(&p->alock, flags);

	/* Outside alock: this takes the stream lock, which trigger holds
	 * while taking alock. */
	if (elapsed)
		snd_pcm_period_elapsed(sub);
}

static void fp_audio_service(struct sc0710_4port *fp)
{
	u32 done = sc_read(fp->dev, 1, FP_C2H_DONE(1));
	u32 pending = done - fp->adone_seen;
	int i;

	if (!pending)
		return;
	if (pending >= FP_AUDIO_DESCS - 1) {
		fp->aoverruns++;
		fp->adone_seen = done;
		return;
	}
	while (fp->adone_seen != done) {
		const u8 *blk = fp->abuf + (fp->adone_seen % FP_AUDIO_DESCS) * FP_AUDIO_STRIDE;

		for (i = 0; i < FP_PORTS; i++)
			fp_audio_port(&fp->port[i], blk, FP_AUDIO_CHUNK / 16);
		fp->adone_seen++;
		fp->ablocks++;
	}
}

static int fp_dma_thread(void *data)
{
	struct sc0710_4port *fp = data;

	while (!kthread_should_stop()) {
		if (!READ_ONCE(fp->running)) {
			wait_event_interruptible_timeout(fp->wq,
				READ_ONCE(fp->running) || kthread_should_stop(), HZ);
			continue;
		}
		mutex_lock(&fp->dma_lock);
		if (fp->running) {
			fp_service(fp);
			fp_audio_service(fp);
		}
		mutex_unlock(&fp->dma_lock);
		usleep_range(500, 1000);
	}
	return 0;
}

/* ----------------------------------------------------------- */
/* Input status (MCU at I2C 0x32)                               */

static int fp_read_status(struct sc0710_4port *fp, int port, struct fp_status *st)
{
	u8 buf[26];
	const u8 *b;
	int ret;

	/* The vendor reads port 0 from sub-address 0 (four leading bytes,
	 * then the block) and the others from their block start. */
	if (port == 0) {
		ret = sc0710_i2c_read_mcu(fp->dev, 0x00, buf, 26);
		b = buf + 4;
	} else {
		ret = sc0710_i2c_read_mcu(fp->dev, 0x04 + 0x3c * port, buf, 22);
		b = buf;
	}
	if (ret < 0)
		return ret;

	st->vtotal  = b[0] | b[1] << 8;
	st->htotal  = b[2] | b[3] << 8;
	st->vactive = b[4] | b[5] << 8;
	st->hactive = b[6] | b[7] << 8;
	st->fps     = b[8];
	return 0;
}

static int fp_status_thread(void *data)
{
	struct sc0710_4port *fp = data;
	struct sc0710_dev *dev = fp->dev;
	struct fp_status st;
	int i;

	while (!kthread_should_stop()) {
		for (i = 0; i < FP_PORTS && !kthread_should_stop(); i++) {
			struct fp_port *p = &fp->port[i];
			bool locked;

			if (fp_read_status(fp, i, &st) < 0)
				continue;
			locked = st.hactive && st.vactive;
			/* The rate is measured after lock, so it can read 0 at first:
			 * log mode changes on a locked input as well as lock edges. */
			if (locked != p->locked)
				printk(KERN_INFO "%s: HDMI %d %s %ux%u@%u\n", dev->name, i + 1,
					locked ? "signal" : "no signal", st.hactive, st.vactive, st.fps);
			else if (locked && (st.hactive != p->st.hactive ||
					    st.vactive != p->st.vactive || st.fps != p->st.fps))
				printk(KERN_INFO "%s: HDMI %d mode %ux%u@%u\n", dev->name, i + 1,
					st.hactive, st.vactive, st.fps);
			/* The ioctls read st/locked under the same lock */
			mutex_lock(&p->lock);
			p->st = st;
			p->locked = locked;
			fp_port_update_format(p);
			mutex_unlock(&p->lock);
		}
		/* The vendor rewrites the port-enable bits on every pass */
		if (sc_read(dev, 0, 0xec) != 0xf0)
			sc_write(dev, 0, 0xec, 0xf0);
		msleep_interruptible(max(fourport_status_ms, 20u));
	}
	return 0;
}

/* ----------------------------------------------------------- */
/* Hardware session                                             */

/* The vendor's session start (obs-open trace), verbatim. */
static void fp_hw_start(struct sc0710_4port *fp)
{
	struct sc0710_dev *dev = fp->dev;
	u8 one = 1;
	int i;

	sc_write(dev, 0, BAR0_00D0, 0x4180);
	sc_write(dev, 1, FP_C2H_CTRL_W1C(0), 1);
	sc_write(dev, 1, FP_C2H_CTRL_W1C(1), 1);

	for (i = 0; i < 2; i++) {
		sc_write(dev, 1, FP_H2C_SG(i) + FP_SG_HI, 0);
		sc_write(dev, 1, FP_H2C_SG(i) + FP_SG_LO, 0);
		sc_write(dev, 1, FP_H2C_SG(i) + FP_SG_ADJ, 0);
	}
	sc_write(dev, 1, FP_C2H_SG(0) + FP_SG_HI, upper_32_bits(fp->desc_dma));
	sc_write(dev, 1, FP_C2H_SG(0) + FP_SG_LO, lower_32_bits(fp->desc_dma));
	sc_write(dev, 1, FP_C2H_SG(0) + FP_SG_ADJ, 0);
	sc_write(dev, 1, FP_C2H_SG(1) + FP_SG_HI, upper_32_bits(fp->adesc_dma));
	sc_write(dev, 1, FP_C2H_SG(1) + FP_SG_LO, lower_32_bits(fp->adesc_dma));
	sc_write(dev, 1, FP_C2H_SG(1) + FP_SG_ADJ, 0);

	sc_write(dev, 0, BAR0_00C8, 0);
	sc_write(dev, 0, BAR0_00D0, 0x4180);
	sc_write(dev, 0, 0xcc, 0);
	sc_write(dev, 0, BAR0_00DC, 0);
	sc_write(dev, 0, BAR0_00D0, 0x4380);	/* pipeline reset pulse */
	sc_write(dev, 0, BAR0_00D0, 0x4180);

	/* The completed-descriptor count restarts at 0 on the Run edge, and
	 * the engine at the first descriptor. */
	mutex_lock(&fp->dma_lock);
	fp->done_seen = 0;
	fp->carry_len = 0;
	fp->synced = false;
	fp->locked_once = false;
	fp->overruns = 0;
	fp->resyncs = 0;
	fp->adone_seen = 0;
	fp->ablocks = 0;
	fp->aoverruns = 0;
	fp->session_start = jiffies;
	mutex_unlock(&fp->dma_lock);

	sc_write(dev, 1, FP_C2H_CTRL_W1S(0), 1);
	sc_write(dev, 1, FP_C2H_CTRL_W1S(1), 1);
	sc_set(dev, 0, BAR0_00D0, 0x0001);	/* GO */

	/* MCU 0x11 <- 1 after GO, as the vendor does on every session start
	 * (the MK.2 code knows 0x11 as its HDR->SDR tonemap switch). */
	sc0710_i2c_write_mcu(dev, 0x11, &one, 1);

	WRITE_ONCE(fp->running, true);
	wake_up(&fp->wq);
	printk(KERN_DEBUG "%s: capture session started\n", dev->name);
}

static void fp_hw_stop(struct sc0710_4port *fp)
{
	struct sc0710_dev *dev = fp->dev;

	mutex_lock(&fp->dma_lock);
	WRITE_ONCE(fp->running, false);
	fp_drop_partials(fp);
	mutex_unlock(&fp->dma_lock);

	sc_clr(dev, 0, BAR0_00D0, 0x0001);
	sc_write(dev, 1, FP_C2H_CTRL_W1C(0), 1);
	sc_write(dev, 1, FP_C2H_CTRL_W1C(1), 1);
	/* 100 audio blocks per second means the 48 kHz assumption holds */
	printk(KERN_DEBUG "%s: capture session stopped (overruns %llu, resyncs %llu, audio %llu blocks in %u ms, %llu overruns)\n",
		dev->name, fp->overruns, fp->resyncs, fp->ablocks,
		jiffies_to_msecs(jiffies - fp->session_start), fp->aoverruns);
}

/* The stream runs while any node is open, so formats are known (from the
 * line types) before a client negotiates, and for FP_IDLE_STOP_MS after
 * the last close. */
static int fp_hw_get(struct sc0710_4port *fp)
{
	int ret = 0;

	mutex_lock(&fp->hw_lock);
	if (fp->disconnected) {
		ret = -ENODEV;
	} else {
		fp->users++;
		cancel_delayed_work(&fp->stop_work);
		if (!fp->running)
			fp_hw_start(fp);
	}
	mutex_unlock(&fp->hw_lock);
	return ret;
}

static void fp_hw_put(struct sc0710_4port *fp)
{
	mutex_lock(&fp->hw_lock);
	if (--fp->users == 0 && !fp->disconnected)
		schedule_delayed_work(&fp->stop_work, msecs_to_jiffies(FP_IDLE_STOP_MS));
	mutex_unlock(&fp->hw_lock);
}

/* A get that raced the timer and lost the cancel finds users > 0 here. */
static void fp_stop_work(struct work_struct *work)
{
	struct sc0710_4port *fp = container_of(to_delayed_work(work), struct sc0710_4port, stop_work);

	mutex_lock(&fp->hw_lock);
	if (!fp->users && fp->running && !fp->disconnected)
		fp_hw_stop(fp);
	mutex_unlock(&fp->hw_lock);
}

/* ----------------------------------------------------------- */
/* vb2                                                          */

static int fp_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
	unsigned int *nplanes, unsigned int sizes[], struct device *alloc_devs[])
{
	struct fp_port *p = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < p->sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = p->sizeimage;
	if (*nbuffers < 3)
		*nbuffers = 3;
	return 0;
}

static int fp_buf_prepare(struct vb2_buffer *vb)
{
	struct fp_port *p = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < p->sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, p->sizeimage);
	return 0;
}

static void fp_buf_queue(struct vb2_buffer *vb)
{
	struct fp_port *p = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct fp_buffer *buf = container_of(vbuf, struct fp_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&p->qlock, flags);
	list_add_tail(&buf->list, &p->bufs);
	spin_unlock_irqrestore(&p->qlock, flags);
}

static void fp_return_all(struct fp_port *p, enum vb2_buffer_state state)
{
	struct fp_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&p->qlock, flags);
	list_for_each_entry_safe(buf, tmp, &p->bufs, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&p->qlock, flags);
}

static int fp_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct fp_port *p = vb2_get_drv_priv(q);
	struct sc0710_4port *fp = p->fp;

	mutex_lock(&fp->dma_lock);
	p->sequence = 0;
	p->cur = NULL;
	WRITE_ONCE(p->streaming, true);
	mutex_unlock(&fp->dma_lock);
	return 0;
}

static void fp_stop_streaming(struct vb2_queue *q)
{
	struct fp_port *p = vb2_get_drv_priv(q);
	struct sc0710_4port *fp = p->fp;

	mutex_lock(&fp->dma_lock);
	WRITE_ONCE(p->streaming, false);
	if (p->cur)
		fp_buffer_finish(p, VB2_BUF_STATE_ERROR);
	mutex_unlock(&fp->dma_lock);
	fp_return_all(p, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops fp_vb2_ops = {
	.queue_setup     = fp_queue_setup,
	.buf_prepare     = fp_buf_prepare,
	.buf_queue       = fp_buf_queue,
	.start_streaming = fp_start_streaming,
	.stop_streaming  = fp_stop_streaming,
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
#endif
};

/* ----------------------------------------------------------- */
/* V4L2                                                         */

static int fp_open(struct file *file)
{
	struct fp_port *p = video_drvdata(file);
	int ret;

	ret = v4l2_fh_open(file);
	if (ret)
		return ret;
	ret = fp_hw_get(p->fp);
	if (ret)
		v4l2_fh_release(file);
	return ret;
}

static int fp_release(struct file *file)
{
	struct fp_port *p = video_drvdata(file);
	int ret;

	ret = vb2_fop_release(file);
	fp_hw_put(p->fp);
	return ret;
}

static void fp_fill_fmt(struct fp_port *p, struct v4l2_pix_format *pix)
{
	pix->width = p->width;
	pix->height = p->height;
	pix->pixelformat = p->pixfmt;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = p->bpl;
	pix->sizeimage = p->sizeimage;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static int fp_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct fp_port *p = video_drvdata(file);

	strscpy(cap->driver, "sc0710", sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "Cam Link Pro HDMI %d", p->idx + 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", pci_name(p->fp->dev->pci));
	return 0;
}

static int fp_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct fp_port *p = video_drvdata(file);

	/* The card picks the format per input; offer that one only */
	if (f->index)
		return -EINVAL;
	f->pixelformat = p->pixfmt;
	return 0;
}

static int fp_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	fp_fill_fmt(video_drvdata(file), &f->fmt.pix);
	return 0;
}

static int fp_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct fp_port *p = video_drvdata(file);

	if (vb2_is_busy(&p->q))
		return -EBUSY;
	fp_fill_fmt(p, &f->fmt.pix);
	return 0;
}

static int fp_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	struct fp_port *p = video_drvdata(file);

	if (fs->index || fs->pixel_format != p->pixfmt)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = p->width;
	fs->discrete.height = p->height;
	return 0;
}

static int fp_enum_frameintervals(struct file *file, void *priv, struct v4l2_frmivalenum *fi)
{
	struct fp_port *p = video_drvdata(file);

	if (fi->index || fi->pixel_format != p->pixfmt ||
	    fi->width != p->width || fi->height != p->height)
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = 1;
	fi->discrete.denominator = p->fps;
	return 0;
}

static int fp_g_parm(struct file *file, void *priv, struct v4l2_streamparm *sp)
{
	struct fp_port *p = video_drvdata(file);

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.timeperframe.numerator = 1;
	sp->parm.capture.timeperframe.denominator = p->fps;
	sp->parm.capture.readbuffers = 3;
	return 0;
}

static int fp_enum_input(struct file *file, void *priv, struct v4l2_input *i)
{
	struct fp_port *p = video_drvdata(file);

	if (i->index)
		return -EINVAL;
	i->type = V4L2_INPUT_TYPE_CAMERA;
	i->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	snprintf(i->name, sizeof(i->name), "HDMI %d", p->idx + 1);
	i->status = p->locked ? 0 : V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int fp_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int fp_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

/* What a line can carry bounds the modes; the MCU reports only totals,
 * actives and an integer rate, so only progressive modes are known. */
static const struct v4l2_dv_timings_cap fp_dv_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 640,
		.max_width = 3840,
		.min_height = 480,
		.max_height = 2160,
		.min_pixelclock = 25000000,
		.max_pixelclock = 600000000,
		.standards = V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT |
			     V4L2_DV_BT_STD_CVT | V4L2_DV_BT_STD_GTF,
		.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE,
	},
};

/* Timings of the input: the standard mode with the same raster and rate
 * if there is one, else the raster with all blanking in the back porch
 * (the MCU doesn't report sync widths or polarities). */
static int fp_port_timings(struct fp_port *p, struct v4l2_dv_timings *t)
{
	struct fp_status st = p->st;
	struct v4l2_bt_timings *bt = &t->bt;
	int i;

	if (!p->locked)
		return -ENOLINK;
	if (!st.fps || st.htotal < st.hactive || st.vtotal < st.vactive)
		return -ENOLCK;

	for (i = 0; v4l2_dv_timings_presets[i].bt.width; i++) {
		const struct v4l2_bt_timings *pb = &v4l2_dv_timings_presets[i].bt;
		u32 htot = V4L2_DV_BT_FRAME_WIDTH(pb), vtot = V4L2_DV_BT_FRAME_HEIGHT(pb);

		if (pb->interlaced || pb->width != st.hactive || pb->height != st.vactive ||
		    htot != st.htotal || vtot != st.vtotal ||
		    DIV_ROUND_CLOSEST_ULL(pb->pixelclock, (u64)htot * vtot) != st.fps)
			continue;
		*t = v4l2_dv_timings_presets[i];
		return 0;
	}

	memset(t, 0, sizeof(*t));
	t->type = V4L2_DV_BT_656_1120;
	bt->width = st.hactive;
	bt->height = st.vactive;
	bt->hbackporch = st.htotal - st.hactive;
	bt->vbackporch = st.vtotal - st.vactive;
	bt->pixelclock = (u64)st.htotal * st.vtotal * st.fps;
	return 0;
}

static int fp_query_dv_timings(struct file *file, void *priv, struct v4l2_dv_timings *t)
{
	return fp_port_timings(video_drvdata(file), t);
}

/* The format follows the input by itself, so the current timings are
 * the detected ones and the only ones that can be set. */
static int fp_g_dv_timings(struct file *file, void *priv, struct v4l2_dv_timings *t)
{
	return fp_port_timings(video_drvdata(file), t) ? -ENODATA : 0;
}

static int fp_s_dv_timings(struct file *file, void *priv, struct v4l2_dv_timings *t)
{
	struct v4l2_dv_timings cur;

	if (fp_port_timings(video_drvdata(file), &cur) ||
	    !v4l2_match_dv_timings(t, &cur, 0, false))
		return -EINVAL;
	*t = cur;
	return 0;
}

static int fp_enum_dv_timings(struct file *file, void *priv, struct v4l2_enum_dv_timings *et)
{
	if (et->pad || et->index)
		return -EINVAL;
	memset(et->reserved, 0, sizeof(et->reserved));
	return fp_port_timings(video_drvdata(file), &et->timings) ? -EINVAL : 0;
}

static int fp_dv_timings_cap(struct file *file, void *priv, struct v4l2_dv_timings_cap *cap)
{
	if (cap->pad)
		return -EINVAL;
	*cap = fp_dv_cap;
	return 0;
}

static int fp_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	return -EINVAL;
}

static const struct v4l2_ioctl_ops fp_ioctl_ops = {
	.vidioc_querycap              = fp_querycap,
	.vidioc_enum_fmt_vid_cap      = fp_enum_fmt,
	.vidioc_g_fmt_vid_cap         = fp_g_fmt,
	.vidioc_try_fmt_vid_cap       = fp_g_fmt,
	.vidioc_s_fmt_vid_cap         = fp_s_fmt,
	.vidioc_enum_framesizes       = fp_enum_framesizes,
	.vidioc_enum_frameintervals   = fp_enum_frameintervals,
	.vidioc_g_parm                = fp_g_parm,
	.vidioc_s_parm                = fp_g_parm,
	.vidioc_enum_input            = fp_enum_input,
	.vidioc_g_input               = fp_g_input,
	.vidioc_s_input               = fp_s_input,
	.vidioc_query_dv_timings      = fp_query_dv_timings,
	.vidioc_g_dv_timings          = fp_g_dv_timings,
	.vidioc_s_dv_timings          = fp_s_dv_timings,
	.vidioc_enum_dv_timings       = fp_enum_dv_timings,
	.vidioc_dv_timings_cap        = fp_dv_timings_cap,
	.vidioc_reqbufs               = vb2_ioctl_reqbufs,
	.vidioc_create_bufs           = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf           = vb2_ioctl_prepare_buf,
	.vidioc_querybuf              = vb2_ioctl_querybuf,
	.vidioc_qbuf                  = vb2_ioctl_qbuf,
	.vidioc_dqbuf                 = vb2_ioctl_dqbuf,
	.vidioc_expbuf                = vb2_ioctl_expbuf,
	.vidioc_streamon              = vb2_ioctl_streamon,
	.vidioc_streamoff             = vb2_ioctl_streamoff,
	.vidioc_subscribe_event       = fp_subscribe_event,
	.vidioc_unsubscribe_event     = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations fp_fops = {
	.owner          = THIS_MODULE,
	.open           = fp_open,
	.release        = fp_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.mmap           = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* Everything the DMA and status threads can touch, so it must run before
 * either thread starts. */
static void fp_port_init(struct sc0710_4port *fp, int idx)
{
	struct fp_port *p = &fp->port[idx];

	p->fp = fp;
	p->idx = idx;
	mutex_init(&p->lock);
	spin_lock_init(&p->qlock);
	spin_lock_init(&p->alock);
	INIT_LIST_HEAD(&p->bufs);
	fp_port_set_format(p, V4L2_PIX_FMT_YUYV, 1920, 1080, 60);
}

static int fp_port_register(struct sc0710_4port *fp, int idx)
{
	struct fp_port *p = &fp->port[idx];
	struct vb2_queue *q = &p->q;
	int ret;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = p;
	q->buf_struct_size = sizeof(struct fp_buffer);
	q->ops = &fp_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &p->lock;
	q->dev = &fp->dev->pci->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	snprintf(p->vdev.name, sizeof(p->vdev.name), "Cam Link Pro HDMI %d", idx + 1);
	p->vdev.fops = &fp_fops;
	p->vdev.ioctl_ops = &fp_ioctl_ops;
	p->vdev.release = video_device_release_empty;
	p->vdev.v4l2_dev = &fp->dev->v4l2_dev;
	p->vdev.lock = &p->lock;
	p->vdev.queue = q;
	p->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	video_set_drvdata(&p->vdev, p);

	ret = video_register_device(&p->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;
	printk(KERN_INFO "%s: HDMI %d -> %s\n", fp->dev->name, idx + 1,
		video_device_node_name(&p->vdev));
	return 0;
}

static void fp_port_unregister(struct fp_port *p)
{
	if (!video_is_registered(&p->vdev))
		return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	vb2_video_unregister_device(&p->vdev);
#else
	video_unregister_device(&p->vdev);
#endif
}

/* ----------------------------------------------------------- */
/* ALSA                                                         */

static const struct snd_pcm_hardware fp_pcm_hw = {
	.info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			    SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = 48000,
	.rate_max         = 48000,
	.channels_min     = 2,
	.channels_max     = 2,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 480 * 4,	/* one 10 ms DMA block */
	.period_bytes_max = 64 * 1024,
	.periods_min      = 2,
	.periods_max      = 64,
};

static int fp_pcm_open(struct snd_pcm_substream *sub)
{
	struct fp_port *p = snd_pcm_substream_chip(sub);
	int ret;

	ret = fp_hw_get(p->fp);
	if (ret)
		return ret;
	/* An open PCM pins dev (and with it fp) past a remove, like an open
	 * video node does through the v4l2 core. */
	v4l2_device_get(&p->fp->dev->v4l2_dev);
	sub->runtime->hw = fp_pcm_hw;
	return snd_pcm_hw_constraint_integer(sub->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int fp_pcm_close(struct snd_pcm_substream *sub)
{
	struct fp_port *p = snd_pcm_substream_chip(sub);
	struct sc0710_4port *fp = p->fp;
	struct sc0710_dev *dev = fp->dev;

	/* Wait out a DMA pass that may still signal this substream */
	mutex_lock(&fp->dma_lock);
	mutex_unlock(&fp->dma_lock);
	fp_hw_put(fp);
	v4l2_device_put(&dev->v4l2_dev);
	return 0;
}

static int fp_pcm_prepare(struct snd_pcm_substream *sub)
{
	struct fp_port *p = snd_pcm_substream_chip(sub);
	unsigned long flags;

	spin_lock_irqsave(&p->alock, flags);
	p->apos = 0;
	p->aperiod = 0;
	spin_unlock_irqrestore(&p->alock, flags);
	return 0;
}

static int fp_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct fp_port *p = snd_pcm_substream_chip(sub);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock(&p->alock);
		p->asub = sub;
		spin_unlock(&p->alock);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock(&p->alock);
		p->asub = NULL;
		spin_unlock(&p->alock);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t fp_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct fp_port *p = snd_pcm_substream_chip(sub);

	return READ_ONCE(p->apos);
}

static const struct snd_pcm_ops fp_pcm_ops = {
	.open    = fp_pcm_open,
	.close   = fp_pcm_close,
	.prepare = fp_pcm_prepare,
	.trigger = fp_pcm_trigger,
	.pointer = fp_pcm_pointer,
};

/* One card, PCM device n = HDMI n+1 */
static int fp_audio_register(struct sc0710_4port *fp)
{
	struct sc0710_dev *dev = fp->dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int i, ret;

	ret = snd_card_new(&dev->pci->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, 0, &card);
	if (ret)
		return ret;
	strscpy(card->driver, "sc0710", sizeof(card->driver));
	strscpy(card->shortname, "Cam Link Pro", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "Elgato Cam Link Pro at %s",
		 pci_name(dev->pci));

	for (i = 0; i < FP_PORTS; i++) {
		ret = snd_pcm_new(card, "Cam Link Pro", i, 0, 1, &pcm);
		if (ret)
			goto fail;
		pcm->private_data = &fp->port[i];
		snprintf(pcm->name, sizeof(pcm->name), "Cam Link Pro HDMI %d", i + 1);
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &fp_pcm_ops);
		snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	}

	ret = snd_card_register(card);
	if (ret)
		goto fail;
	fp->card = card;
	printk(KERN_INFO "%s: audio: ALSA card %d, PCM 0-3 = HDMI 1-4\n", dev->name, card->number);
	return 0;

fail:
	snd_card_free(card);
	return ret;
}

/* ----------------------------------------------------------- */
/* Probe / remove                                               */

static void fp_free_rings(struct sc0710_4port *fp)
{
	struct device *d = &fp->dev->pci->dev;
	u32 i;

	for (i = 0; i < fp->nchunks; i++)
		dma_free_coherent(d, FP_CHUNK, fp->chunk_cpu[i], fp->chunk_dma[i]);
	fp->nchunks = 0;
	if (fp->desc)
		dma_free_coherent(d, 256 * sizeof(*fp->desc), fp->desc, fp->desc_dma);
	if (fp->adesc)
		dma_free_coherent(d, FP_AUDIO_DESCS * sizeof(*fp->adesc), fp->adesc, fp->adesc_dma);
	if (fp->abuf)
		dma_free_coherent(d, FP_AUDIO_DESCS * FP_AUDIO_STRIDE, fp->abuf, fp->abuf_dma);
	fp->desc = NULL;
	fp->adesc = NULL;
	fp->abuf = NULL;
}

static void fp_desc_set(struct sc0710_dma_descriptor *d, u32 len, dma_addr_t dst, dma_addr_t next)
{
	d->control = FP_DESC_CTRL;
	d->lengthBytes = len;
	d->src_l = 0;
	d->src_h = 0;
	d->dst_l = lower_32_bits(dst);
	d->dst_h = upper_32_bits(dst);
	d->next_l = lower_32_bits(next);
	d->next_h = upper_32_bits(next);
}

static int fp_alloc_rings(struct sc0710_4port *fp)
{
	struct device *d = &fp->dev->pci->dev;
	u32 want = clamp(fourport_ring_chunks, 8u, 256u), i;

	fp->desc = dma_alloc_coherent(d, 256 * sizeof(*fp->desc), &fp->desc_dma, GFP_KERNEL);
	fp->adesc = dma_alloc_coherent(d, FP_AUDIO_DESCS * sizeof(*fp->adesc), &fp->adesc_dma, GFP_KERNEL);
	fp->abuf = dma_alloc_coherent(d, FP_AUDIO_DESCS * FP_AUDIO_STRIDE, &fp->abuf_dma, GFP_KERNEL);
	if (!fp->desc || !fp->adesc || !fp->abuf)
		return -ENOMEM;

	for (i = 0; i < want; i++) {
		fp->chunk_cpu[i] = dma_alloc_coherent(d, FP_CHUNK, &fp->chunk_dma[i], GFP_KERNEL);
		if (!fp->chunk_cpu[i])
			break;
		fp->nchunks++;
	}
	if (fp->nchunks < 8)
		return -ENOMEM;
	if (fp->nchunks < want)
		printk(KERN_WARNING "%s: video ring limited to %u of %u chunks\n",
			fp->dev->name, fp->nchunks, want);

	for (i = 0; i < fp->nchunks; i++)
		fp_desc_set(&fp->desc[i], FP_CHUNK, fp->chunk_dma[i],
			fp->desc_dma + ((i + 1) % fp->nchunks) * sizeof(*fp->desc));
	for (i = 0; i < FP_AUDIO_DESCS; i++)
		fp_desc_set(&fp->adesc[i], FP_AUDIO_CHUNK, fp->abuf_dma + i * FP_AUDIO_STRIDE,
			fp->adesc_dma + ((i + 1) % FP_AUDIO_DESCS) * sizeof(*fp->adesc));

	printk(KERN_INFO "%s: video ring %u x %u bytes, audio ring %u x %u bytes\n",
		fp->dev->name, fp->nchunks, FP_CHUNK, FP_AUDIO_DESCS, FP_AUDIO_CHUNK);
	return 0;
}

/* Per-port enable bits (BAR0 0xEC bit 4+n): the vendor clears them one
 * port at a time and sets them again the same way, ~22 ms apart. */
static void fp_ports_enable(struct sc0710_dev *dev)
{
	static const u32 seq[] = { 0xe0, 0xc0, 0x80, 0x00, 0x10, 0x30, 0x70, 0xf0 };
	int i;

	sc_write(dev, 0, 0xec, 0xf0);
	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		msleep(22);
		sc_write(dev, 0, 0xec, seq[i]);
	}
}

int sc0710_4port_probe(struct sc0710_dev *dev)
{
	struct sc0710_4port *fp;
	int i, ret;

	fp = kzalloc(sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	fp->dev = dev;
	dev->fourport = fp;
	mutex_init(&fp->hw_lock);
	INIT_DELAYED_WORK(&fp->stop_work, fp_stop_work);
	mutex_init(&fp->dma_lock);
	init_waitqueue_head(&fp->wq);

	for (i = 0; i < FP_PORTS; i++)
		fp_port_init(fp, i);

	ret = fp_alloc_rings(fp);
	if (ret)
		goto fail;

	fp_ports_enable(dev);

	fp->dma_thread = kthread_run(fp_dma_thread, fp, "sc0710 4port dma");
	if (IS_ERR(fp->dma_thread)) {
		ret = PTR_ERR(fp->dma_thread);
		fp->dma_thread = NULL;
		goto fail;
	}
	fp->status_thread = kthread_run(fp_status_thread, fp, "sc0710 4port status");
	if (IS_ERR(fp->status_thread)) {
		ret = PTR_ERR(fp->status_thread);
		fp->status_thread = NULL;
		goto fail;
	}

	/* Nodes last: once registered they can be opened */
	for (i = 0; i < FP_PORTS; i++) {
		ret = fp_port_register(fp, i);
		if (ret) {
			printk(KERN_ERR "%s: HDMI %d node registration failed (%d)\n",
				dev->name, i + 1, ret);
			goto fail;
		}
	}

	/* Audio is optional, as for the other boards */
	ret = fp_audio_register(fp);
	if (ret)
		printk(KERN_WARNING "%s: audio registration failed (%d), continuing without audio\n",
			dev->name, ret);
	return 0;

fail:
	sc0710_4port_remove(dev);
	return ret;
}

void sc0710_4port_remove(struct sc0710_dev *dev)
{
	struct sc0710_4port *fp = dev->fourport;
	int i;

	if (!fp)
		return;

	/* Nodes first: stops any streaming queue and refuses new opens.
	 * Open PCMs hold their own dev reference and see the disconnect. */
	for (i = 0; i < FP_PORTS; i++)
		fp_port_unregister(&fp->port[i]);
	if (fp->card) {
		snd_card_disconnect(fp->card);
		snd_card_free_when_closed(fp->card);
		fp->card = NULL;
	}

	if (fp->status_thread)
		kthread_stop(fp->status_thread);
	fp->status_thread = NULL;

	/* Stop the engines before freeing anything they write to; handles
	 * that are still open only drop their user count from here on. */
	mutex_lock(&fp->hw_lock);
	fp->disconnected = true;
	if (fp->running)
		fp_hw_stop(fp);
	mutex_unlock(&fp->hw_lock);
	cancel_delayed_work_sync(&fp->stop_work);

	if (fp->dma_thread)
		kthread_stop(fp->dma_thread);
	fp->dma_thread = NULL;

	fp_free_rings(fp);
}

#ifdef CONFIG_PROC_FS
/* DMA bytes per second for one input: the card sends every locked input,
 * captured or not, as fixed-size lines (NV12: Y then UV lines; YUY2: one
 * line per row). */
static u64 fp_port_bps(const struct fp_port *p)
{
	u32 lines = p->pixfmt == V4L2_PIX_FMT_NV12 ? p->height * 3 / 2 : p->height;

	return (u64)lines * FP_LINE * p->fps;
}

/* /proc/sc0710-state for the demux path. Cached state only, as for the
 * other boards: no I2C or register access from this world-readable file.
 * Called with the core's devlist mutex held, so fp can't go away. */
void sc0710_4port_proc_state(struct seq_file *m, struct sc0710_dev *dev)
{
	struct sc0710_4port *fp = dev->fourport;
	u64 total = 0;
	int i, nlocked = 0;

	seq_printf(m, "     capture: clp_demux (software demux of C2H 0)\n");
	for (i = 0; i < FP_PORTS; i++) {
		struct fp_port *p = &fp->port[i];

		mutex_lock(&p->lock);
		if (p->locked) {
			u64 bps = fp_port_bps(p);

			/* The first locked input also fills the single-input
			 * "HDMI:" and "req bytes/s:" fields that sc0710-cli reads. */
			if (!nlocked++)
				seq_printf(m, "        HDMI: %ux%up%u (HDMI-%d)\n",
					p->width, p->height, p->fps, i + 1);
			total += bps;
			seq_printf(m, "      HDMI-%d: %ux%up%u %c%c%c%c, %llu MB/s, %s%s, %llu frames, %llu dropped\n",
				i + 1, p->width, p->height, p->fps,
				p->pixfmt & 0xff, (p->pixfmt >> 8) & 0xff,
				(p->pixfmt >> 16) & 0xff, (p->pixfmt >> 24) & 0xff,
				bps / 1000000,
				video_is_registered(&p->vdev) ?
					video_device_node_name(&p->vdev) : "-",
				p->streaming ? " [streaming]" : "",
				p->frames, p->dropped);
		} else {
			seq_printf(m, "      HDMI-%d: no signal, %s\n", i + 1,
				video_is_registered(&p->vdev) ?
					video_device_node_name(&p->vdev) : "-");
		}
		mutex_unlock(&p->lock);
	}
	if (!nlocked)
		seq_printf(m, "        HDMI: no signal\n");
	else
		/* No fourcc here: the card picks each input's format itself */
		seq_printf(m, "  req bytes/s: %llu (%llu MB/s for %d locked input%s)\n",
			total, total / 1000000, nlocked, nlocked == 1 ? "" : "s");

	mutex_lock(&fp->hw_lock);
	seq_printf(m, "     session: %s, %u users, %llu overruns, %llu resyncs\n",
		fp->running ? "running" : "idle", fp->users,
		fp->overruns, fp->resyncs);
	if (fp->card)
		seq_printf(m, "       audio: ALSA card %d, %llu blocks, %llu overruns\n",
			fp->card->number, fp->ablocks, fp->aoverruns);
	else
		seq_printf(m, "       audio: none\n");
	mutex_unlock(&fp->hw_lock);
}
#endif

/* Last reference to the v4l2_device gone: no node can reach fp anymore. */
void sc0710_4port_release(struct sc0710_dev *dev)
{
	kfree(dev->fourport);
	dev->fourport = NULL;
}
