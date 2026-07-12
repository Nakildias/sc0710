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

/* Each FPGA DMA descriptor is 8xDWORD (32 Bytes)
 * We're going to have 8 descriptors per channel, where
 * each descriptor is either a frame of video or a 'chunk' of audio.
 * The entire descriptor pagetable for a single channel will fit
 * inside a single page of memory. (8 * 32).
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/vmalloc.h>

#include "sc0710.h"

#define dprintk(level, fmt, arg...)\
        do { if (sc0710_debug_mode >= level)\
                printk(KERN_DEBUG "%s: " fmt, dev->name, ## arg);\
        } while (0)

#define DMA_AUDIO_TRANSFER_SIZE 0x4000
#define DMA_TRANSFER_CHAINS     4

bool sc0710_guess_dims_from_framesize(u32 frame_bytes, u32 *w, u32 *h)
{
	struct {
		u32 width;
		u32 height;
	} candidates[] = {
		{ 3840, 2160 },
		{ 2560, 1440 },
		{ 1920, 1080 },
		{ 1280,  720 },
	};
	u32 best_diff = U32_MAX;
	u32 best_w = 0, best_h = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		u32 fs = candidates[i].width * 2 * candidates[i].height;
		u32 diff = (frame_bytes > fs) ? (frame_bytes - fs) : (fs - frame_bytes);
		if (diff < best_diff) {
			best_diff = diff;
			best_w = candidates[i].width;
			best_h = candidates[i].height;
		}
	}

	/* Accept page-aligned/allocator-rounded sizes that are still very close. */
	if (best_diff <= (PAGE_SIZE * 16)) {
		*w = best_w;
		*h = best_h;
		return true;
	}

	return false;
}

/* Weave two vertically-stacked fields into a proper interlaced frame.
 * The hardware delivers interlaced content as Field 1 (top) followed by
 * Field 2 (bottom) in a single DMA buffer.  This function interleaves
 * the lines so that even output lines come from Field 1 and odd output
 * lines come from Field 2, producing a standard V4L2_FIELD_INTERLACED frame.
 *
 * @src and @dst must not overlap.  @height is the full frame height
 * (must be even).
 */
static void sc0710_weave_fields(const u8 *src, u8 *dst, u32 width, u32 height)
{
	u32 stride = width * 2; /* YUYV: 2 bytes per pixel */
	u32 field_height = height / 2;
	const u8 *field1 = src;
	const u8 *field2 = src + field_height * stride;
	u32 y;

	for (y = 0; y < field_height; y++) {
		memcpy(dst + (y * 2)     * stride, field1 + y * stride, stride);
		memcpy(dst + (y * 2 + 1) * stride, field2 + y * stride, stride);
	}
}

/* Detect a likely persistent horizontal tear seam.
 * This runs only in a short post-resync validation window.
 */
static bool sc0710_detect_horizontal_tear(const u8 *buf, u32 width, u32 height, int *tear_line)
{
	u32 stride;
	u32 step_x;
	u64 avg_score = 0;
	u32 max_score = 0;
	int max_line = -1;
	u32 y;

	if (!buf || width < 320 || height < 120)
		return false;

	stride = width * 2; /* YUYV */
	step_x = width / 128;
	if (step_x < 8)
		step_x = 8;

	for (y = 0; y + 1 < height; y++) {
		const u8 *row0 = buf + (y * stride);
		const u8 *row1 = row0 + stride;
		u32 x;
		u32 score = 0;
		u32 samples = 0;

		for (x = 0; x < width; x += step_x) {
			u32 off = x * 2; /* Y component per pixel */
			score += abs((int)row1[off] - (int)row0[off]);
			samples++;
		}
		if (samples)
			score /= samples;

		avg_score += score;
		if (score > max_score) {
			max_score = score;
			max_line = (int)y;
		}
	}

	if (height > 1)
		avg_score /= (height - 1);

	/* Require both absolute and relative separation from baseline. */
	if (max_score >= 42 && max_score > (avg_score * 2 + 12)) {
		if (tear_line)
			*tear_line = max_line;
		return true;
	}

	return false;
}

/* The ways of processing the DMA.
 * 1. Polled
 * 2. IRQ.
 * 
 * The first implementation is polled. See why below.
 * Later, we added IRQ support.
 *
 * 1. Poll Support:
 *
 * We're going to rely on a 2ms kernel thread to poll and dequeue
 * buffers.
 *
 * This matches the windows driver design, and after review the industry
 * (xilinx) believe that a looping descriptor set, that runs consistent
 * and never terminates, it needs no IRQ servicing
 * keeps the DMA bus 100% busy, all the time and maximises throughput on
 * the DMA channel. Where as, (IRQ servicing) waiting for an
 * interrupt to services the DMA system (which its stopped) introduces
 * unwanted latency.
 *
 * The basic design for this polled dma driver is, every 2ms this
 * function is called. We'll read the DMA controller
 * 'descriptors count complete' register for this channel. We see
 * the windows driver doing this (via the PCIe analyzer).
 *
 * If the descriptor counter has changed since the last time we read
 * then another descriptor has completed (which contains an entire
 * frame).
 *
 * Upon descriptor completion, we'll look up which descriptor
 * has changed, and copy the data out of the descriptor buffer BEFORE
 * the dma subsystem has chance to overwrite it.
 *
 * Each channel has N chains (ch->numDescriptorChains) of descriptors, a minimum of four, this
 * lets us splut very large video frames into smaller and more reasonable
 * scatter gather PCIe memory allocations, rathar than assuming
 * we can allocate a single valuable chunk of ram for a 4K video frame.
 *
 * So, our latency is the counter change, we notice 2ms later, we spend
 * micro seconds looking at each descriptor in turn (N - typically 6),
 * when we detect that its changed, we'll immediately memcpy the dma
 * dest buffer into a previously allocated user facing video4linux buffer.
 *
 * 1. We'll allocate two PAGEs of PCIe root addressible ram
 *    to hold a) scatter gather descriptors and
 *            b) metadata writeback data provided
 *    by the card root controller.
 *
 *    When the DMA controller finishes a descriptor, it updates
 *    the metadata writeback so we'll monitor the metadata to see
 *    which descriptor chains have finished.
 *
 *    Descriptor1ChainA (first quarter of the video frame) when
 *                      complete, continues at chain2.
 *    Descriptor1ChainB (second quarter of the video frame) when
 *                      complete, continues at chain3.
 *    Descriptor1ChainC (third quarter of the video frame) when
 *                      complete, continues at chain4.
 *    Descriptor1ChainD (last quarter of the video frame) when
 *                      complete, continues at descriptor2ChainA, and
 *                      metadata writeback data is incremented.
 *    At the end of Descriptor3ChainD, processing wraps and continues
 *    back at the very beginning of Descriptor1ChainA.
 *
 *    PAGE 1 PCIe root addressible:
 *    0x0000  descriptorChain1a
 *    0x0020  descriptorChain1b
 *    0x0040  descriptorChain1c
 *    0x0060  descriptorChain1d
 *    0x0080  descriptorChain2a
 *    0x00a0  descriptorChain2b
 *    ... etc
 *    PAGE 2 PCIe root addressible:
 *    0x1000  descriptor1 writeback metadata location
 *    0x1020  descriptor2 writeback metadata location
 *    0x1030  descriptor3 writeback metadata location
 *    0x1040  descriptor4 writeback metadata location
 *    0x1050  descriptor5 writeback metadata location
 *    0x1060  descriptor6 writeback metadata location
 *
 * 2. We'll allocate multiple large DMA addressible buffers
 *    to hold the final pixels and audio. These will be referenced
 *    by the descriptors in each chain. A chain is expected to
 *    contain a single video picture, and create multiple
 *    buffers and point multiple descriptors at the buffers
 *    in order that the DMA controller can DMA the data into RAM.
 *
 * 3. The descriptors will contain lengths for the dma transfer and
 *    locations for the metadata writeback to happen.
 *
 * ---
 *
 * During testing of Poll mode, I would see occasional frame alignment
 * issues in ffmpeg. Meaning, the top of the frame would start vrttically
 * in the front place in the frame. Since the beginning of the transfer is
 * assuming to contain (ALWAYS) VSYNC, if I switch to IRQ based transfers,
 * do I avoid this unusual condition? I don't know, but I'm trying it anyway.
 *
 * 2. IRQ Support:
 *
 * Insteaf of writing descriptor tables that constaintly run, when complete
 * restarting at the beginning (and incrementing the writeback metdata), I
 * will have the descriptor processing stop. An interrupt should be triggered.
 * A new interrupt handler (the driver currently never requests the IRQ)
 * will then take ownership of determining which descriptor just stopped,
 * immediately starting a different descriptor, then deferring service
 * of the completeed descriptor chain into a worker thread.
 *
 * a. in sc0710_thread_dma_function(), don't call dma_channels_service()
 *    every 2ms.
 * b. in sc0710_dma_channel_chains_link() instead of having the last descriptor
 *    loop back to the first, terminate the descriptor chain and raise
 *    an interrupt.
 * c. in the new irq handler, using a variation of dma_channels_service(),
 *    - look at every descriptor, find the first free descriptor not
 *      being used and put that back on the hardware for the next transfer.
 *    - Find the decriptor chain just completed, schedule this for
 *      dequeue in a workthread at some future point in time.
 * d. Create a workthread that takes takes a specific dma chain,
 *    dq's it to the audio / video subsystems, cleans up the chain,
 *    marks is as empty then the irq handler can use this thread in the future
 *    to perform transfers.
 */

/* Copy the contains of the video chain into a video4linux buffer.
 * Return < 0 on error
 * Return number of buffers we copyinto from dma into user buffers.
 * 
 * @cached_framesize: Frame size cached at service start to prevent mid-operation changes.
 *                    This ensures consistent behavior even if dev->fmt changes during processing.
 */
static void sc0710_dma_dequeue_video(struct sc0710_dma_channel *ch,
	struct sc0710_dma_descriptor_chain *chain,
	u32 cached_framesize,
	u32 cached_width,
	u32 cached_height,
	u32 cached_interlaced)
{
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_client *client;
	unsigned long flags;
	u32 source_framesize = cached_framesize;
	u32 source_w = cached_width, source_h = cached_height;
	int frame_gathered = 0;
	int delivered = 0;
	int stale_clients = 0;
	const u8 *woven_frame = NULL;
	u32 streak_required = dma_resync_tear_streak_required ?
		dma_resync_tear_streak_required : 1;

	if (cached_framesize == 0) {
		dprintk(1, "%s() no format detected, skipping\n", __func__);
		return;
	}

	if (ch->skip_next_frames > 0) {
		ch->skip_next_frames--;
		dprintk(1, "%s() post-restart skip (%u remaining)\n",
			__func__, ch->skip_next_frames);
		return;
	}

	/* Format negotiation refuses formats the field weave can't produce,
	 * but the source can go interlaced mid-session; drop rather than
	 * deliver scrambled frames. */
	if (cached_interlaced && !dev->pixfmt->weave_ok) {
		printk_ratelimited(KERN_WARNING "%s: interlaced signal with a capture format the field weave does not support, dropping frames\n",
			dev->name);
		return;
	}

	/* During resolution transitions the detected format (cached_framesize)
	 * may differ from the DMA chain allocation (total_transfer_size) because
	 * the channel cannot be resized while STATE_RUNNING.
	 * Delivering mis-sized raw data would corrupt the image, so drop;
	 * clients renegotiate on the V4L2_EVENT_SOURCE_CHANGE they were sent.
	 */
	if (chain->total_transfer_size > 0 &&
	    cached_framesize != (u32)chain->total_transfer_size) {
		dprintk(1, "%s() frame size mismatch (%u vs DMA %d) - dropping\n",
			__func__, cached_framesize, chain->total_transfer_size);
		return;
	}

	/* Pre-allocate staging buffer outside of spinlock context.
	 * vzalloc/vfree are sleeping calls that must not be called
	 * while holding spinlocks.  We size the buffer once here for
	 * the tear-validation and interlaced weaving paths.
	 */
	if ((cached_interlaced || ch->tear_validation_frames_left > 0) &&
	    (!dev->frame_staging_buf ||
	     dev->frame_staging_size < source_framesize)) {
		u8 *old = dev->frame_staging_buf;

		dev->frame_staging_buf = vzalloc(source_framesize);
		if (dev->frame_staging_buf) {
			dev->frame_staging_size = source_framesize;
		} else {
			dev->frame_staging_size = 0;
			printk_ratelimited(KERN_ERR "%s: Failed to allocate frame staging buffer (%u bytes)\n",
				dev->name, source_framesize);
		}
		if (old)
			vfree(old);
	}

	if (cached_interlaced &&
	    (!dev->weave_staging_buf ||
	     dev->weave_staging_size < source_framesize)) {
		u8 *old = dev->weave_staging_buf;

		dev->weave_staging_buf = vzalloc(source_framesize);
		if (dev->weave_staging_buf) {
			dev->weave_staging_size = source_framesize;
		} else {
			dev->weave_staging_size = 0;
			printk_ratelimited(KERN_ERR "%s: Failed to allocate weave staging buffer (%u bytes)\n",
				dev->name, source_framesize);
		}
		if (old)
			vfree(old);
	}

	/* Validate the first frames after resync and schedule a follow-up
	 * resync when a persistent tear seam is detected.
	 */
	if (ch->tear_validation_frames_left > 0) {
		if (dev->pixfmt->tear_ok &&
		    dev->frame_staging_buf &&
		    dev->frame_staging_size >= source_framesize) {
			int gathered = sc0710_dma_chain_dq_to_ptr(ch, chain,
				dev->frame_staging_buf, source_framesize);
			int tear_line = -1;
			bool tear_detected = false;

			if (gathered == (int)source_framesize) {
				frame_gathered = 1;
				tear_detected = sc0710_detect_horizontal_tear(
					dev->frame_staging_buf, source_w, source_h, &tear_line);
			}

			if (tear_detected) {
				bool near_same_line = (ch->tear_last_line >= 0) &&
					(abs(ch->tear_last_line - tear_line) <= 8);
				ch->tear_streak_count = near_same_line ?
					(ch->tear_streak_count + 1) : 1;
				ch->tear_last_line = tear_line;

				if (ch->tear_streak_count >= streak_required) {
					if (ch->tear_resync_retries_left > 0 && !dev->tear_resync_pending) {
						ch->tear_resync_retries_left--;
						dev->tear_resync_pending = 1;
						printk(KERN_WARNING
						       "%s: Tear seam persisted near line %d on channel %d; scheduling DMA re-resync (%u retries left)\n",
						       dev->name, tear_line, ch->nr,
						       ch->tear_resync_retries_left);
					}
					ch->tear_validation_frames_left = 0;
				}
			} else {
				ch->tear_streak_count = 0;
				ch->tear_last_line = -1;
			}

			ch->tear_validation_frames_left--;
		} else {
			/* Validation needs a contiguous frame buffer and the
			 * YUYV-only tear detector; disable if unavailable. */
			ch->tear_validation_frames_left = 0;
			ch->tear_streak_count = 0;
			ch->tear_last_line = -1;
		}
	}

	/* For interlaced content the hardware delivers two fields stacked
	 * vertically (Field 1 on top, Field 2 on bottom).  Weave them into
	 * a proper interleaved frame before delivering to clients.
	 */
	if (cached_interlaced && source_h >= 2 &&
	    dev->frame_staging_buf && dev->frame_staging_size >= source_framesize &&
	    dev->weave_staging_buf && dev->weave_staging_size >= source_framesize) {
		if (!frame_gathered) {
			int gathered = sc0710_dma_chain_dq_to_ptr(ch, chain,
				dev->frame_staging_buf, source_framesize);
			if (gathered == (int)source_framesize)
				frame_gathered = 1;
		}
		if (frame_gathered) {
			sc0710_weave_fields(dev->frame_staging_buf,
				dev->weave_staging_buf, source_w, source_h);
			woven_frame = dev->weave_staging_buf;
		}
	}

	/* Broadcast frame to all streaming clients */
	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_for_each_entry(client, &ch->client_list, list) {
		struct sc0710_buffer *vb_buf;
		unsigned long buf_flags;
		u8 *dst;
		unsigned long buffer_size;

		if (!client->streaming)
			continue;

		/* A client still locked to a different resolution gets nothing:
		 * it was told to renegotiate via V4L2_EVENT_SOURCE_CHANGE, and
		 * mis-sized raw delivery would corrupt the image. */
		if (client->stream_width && client->stream_height &&
		    (client->stream_width != source_w ||
		     client->stream_height != source_h)) {
			stale_clients++;
			continue;
		}

		spin_lock_irqsave(&client->buffer_lock, buf_flags);

		if (list_empty(&client->buffer_list)) {
			spin_unlock_irqrestore(&client->buffer_lock, buf_flags);
			continue; /* No buffer available for this client */
		}

		vb_buf = list_first_entry(&client->buffer_list, struct sc0710_buffer, list);
		dst = vb2_plane_vaddr(&vb_buf->vb.vb2_buf, 0);
		buffer_size = vb2_plane_size(&vb_buf->vb.vb2_buf, 0);

		if (!dst) {
			spin_unlock_irqrestore(&client->buffer_lock, buf_flags);
			continue;
		}

		if (woven_frame) {
			if (source_framesize <= buffer_size) {
				memcpy(dst, woven_frame, source_framesize);
				vb2_set_plane_payload(&vb_buf->vb.vb2_buf, 0, source_framesize);
			} else {
				memcpy(dst, woven_frame, buffer_size);
				vb2_set_plane_payload(&vb_buf->vb.vb2_buf, 0, buffer_size);
			}
		} else {
			int len;
			if (source_framesize > buffer_size) {
				len = sc0710_dma_chain_dq_to_ptr(ch, chain, dst, buffer_size);
				vb2_set_plane_payload(&vb_buf->vb.vb2_buf, 0, buffer_size);
			} else {
				len = sc0710_dma_chain_dq_to_ptr(ch, chain, dst, source_framesize);
				vb2_set_plane_payload(&vb_buf->vb.vb2_buf, 0, source_framesize);
			}
		}

		vb_buf->vb.vb2_buf.timestamp = ktime_get_ns();
		vb_buf->vb.sequence = ch->frame_sequence;
		vb_buf->vb.field = cached_interlaced ?
			V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;

		list_del(&vb_buf->list);
		vb2_buffer_done(&vb_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		delivered = 1;

		spin_unlock_irqrestore(&client->buffer_lock, buf_flags);
	}
	ch->frame_sequence++;
	spin_unlock_irqrestore(&ch->client_list_lock, flags);

	/* Count for the zero-copy split only when a client actually got the
	 * frame; drops and skips count in neither bucket. */
	if (zero_copy && delivered)
		ch->zc_frames_copied++;

	/* Re-set the buffer timeout. The placeholder deadline is pushed
	 * forward only when every streaming client got this frame: while any
	 * client is still locked to a stale resolution the pending timer is
	 * left alone, so it fires and keeps those clients fed with
	 * placeholders until they renegotiate (the timeout handler skips the
	 * live-resolution clients and re-arms itself). A dead timer (the
	 * resync stop deletes it) is still re-armed. */
	if ((delivered && !stale_clients) || !timer_pending(&ch->timeout))
		mod_timer(&ch->timeout, jiffies + VBUF_TIMEOUT);
}


/* Copy the contains of the audio chain into linux audio subsystem.
 */
static void sc0710_dma_dequeue_audio(struct sc0710_dma_channel *ch, struct sc0710_dma_descriptor_chain *chain)
{
	struct sc0710_dma_descriptor_chain_allocation *dca = &chain->allocations[0];
	int samplesPerChannel;
	int stride = 16;
	int ret;
	int i;

	if (chain->numAllocations != 1) {
		printk("%s() allocations should be one, dma issue?\n", __func__);
	}

	for (i = 0; i < chain->numAllocations; i++) {

		samplesPerChannel = dca->buf_size / stride;

		ret = sc0710_audio_deliver_samples(ch->dev, ch,
			(const u8 *)dca->buf_cpu,
			16,     /* bitwidth */
			stride,
			2,      /* channels */
			samplesPerChannel);

		dca++;
	}

}

/* For a given channel, audio or video, check if any of the writeback
 * descriptors have been set (indicating a complete transfer of audio or
 * video is complete. Process this transfered data into video or audio
 * frames.
 */
/* ---- Zero-copy chain targeting (zero_copy=1) --------------------------- */

struct sc0710_zc_piece {
	dma_addr_t addr;
	u32        len;
};

/* Split the buffer's mapped DMA segments into exactly numAllocations
 * contiguous pieces summing to the chain's transfer size, so the descriptor
 * count (and with it the next pointers and the one-time credit programming)
 * never changes when a chain is retargeted. Piece sizes are free - each
 * descriptor's length is rewritten anyway - so the frame's bytes in each
 * segment are tiled with that segment's share of the pieces; a piece only
 * must not span segments. Returns false when the buffer can't back this
 * chain (more frame-covering segments than descriptors). */
static bool sc0710_dma_chain_carve(const struct sc0710_dma_descriptor_chain *chain,
	const struct sc0710_buffer *buf,
	struct sc0710_zc_piece pieces[SC0710_MAX_CHAIN_DESCRIPTORS])
{
	u32 npieces = chain->numAllocations;
	u32 remaining = chain->total_transfer_size;
	u32 used[SC0710_MAX_CHAIN_DESCRIPTORS];
	u32 nused = 0;
	u32 i, j = 0;

	if (buf->zc_nsegs == 0 || remaining == 0)
		return false;

	/* The frame occupies the first total_transfer_size bytes; clip each
	 * segment to its frame-covering share. */
	for (i = 0; i < buf->zc_nsegs && remaining; i++) {
		used[nused] = min(buf->zc_seg[i].len, remaining);
		remaining -= used[nused];
		nused++;
	}
	if (remaining || nused > npieces)
		return false;

	for (i = 0; i < nused; i++) {
		/* Remaining pieces over remaining segments, front-loaded;
		 * every segment gets at least one. */
		u32 k = (npieces - j) - (nused - i - 1) * ((npieces - j) / (nused - i));
		u32 off = 0, chunk;

		while (k--) {
			chunk = k ? (used[i] - off) / (k + 1) : used[i] - off;
			if (chunk == 0)
				return false;
			pieces[j].addr = buf->zc_seg[i].addr + off;
			pieces[j].len  = chunk;
			off += chunk;
			j++;
		}
	}

	return j == npieces;
}

/* A descriptor's address must change in one store: the fetcher reads
 * descriptors independently of the completed count, and a torn fetch
 * mixing old and new address halves would DMA a frame to a wild address. */
static void sc0710_desc_set_dst(struct sc0710_dma_descriptor *desc, dma_addr_t addr)
{
	u64 *dst = (u64 *)(void *)&desc->dst_l;

	WRITE_ONCE(*dst, (u64)addr);
}

/* Same single-store rule for the writeback address. */
static void sc0710_desc_set_src(struct sc0710_dma_descriptor *desc, dma_addr_t addr)
{
	u64 *src = (u64 *)(void *)&desc->src_l;

	WRITE_ONCE(*src, (u64)addr);
}

/* Staleness sentinel: move every descriptor's writeback to the other half
 * of its slot. The device fetches a descriptor's writeback address together
 * with its data address, so after a rewrite, a writeback landing in the
 * retired half proves the device consumed a stale (pre-rewrite) descriptor
 * - the same stale copy whose data address is also old. Caller holds
 * ch->lock, with the chain's next lap not yet credit-granted. */
static void sc0710_dma_chain_flip_wbm(struct sc0710_dma_channel *ch,
	struct sc0710_dma_descriptor_chain *chain)
{
	u32 phase = chain->wbm_phase ^ 1;
	u32 j;

	for (j = 0; j < chain->numAllocations; j++) {
		struct sc0710_dma_descriptor_chain_allocation *dca =
			&chain->allocations[j];
		u32 *slot = dca->wbm_cpu;

		/* Retire the whole slot: the device writes back on EVERY
		 * descriptor, so the just-serviced lap left writebacks in the
		 * retiring half (only the last descriptor's are cleared by the
		 * service path) - residue that would read as staleness when
		 * this half is scanned after the next flip. */
		slot[0] = 0;
		slot[1] = 0;
		slot[2] = 0;
		slot[3] = 0;
		dca->wbm[0] = &slot[phase * 2];
		dca->wbm[1] = &slot[phase * 2 + 1];
		sc0710_desc_set_src(dca->desc, dca->wbm_dma + phase * 8);
	}
	/* Descriptor rewrites must be DMA-visible before the chain is granted again. */
	wmb();
	chain->wbm_phase = phase;
	ch->zc_wbm_flips++;
}

/* Count (and clear) writebacks that landed in the retired slot halves:
 * each is one descriptor the device fetched before the last rewrite. */
static u32 sc0710_dma_chain_stale_scan(struct sc0710_dma_channel *ch,
	struct sc0710_dma_descriptor_chain *chain)
{
	u32 old = chain->wbm_phase ^ 1;
	u32 stale = 0;
	u32 j;

	for (j = 0; j < chain->numAllocations; j++) {
		u32 *half = chain->allocations[j].wbm_cpu + old * 2;

		if (half[0] || half[1]) {
			half[0] = 0;
			half[1] = 0;
			stale++;
		}
	}
	if (stale) {
		ch->zc_stale_events++;
		ch->zc_stale_descs += stale;
	}
	return stale;
}

/* Point the chain's descriptors back at its own coherent allocations.
 * Caller holds ch->lock and has verified the engine can't be fetching
 * these descriptors (chain just completed, or engine stopped). */
static void sc0710_dma_chain_target_scratch(struct sc0710_dma_descriptor_chain *chain)
{
	u32 j;

	for (j = 0; j < chain->numAllocations; j++) {
		struct sc0710_dma_descriptor_chain_allocation *dca =
			&chain->allocations[j];

		dca->desc->lengthBytes = dca->buf_size;
		sc0710_desc_set_dst(dca->desc, dca->buf_dma);
	}
	/* Descriptor rewrites must be DMA-visible before the chain is granted again. */
	wmb();
	chain->target_buf = NULL;
	chain->target_client = NULL;
}

/* Take the client's next queued buffer and aim the chain's descriptors at
 * it. On any failure the buffer returns to the head of the queue and the
 * chain keeps its current (scratch) target. Caller holds ch->lock and the
 * chain must be untargeted. Rewriting is safe here because the fetcher is
 * credit-gated: this chain's next lap is granted only after the service
 * pass's rewrites have landed, so the engine can't be fetching them. */
static void sc0710_dma_chain_try_target(struct sc0710_dma_channel *ch, int chain_idx,
	struct sc0710_client *client)
{
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_dma_descriptor_chain *chain = &ch->chains[chain_idx];
	struct sc0710_zc_piece pieces[SC0710_MAX_CHAIN_DESCRIPTORS];
	struct sc0710_buffer *buf = NULL;
	struct sc0710_dma_descriptor *desc;
	unsigned long flags;
	u32 j;

	if (chain->target_buf)
		return;

	spin_lock_irqsave(&client->buffer_lock, flags);
	if (!list_empty(&client->buffer_list)) {
		buf = list_first_entry(&client->buffer_list, struct sc0710_buffer, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&client->buffer_lock, flags);
	if (!buf)
		return;

	if (!sc0710_dma_chain_carve(chain, buf, pieces)) {
		dprintk(2, "%s() carve failed (%u segs for %u pieces), copy path\n",
			__func__, buf->zc_nsegs, chain->numAllocations);
		spin_lock_irqsave(&client->buffer_lock, flags);
		list_add(&buf->list, &client->buffer_list);
		spin_unlock_irqrestore(&client->buffer_lock, flags);
		return;
	}

	for (j = 0; j < chain->numAllocations; j++) {
		desc = chain->allocations[j].desc;
		desc->lengthBytes = pieces[j].len;
		sc0710_desc_set_dst(desc, pieces[j].addr);
	}
	/* Descriptor rewrites must be DMA-visible before the chain is granted again. */
	wmb();
	chain->target_buf = buf;
	chain->target_client = client;
}

/* Deliver a frame the hardware already placed in the targeted buffer. */
static void sc0710_dma_deliver_targeted(struct sc0710_dma_channel *ch,
	struct sc0710_dma_descriptor_chain *chain)
{
	struct sc0710_buffer *buf = chain->target_buf;

	/* Point the chain back at scratch before handing the buffer over:
	 * whether or not a new buffer gets targeted afterwards, the chain
	 * must never keep DMA-targeting a buffer userspace owns. Safe to
	 * rewrite here: the chain's next lap is not yet credit-granted. */
	sc0710_dma_chain_target_scratch(chain);

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, chain->total_transfer_size);
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = ch->frame_sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	ch->zc_frames_direct++;

	/* Re-set the buffer timeout */
	mod_timer(&ch->timeout, jiffies + VBUF_TIMEOUT);
}

/* Return an undelivered targeted buffer to the head of its client's queue
 * and point the chain back at scratch. Caller holds ch->lock and the
 * engine must be quiesced (or the chain just completed). */
static void sc0710_dma_chain_untarget(struct sc0710_dma_descriptor_chain *chain)
{
	struct sc0710_buffer *buf = chain->target_buf;
	struct sc0710_client *client = chain->target_client;
	unsigned long flags;

	if (!buf)
		return;

	sc0710_dma_chain_target_scratch(chain);

	spin_lock_irqsave(&client->buffer_lock, flags);
	list_add(&buf->list, &client->buffer_list);
	spin_unlock_irqrestore(&client->buffer_lock, flags);
}

/* Point every chain back at the scratch ring and requeue targeted buffers.
 * Callers hold kthread_dma_lock or run after the DMA thread has stopped
 * (PCI remove), with the XDMA engine stopped and drained, ahead of anything
 * that frees chains or unmaps client buffers. */
void sc0710_dma_channel_untarget_all(struct sc0710_dma_channel *ch)
{
	int i;

	mutex_lock(&ch->lock);
	for (i = 0; i < ch->numDescriptorChains; i++)
		sc0710_dma_chain_untarget(&ch->chains[i]);
	mutex_unlock(&ch->lock);
}

/* Zero-copy is engaged per completed chain, and only when the whole frame
 * can land in the client's buffer exactly as the source produced it: one
 * streaming client, progressive, client negotiated the source format, no
 * post-resync frame dropping and no tear-validation window (both read
 * frames from the scratch ring). Returns the client, or NULL for the copy
 * path. Caller holds ch->lock. */
static struct sc0710_client *sc0710_dma_zc_client(struct sc0710_dma_channel *ch,
	u32 cached_framesize, u32 cached_width, u32 cached_height,
	u32 cached_interlaced)
{
	struct sc0710_client *client, *zc_client = NULL;
	unsigned long flags;
	int streaming = 0;

	if (!zero_copy || ch->mediatype != CHTYPE_VIDEO)
		return NULL;
	if (ch->zc_stale_trip)
		return NULL;
	if (cached_framesize == 0 || cached_interlaced)
		return NULL;
	if (ch->skip_next_frames || ch->tear_validation_frames_left)
		return NULL;

	spin_lock_irqsave(&ch->client_list_lock, flags);
	list_for_each_entry(client, &ch->client_list, list) {
		if (!client->streaming)
			continue;
		streaming++;
		zc_client = client;
	}
	spin_unlock_irqrestore(&ch->client_list_lock, flags);

	if (streaming != 1 ||
	    zc_client->stream_width != cached_width ||
	    zc_client->stream_height != cached_height ||
	    zc_client->stream_framesize != cached_framesize)
		return NULL;

	return zc_client;
}

int sc0710_dma_channel_service(struct sc0710_dma_channel *ch)
{
	struct sc0710_dev *dev = ch->dev;
	struct sc0710_dma_descriptor_chain_allocation *dca;
	struct sc0710_dma_descriptor_chain *chain;
	const struct sc0710_format *cached_fmt;
	struct sc0710_client *zc_client;
	u32 cached_framesize;
	u32 cached_width, cached_height;
	u32 cached_interlaced;
	u32 wbm[2];
	u32 v;
	bool sentinel;
	bool stale_completion;
	int consumed = 0;
	int i;

	mutex_lock(&ch->lock);

	if (ch->enabled == 0) {
		mutex_unlock(&ch->lock);
		return -1;
	}

	/* Only service the channel if it is actively running. 
	 * This prevents race conditions during resize/allocation 
	 * where chains might be invalid but HW counters have changed.
	 */
	if (ch->state != STATE_RUNNING) {
		mutex_unlock(&ch->lock);
		return 0;
	}

	cached_fmt = READ_ONCE(dev->fmt);
	cached_framesize = sc0710_framesize(dev, cached_fmt);
	cached_width = cached_fmt ? cached_fmt->width : 0;
	cached_height = cached_fmt ? cached_fmt->height : 0;
	cached_interlaced = cached_fmt ? cached_fmt->interlaced : 0;

	/* Read how many descriptors have complete, if this hasn't changed
	 * single we last checked, end early, nothing for us to do.
	 */
	v = sc_read(ch->dev, 1, ch->reg_dma_completed_descriptor_count);
	if (v == ch->dma_completed_descriptor_count_last) {
		/* No new buffers since our last service call. */
		mutex_unlock(&ch->lock);
		return 0;
	}

	dprintk(3, "ch#%d    was %d now %d\n", ch->nr, ch->dma_completed_descriptor_count_last, v);
	ch->dma_completed_descriptor_count_last = v;
	ch->dma_last_completion_jiffies = jiffies;

	zc_client = sc0710_dma_zc_client(ch, cached_framesize,
		cached_width, cached_height, cached_interlaced);

	sentinel = zero_copy && ch->mediatype == CHTYPE_VIDEO;

	for (i = 0; i < ch->numDescriptorChains; i++) {
		chain = &ch->chains[i];

		/* An empty chain (failed or partial allocation) has no last buffer;
		 * numAllocations is u32, so [numAllocations - 1] would wrap to a
		 * wild index. */
		if (chain->numAllocations == 0)
			continue;

		/* Last allocated SG buffer in the chain. */
		dca = &chain->allocations[ chain->numAllocations - 1 ];

		/* Read memory barrier to ensure we see the latest writeback metadata
		 * values after any DMA or CPU writes.
		 */
		rmb();

		/* Read the writeback metadata once, cache it locally. */
		wbm[0] = *dca->wbm[0];
		wbm[1] = *dca->wbm[1];

		/* With the sentinel armed, a lap whose last descriptor was
		 * consumed stale completes into the retired slot half. */
		stale_completion = false;
		if (sentinel && !(wbm[0] && wbm[1])) {
			u32 *half = dca->wbm_cpu + (chain->wbm_phase ^ 1) * 2;

			wbm[0] = half[0];
			wbm[1] = half[1];
			stale_completion = wbm[0] && wbm[1];
		}

		/* If the write back metadata is set, we know the chain is complete, we'll
		 * need to process a complete video/audio transfer.
		 */
		if (wbm[0] && wbm[1]) {
			if (sentinel && sc0710_dma_chain_stale_scan(ch, chain) &&
			    !ch->zc_stale_trip) {
				/* The device consumed a pre-rewrite descriptor:
				 * its read-ahead outran the credit gating, and a
				 * frame may already have landed in a delivered
				 * buffer. Permanently force the copy path; the
				 * resync rebuilds the ring with the engine
				 * stopped and returns held buffers. */
				ch->zc_stale_trip = true;
				dev->tear_resync_pending = 1;
				zc_client = NULL;
				printk(KERN_ERR "%s: [ch%d] zero-copy: stale descriptor "
					"writeback detected - a delivered frame may have been "
					"overwritten; disabling zero-copy targeting and "
					"scheduling a DMA resync\n", dev->name, ch->nr);
			}

			/* A tripped sentinel holds a targeted chain untouched
			 * (writeback included): the buffer is still
			 * driver-owned, and the resync returns it with the
			 * engine stopped. */
			if (chain->target_buf && ch->zc_stale_trip)
				continue;

			consumed++;

			if (sc0710_debug_mode > 2) {
				printk("%s ch#%d    [%02d] %08x - wbm %08x %08x (DQ) segs: %d\n",
					ch->dev->name,
					ch->nr,
					i,
					dca->desc->control,
					wbm[0],
					wbm[1], chain->numAllocations);
			}

			/* Update some internal stats that measure throughput. */
			sc0710_things_per_second_update(&ch->bitsPerSecond, chain->total_transfer_size * 8);
			sc0710_things_per_second_update(&ch->descPerSecond, chain->numAllocations);

			/* Service the audio, or video.
			 * Pass cached_framesize to prevent mid-operation format changes.
			 * A completion seen only through the retired writeback half
			 * came from a pre-rewrite descriptor: this lap's data went
			 * to that descriptor's old address, not the scratch ring,
			 * so there is no frame to deliver - housekeeping still runs
			 * below so the ring keeps turning until the resync. */
			if (ch->mediatype == CHTYPE_VIDEO && !stale_completion) {
				if (chain->target_buf) {
					sc0710_dma_deliver_targeted(ch, chain);
				} else {
					sc0710_dma_dequeue_video(ch, chain, cached_framesize,
						cached_width, cached_height,
						cached_interlaced);
				}
			} else
			if (ch->mediatype == CHTYPE_AUDIO) {
				sc0710_dma_dequeue_audio(ch, chain);
			}

			/* Reset the descriptor state so we know when it's complete next time. */
			*(dca->wbm[0]) = 0;
			*(dca->wbm[1]) = 0;

			/* Write memory barrier to ensure metadata clear is visible
			 * before any subsequent operations.
			 */
			wmb();

			/* Arm the sentinel for the next lap alongside whatever
			 * rewrite this pass makes (scratch re-point, retarget, or
			 * the flip itself in measurement mode). */
			if (sentinel && !ch->zc_stale_trip)
				sc0710_dma_chain_flip_wbm(ch, chain);

			/* Re-arm this chain for the next lap. */
			if (zc_client &&
			    (u32)chain->total_transfer_size == cached_framesize)
				sc0710_dma_chain_try_target(ch, i, zc_client);

			/* Credit-gated fetching (zero_copy): fund this chain's
			 * next lap now that every rewrite of this pass has
			 * landed - the fetcher can never read a stale
			 * descriptor. Unconditional per consumed completion:
			 * copy-path and skipped frames use ring credits too. */
			if (zero_copy && ch->mediatype == CHTYPE_VIDEO)
				sc_write(ch->dev, 1, ch->reg_sg_credits,
					chain->numAllocations);
		}
	}

	mutex_unlock(&ch->lock);
	return consumed;
}

/* Build the scatter gather table chaining all of the chains and decriptors together. */
static int sc0710_dma_channel_chains_link(struct sc0710_dma_channel *ch)
{
	struct sc0710_dma_descriptor_chain *chain;
	struct sc0710_dma_descriptor_chain_allocation *dca;
	struct sc0710_dma_descriptor *pt_desc = (struct sc0710_dma_descriptor *)ch->pt_cpu;
	dma_addr_t curr_tbl = ch->pt_dma;
	dma_addr_t curr_wbm = ch->pt_dma + PAGE_SIZE;
	/* Virtual address for writeback metadata - second page of coherent allocation */
	u8 *wbm_cpu = (u8 *)ch->pt_cpu + PAGE_SIZE;
	int i, j;

	/* Both pt pages bound the ring: page 1 holds the 32-byte descriptors,
	 * page 2 the writeback slots at the same stride. */
	BUILD_BUG_ON(SC0710_MAX_CHANNEL_DESCRIPTOR_CHAINS * SC0710_MAX_CHAIN_DESCRIPTORS *
		     sizeof(struct sc0710_dma_descriptor) > PAGE_SIZE);

	/* Now that we have all of the dma allocations, we can update the descriptor tables with DMA io addresses. */
	for (i = 0; i < ch->numDescriptorChains; i++) {
		chain = &ch->chains[i];

		/* A (re)built ring always starts on the scratch allocations,
		 * with the writeback ping-pong on its first half. */
		chain->target_buf = NULL;
		chain->target_client = NULL;
		chain->wbm_phase = 0;

		for (j = 0; j < chain->numAllocations; j++) {
			dca = &chain->allocations[j];

			dca->desc = pt_desc++;

			if ((i + 1 == ch->numDescriptorChains) && (j + 1 == chain->numAllocations)) {
				/* Last descriptor in the last chains needs to point to the
				 * first desc in first chain. */
				dca->desc->next_l = (u64)ch->pt_dma;
				dca->desc->next_h = (u64)ch->pt_dma >> 32;
			} else {
				/* Point to the next descriptor in the chain. */
				dca->desc->next_l = (u64)curr_tbl + sizeof(struct sc0710_dma_descriptor);
				dca->desc->next_h = ((u64)curr_tbl + sizeof(struct sc0710_dma_descriptor)) >> 32;
			}

			/* The Completed control bit - which the vendor never uses -
			 * makes the engine raise a completion event (with the IRQ
			 * block armed, an interrupt) for the descriptor. The
			 * interrupt-driven service wants one per chain, so the
			 * last descriptor carries it. */
			dca->desc->control     = 0xAD4B0000;
			if (ch->dev->irq_service_active &&
			    j + 1 == chain->numAllocations)
				dca->desc->control |= 0x02;
			dca->desc->lengthBytes = dca->buf_size;
			dca->desc->src_l       = (u64)curr_wbm;
			dca->desc->src_h       = (u64)curr_wbm >> 32;
			dca->desc->dst_l       = (u64)dca->buf_dma;
			dca->desc->dst_h       = (u64)dca->buf_dma >> 32;

			/* Use CPU virtual address calculated from coherent allocation */
			dca->wbm[0] = (u32 *)wbm_cpu;
			dca->wbm[1] = (u32 *)(wbm_cpu + sizeof(u32));
			dca->wbm_cpu = (u32 *)wbm_cpu;
			dca->wbm_dma = curr_wbm;

			curr_tbl += sizeof(struct sc0710_dma_descriptor);
			curr_wbm += sizeof(struct sc0710_dma_descriptor);
			wbm_cpu += sizeof(struct sc0710_dma_descriptor);
		} /* for all allocations in a chain */
	} /* for all chains */

	return 0; /* Success */
}

int sc0710_dma_channel_alloc(struct sc0710_dev *dev, u32 nr, enum sc0710_channel_dir_e direction,
	u32 baseaddr,
	enum sc0710_channel_type_e mediatype)
{
	int ret;
	struct sc0710_dma_channel *ch = &dev->channel[nr];
	if (nr >= SC0710_MAX_CHANNELS)
		return -EINVAL;

	if (direction != CHDIR_INPUT)
		return -EINVAL;

	memset(ch, 0, sizeof(*ch));
	mutex_init(&ch->lock);
	mutex_init(&ch->v4l2_lock);

	/* Multi-client streaming support initialization */
	atomic_set(&ch->streaming_refcount, 0);
	INIT_LIST_HEAD(&ch->client_list);
	spin_lock_init(&ch->client_list_lock);

	spin_lock_init(&ch->v4l2_capture_list_lock);
	INIT_LIST_HEAD(&ch->v4l2_capture_list);

	ch->dev = dev;
	ch->nr = nr;
	ch->enabled = 1;
	ch->direction = direction;
	ch->mediatype = mediatype;
	ch->state = STATE_STOPPED;
	ch->dma_last_completion_jiffies = 0;
	sc0710_things_per_second_reset(&ch->bitsPerSecond);
	sc0710_things_per_second_reset(&ch->descPerSecond);
	sc0710_things_per_second_reset(&ch->audioSamplesPerSecond);

	if (ch->mediatype == CHTYPE_VIDEO) {
		ch->numDescriptorChains = DMA_TRANSFER_CHAINS;
		/* 1280x 720p - default sizing during initialization.
		 * we'll free and re-alloc up or down prior to streaming.
		 */
		ch->buf_size = 1280 * 2 * 720; /* 16bit pixels for everything. */
		if (sc0710_debug_mode)
			printk("Allocating channel for size %d\n", ch->buf_size);
	} else
	if (ch->mediatype == CHTYPE_AUDIO) {
		ch->numDescriptorChains = DMA_TRANSFER_CHAINS;
		ch->buf_size = DMA_AUDIO_TRANSFER_SIZE;
	} else {
		ch->numDescriptorChains = 0;
	}

	/* Page table defaults. */
	/* Page table defaults. */
	/* This assumed PAGE_SIZE is 4K */
	/* allocate the descriptor table, its contigious. */
	ch->pt_size = PAGE_SIZE * 2;

	ch->pt_cpu = dma_alloc_coherent(&dev->pci->dev, ch->pt_size, &ch->pt_dma, GFP_KERNEL);
	if (ch->pt_cpu == 0) {
		ch->enabled = 0;
		return -ENOMEM;
	}

	memset(ch->pt_cpu, 0, ch->pt_size);

	/* register offsets use by the channel and dma descriptor register writes/reads. */

	/* Configure this channel object dma controller registers, so we know how to control
	 * and program the hardware channel.
	 */

	/* DMA controller */
	ch->register_dma_base = baseaddr;
	ch->reg_dma_control = ch->register_dma_base + 0x04;
	ch->reg_dma_control_w1s = ch->register_dma_base + 0x08;
	ch->reg_dma_control_w1c = ch->register_dma_base + 0x0c;
	ch->reg_dma_status1 = ch->register_dma_base + 0x40;
	ch->reg_dma_status2 = ch->register_dma_base + 0x44;
	ch->reg_dma_completed_descriptor_count = ch->register_dma_base + 0x48;
	ch->reg_dma_poll_wba_l = ch->register_dma_base + 0x88;
	ch->reg_dma_poll_wba_h = ch->register_dma_base + 0x8c;

	/* Configure this channel object scatter gather  controller registers,
	 * so we know how to control and program the hardware channel.
	 */

	/* SGDMA Controller */
	ch->register_sg_base = baseaddr + 0x4000;
        ch->reg_sg_start_l = ch->register_sg_base + 0x80;
        ch->reg_sg_start_h = ch->register_sg_base + 0x84;
        ch->reg_sg_adj = ch->register_sg_base + 0x88;
        ch->reg_sg_credits = ch->register_sg_base + 0x8c;

	/* Allocate all the DMA buffers for this channel. */
	ret = sc0710_dma_chains_alloc(ch, ch->buf_size);
	if (ret < 0) {
		sc0710_dma_chains_free(ch);
		ch->enabled = 0;
		printk(KERN_ERR "%s: channel %d DMA allocation failed (%d)\n",
			dev->name, nr, ret);
		return ret;
	}

	/* Adjust the descriptor chains to correctly reference each other */
	sc0710_dma_channel_chains_link(ch);

	if (sc0710_debug_mode) {
		printk(KERN_INFO "%s channel %d allocated\n", dev->name, nr);
		/* Print the complete chain, descriptor, allocation configuration to the console. */
		sc0710_dma_chains_dump(ch);
	}

	/* The V4L2/ALSA device nodes are registered by the probe once nothing
	 * else can fail; an open node must never outlive its dev. */

	return 0; /* Success */
};

/* adjust the DMA subsystem transfer_size to match the video frame size
 * we've detected from the HDMI receiver.
 * this is called when the user first asks video streaming to be started,
 * and we've detected video in the HDMI receiver and understand what
 * DMA transfer sizes will be needed for a single video frame.
 */
int sc0710_dma_channel_resize(struct sc0710_dev *dev, u32 nr, enum sc0710_channel_dir_e direction,
	u32 baseaddr,
	enum sc0710_channel_type_e mediatype)
{
	struct sc0710_dma_channel *ch = &dev->channel[nr];
	int ret;
	if (nr >= SC0710_MAX_CHANNELS)
		return -EINVAL;

	if (!dev->fmt) {
		return -EINVAL;
	}

	/* Safety: Do not resize active channels (especially Audio) */
	if (ch->state == STATE_RUNNING) {
		printk(KERN_WARNING "%s: Attempt to resize running channel %d! Skipping.\n", __func__, nr);
		return 0;
	}

	sc0710_dma_chains_free(ch);

	printk(KERN_INFO "%s channel %d resized for framesize %d\n",
		dev->name, nr, sc0710_framesize(dev, dev->fmt));

	if (ch->mediatype == CHTYPE_VIDEO) {
		ch->numDescriptorChains = DMA_TRANSFER_CHAINS;
		/* When processing starts, tear down the current DMA allocations and
		 * create new DMA allocation sizes suitable for the detect video frame
		 * size, which could be much larger or smaller than any previous allocation.
		 * Video transfers vary and need adjustment.
		 */
		ch->buf_size = sc0710_framesize(dev, dev->fmt);
		if (sc0710_debug_mode)
			printk("Resizing channel for size %d\n", ch->buf_size);
	} else
	if (ch->mediatype == CHTYPE_AUDIO) {
		/* Audio always uses a fixed transfer size */
		ch->numDescriptorChains = DMA_TRANSFER_CHAINS;
		ch->buf_size = DMA_AUDIO_TRANSFER_SIZE;
	} else {
		return -EINVAL;
	}

	/* Page table defaults. */
	/* This assumed PAGE_SIZE is 4K */
	/* allocate the descriptor table, its contigious. */
	ch->pt_size = PAGE_SIZE * 2;

	ch->pt_cpu = dma_alloc_coherent(&dev->pci->dev, ch->pt_size, &ch->pt_dma, GFP_KERNEL);
	if (ch->pt_cpu == 0) {
		printk(KERN_ERR "%s: channel %d DMA resize failed (page table)\n",
			dev->name, nr);
		return -ENOMEM;
	}

	memset(ch->pt_cpu, 0, ch->pt_size);

	/* allocate DMA based on ch->buf_size */
	ret = sc0710_dma_chains_alloc(ch, ch->buf_size);
	if (ret < 0) {
		sc0710_dma_chains_free(ch);
		printk(KERN_ERR "%s: channel %d DMA resize failed (%d); channel unusable until the next resize\n",
			dev->name, nr, ret);
		return ret;
	}

	sc0710_dma_channel_chains_link(ch);

	if (sc0710_debug_mode) {
		printk(KERN_INFO "%s channel %d allocated\n", dev->name, nr);
		sc0710_dma_chains_dump(ch);
	}

	return 0; /* Success */
};

void sc0710_dma_channel_free(struct sc0710_dev *dev, u32 nr)
{
	struct sc0710_dma_channel *ch = &dev->channel[nr];
	if (nr >= SC0710_MAX_CHANNELS)
		return;

	if (ch->enabled == 0)
		return;

	ch->enabled = 0;

	/* The V4L2/ALSA nodes are taken down by the remove path before any
	 * hardware teardown; this frees DMA resources only. */
	sc0710_dma_chains_free(ch);

	if (sc0710_debug_mode)
		printk(KERN_INFO "%s channel %d deallocated\n", dev->name, nr);
}

/* Prepare the DMA and SG hardware. Reset, establish their
 * first descriptor to process. The hardware itself is started
 * later.
 */
int sc0710_dma_channel_start_prep(struct sc0710_dma_channel *ch)
{
	int i, total_descriptors = 0;

	if (ch->state == STATE_RUNNING)
		return 0;

	/* A channel whose ring failed to (re)allocate must never reach the
	 * hardware; pt_dma would be 0. */
	if (!ch->pt_cpu)
		return -ENOMEM;

	/* Stop engine first */
	sc_write(ch->dev, 1, ch->reg_dma_control_w1c, 0x00000001);

	/* Full XDMA engine reset (Xilinx reference sequence).
	 * Without this, the engine may be wedged from a previous load.
	 */
	sc_write(ch->dev, 1, ch->reg_dma_control_w1s, 0x01000000); /* Assert reset (bit 24) */
	mdelay(1);
	sc_write(ch->dev, 1, ch->reg_dma_control_w1c, 0x01000001); /* Clear reset + run */

	/* Re-apply IE bits after reset cleared the control register */
	sc_write(ch->dev, 1, ch->reg_dma_control_w1s, 0x00fffe7e);
	/* Re-apply interrupt enable register */
	sc_write(ch->dev, 1, ch->register_dma_base + 0x94, 0x00fffe7e);

	ch->dma_completed_descriptor_count_last = 0;
	sc_write(ch->dev, 1, ch->reg_dma_completed_descriptor_count, 0);
	sc_write(ch->dev, 1, ch->reg_sg_start_h, ch->pt_dma >> 32);
	sc_write(ch->dev, 1, ch->reg_sg_start_l, ch->pt_dma);
	sc_write(ch->dev, 1, ch->reg_sg_adj, 0);

	/* Count total descriptors for SG credits (written after run bit in start) */
	for (i = 0; i < ch->numDescriptorChains; i++)
		total_descriptors += ch->chains[i].numAllocations;
	ch->sg_total_descriptors = total_descriptors;

	/* The writeback ping-pong restarts on clean first halves: residue
	 * from a previous session must not read as completion or staleness. */
	for (i = 0; i < ch->numDescriptorChains; i++) {
		struct sc0710_dma_descriptor_chain *chain = &ch->chains[i];
		u32 j;

		chain->wbm_phase = 0;
		for (j = 0; j < chain->numAllocations; j++) {
			struct sc0710_dma_descriptor_chain_allocation *dca =
				&chain->allocations[j];

			memset(dca->wbm_cpu, 0, 4 * sizeof(u32));
			dca->wbm[0] = &dca->wbm_cpu[0];
			dca->wbm[1] = &dca->wbm_cpu[1];
			sc0710_desc_set_src(dca->desc, dca->wbm_dma);
		}
	}

	return 0;
}

/* Wait for the engine to finish in-flight work after the run bit is
 * cleared: the XDMA engine drops the status Busy bit once its descriptor
 * fetches and payload writes have drained. Zero-copy needs this before
 * descriptors are rewritten or client buffers handed back; bounded, with
 * a settle delay as the fallback if Busy never clears. */
static void sc0710_dma_channel_quiesce(struct sc0710_dma_channel *ch)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (!(sc_read(ch->dev, 1, ch->reg_dma_status1) & 0x01))
			return;
		usleep_range(100, 200);
	}
	printk(KERN_WARNING "%s: [ch%d] DMA engine still busy after stop, settling blind\n",
		ch->dev->name, ch->nr);
	usleep_range(5000, 6000);
}

/* Stop the hardware, stop all DMA activity. */
int sc0710_dma_channel_stop(struct sc0710_dma_channel *ch)
{
	/* Serialize against the service thread: a service pass that
	 * already passed the state check must finish (and do its last
	 * mod_timer) before we flip to STOPPED, so the caller's subsequent
	 * timer_delete_sync() is final. */
	mutex_lock(&ch->lock);
	sc_write(ch->dev, 1, ch->reg_dma_control_w1c, 0x00000001);
	if (zero_copy && ch->mediatype == CHTYPE_VIDEO)
		sc0710_dma_channel_quiesce(ch);
	sc0710_things_per_second_reset(&ch->bitsPerSecond);
	sc0710_things_per_second_reset(&ch->descPerSecond);
	ch->state = STATE_STOPPED;
	ch->dma_last_completion_jiffies = 0;
	mutex_unlock(&ch->lock);
	return 0;
}

/* Start the hardware, it was pre-programmed in the start_prep() function,
 * so all we have to do is flip a bit to enable it and video/audio
 * dma transfers will happen immediately.
 */
int sc0710_dma_channel_start(struct sc0710_dma_channel *ch)
{
	if (ch->state == STATE_RUNNING)
		return 0;

	/* Zero-copy retargets descriptors between ring laps, and the fetcher
	 * prefetches the entire ring - so it is gated with the SGDMA common
	 * block's credit mode (0x6020, W1S 0x6024 / W1C 0x6028; C2H channels
	 * are bits 16+): the fetcher only reads descriptors it has been
	 * granted, and a chain's next lap is granted only after its rewrites
	 * (see the service loop). Set or clear the enable explicitly so a
	 * reload without zero-copy never inherits it. */
	if (ch->mediatype == CHTYPE_VIDEO)
		sc_write(ch->dev, 1, zero_copy ? 0x6024 : 0x6028, 0x00010000);

	/* Engine-level interrupt enables are already set at prep, but the
	 * vendor never arms the IRQ block's channel mask (0x2010, W1S 0x2014
	 * / W1C 0x2018) or the vector mapping (0x20a0/0x20a4). Arm the block
	 * for the interrupt-driven service; clear it otherwise so reloads
	 * never inherit it. */
	if (ch->mediatype == CHTYPE_VIDEO) {
		if (ch->dev->irq_service_active) {
			sc_write(ch->dev, 1, 0x20a0, 0);
			sc_write(ch->dev, 1, 0x20a4, 0);
			sc_write(ch->dev, 1, 0x2014, 0xff);
		} else {
			sc_write(ch->dev, 1, 0x2018, 0xff);
		}
	}

	sc_write(ch->dev, 1, ch->reg_dma_control_w1s, 0x00000001);

	/* Write SG credits after run bit is set.
	 * The XDMA SG fetcher requires the engine to be running
	 * before credits are accepted.
	 * With credit mode enabled (zero_copy) this funds exactly one ring
	 * lap and the service loop funds each following lap chain by chain;
	 * with the enable clear (the default) the write is inert and the
	 * ring free-runs.
	 */
	sc_write(ch->dev, 1, ch->reg_sg_credits, ch->sg_total_descriptors);

	ch->state = STATE_RUNNING;
	ch->dma_last_completion_jiffies = jiffies;
	return 0;
}

enum sc0710_channel_state_e sc0710_dma_channel_state(struct sc0710_dma_channel *ch)
{
	return ch->state;
}
