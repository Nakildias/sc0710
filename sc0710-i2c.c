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
#include <linux/delay.h>
#include <asm/io.h>

#include "sc0710.h"

#define I2C_DEV__ARM_MCU (0x32 << 1)
#define I2C_DEV__UNKNOWN (0x33 << 1)

#define SC0710_I2C_HDMI_RETRIES 3
#define SC0710_I2C_HDMI_RETRY_DELAY_US 50000
#define SC0710_LOCK_DROPOUT_MAX 5
#define SC0710_NO_TIMING_THRESHOLD 6

#if 1 /* Enable I2C write for tone mapping control */
static int didack(struct sc0710_dev *dev)
{
	u32 v;
	int cnt = 16;

	while (cnt-- > 0) {
		v = sc_read(dev, 0, BAR0_3104);
		/* TX_FIFO_Empty (bit 7) = data consumed/sent.
		 * BB (bit 2) = bus busy, slave ACK'd.
		 * Either condition indicates successful transmission.
		 * Known values: 0x44 (MK2), 0xC0, 0xC4 (4K Pro after soft reset).
		 */
		if ((v & 0x80) || (v & 0x04))
			return 1; /* device Ack'd */
		udelay(64);
	}

	return 0; /* No Ack */
}
#endif

static u8 busread(struct sc0710_dev *dev)
{
	u32 v;
	int cnt = 32;
	unsigned long timeout = jiffies + msecs_to_jiffies(100); /* 100ms max */

	while (cnt-- > 0) {
		/* Check for global timeout to prevent infinite loop */
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: busread timeout\n", __func__);
			return 0xFF; /* Return error value */
		}

		v = sc_read(dev, 0, BAR0_3104);
//printk("readbus %08x\n", v);
		/* Wait for RX data: RX_FIFO_Empty (bit 6) clear means data available.
		 * MK2 sees 0x8C/0xAC; 4K Pro may differ in bus-busy/SRW bits.
		 */
		if (!(v & 0x40)) /* RX_FIFO not empty — data ready */
			break;
		udelay(100);
	}

	v = sc_read(dev, 0, BAR0_310C);
//printk("readbus ret 0x%02x\n", v);
	return v;
}

#if 1 /* Enable I2C write for MCU commands */
/* Assumes 8 bit device address and 8 bit sub address. */
static int sc0710_i2c_write(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen)
{
	int i;
	u32 v;

	/* Write out to the i2c bus master a reset, then write length and device address */
	sc_write(dev, 0, BAR0_3100, 0x00000002); /* TX_FIFO Reset */
	sc_write(dev, 0, BAR0_3100, 0x00000001); /* AXI IIC Enable */
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 8) /* Start Bit */ | devaddr8bit);

	/* Wait for the device ack */
	if (didack(dev) == 0)
		return -EIO;

	for (i = 1; i < wlen; i++) {
		v = 0x00000000 | *(wbuf + i);
		if (i == (wlen - 1))
			v |= (1 << 9); /* Stop Bit */
		sc_write(dev, 0, BAR0_3108, v);
		if (didack(dev) == 0)
			return -EIO;
	}

	return 0; /* Success */
}
#endif

/* Public I2C write function */
int sc0710_i2c_write_mcu(struct sc0710_dev *dev, u8 subaddr, u8 *data, int len)
{
	u8 wbuf[16];
	int ret;

	if (len > 15)
		return -EINVAL;

	wbuf[0] = subaddr;
	memcpy(&wbuf[1], data, len);

	mutex_lock(&dev->signalMutex);
	ret = sc0710_i2c_write(dev, I2C_DEV__ARM_MCU, wbuf, len + 1);
	mutex_unlock(&dev->signalMutex);

	return ret;
}

static int __sc0710_i2c_writeread(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	u32 v;
	u8 i2c_devaddr = devaddr8bit; /* From dev 64, read 0x1a bytes from subaddress 0 */
	u8 i2c_readlen = rlen;
	u8 i2c_subaddr = wbuf[0];
	int cnt = 16;
	unsigned long timeout = jiffies + msecs_to_jiffies(500); /* 500ms global timeout */

	/* This is a write read transaction, taken from the ISC bus via analyzer.
	 * 7 bit addressing (0x32 is 0x64)
	 * write to 0x32 ack data: 0x00 
	 *  read to 0x32 ack data: 0x00 0x00 0x00 0x00 0x32 0x02 0x98 0x08 0x1C 0x02 0x80 0x07 0x00 0x11 0x02 0x01 0x01 0x01 0x00 0x80 0x80 0x80 0x80 0x00 0x00 0x00
	 *                                             <= 562==> <=2200==> <= 540==> <=1920==>         ^ bit 1 flipped - interlaced?
	 */

	sc_write(dev, 0, BAR0_3100, 0x00000002); /* TX_FIFO Reset */
	sc_write(dev, 0, BAR0_3100, 0x00000001); /* AXI IIC Enable */
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 8) /* Start Bit */ | i2c_devaddr);

	/* Wait for the device ack.
	 * 4K Pro returns 0x40 (no bus-busy bit) instead of 0x44.
	 * Accept both — the transaction proceeds regardless.
	 */
	while (cnt > 0) {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		v = sc_read(dev, 0, BAR0_3104);
		if ((v == 0x00000044) || (v == 0x00000040))
			break;
		udelay(50);
		cnt--;
	}
	//dprintk(0, "Read 3104 %08x at cnt %d -- 44?\n", v, cnt);
	if (cnt <= 0) {
		/* 4K Pro may not always expose the expected bus-busy bit.
		 * Continue to sub-address stage unless we actually hit timeout.
		 */
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
	}

	/* Write out subaddress (single byte) */
	/* Note: Hardware currently only uses single byte sub-addresses. */
	sc_write(dev, 0, BAR0_3108, 0x00000000 | i2c_subaddr);

	/* Wait for sub-address ACK: TX_FIFO_Empty (bit 7) means data was consumed.
	 * MK2 sees 0xC4 (TX empty + RX empty + bus busy).
	 * 4K Pro may differ if bus-busy bit behaves differently.
	 */
	cnt = 16;
	while (cnt > 0) {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		v = sc_read(dev, 0, BAR0_3104);
		if (v & 0x80) /* TX_FIFO_Empty — sub-address byte consumed */
			break;
		udelay(50);
		cnt--;
	}
	//dprintk(0, "Read 3104 %08x at cnt %d -- c4?\n", v, cnt);

	msleep(1); // pkt 15162
	sc_write(dev, 0, BAR0_3120, 0x0000000f);
	sc_write(dev, 0, BAR0_3100, 0x00000002); /* TX_FIFO Reset */
	sc_write(dev, 0, BAR0_3100, 0x00000000);
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 8) /* Start Bit */ | (i2c_devaddr | 1)); /* Read from 0x65 */
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 9) /* Stop Bit */ | i2c_readlen);
	sc_write(dev, 0, BAR0_3100, 0x00000001);

	/* Read the reply */
	cnt = 0;
	while (cnt < i2c_readlen) {
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: I2C timeout reading data\n", __func__);
			return -ETIMEDOUT;
		}
		*(rbuf + cnt) = busread(dev);
		//printk("dat[0x%02x] %02x\n", cnt, *(rbuf + cnt)); 
		cnt++;
	}
	v = sc_read(dev, 0, BAR0_3104);
	/* Completion: TX_FIFO_Empty (bit 7) + RX_FIFO_Empty (bit 6) = all done.
	 * MK2 sees 0xC8/0xCC; 4K Pro may differ in SRW/BB bits.
	 */
	if ((v & 0xC0) != 0xC0) {
		printk("3104 %08x --- completion?\n", v);
		printk("  ac %08x --- 0?\n", sc_read(dev, 0, BAR0_00AC));
		return -1;
	}

	return 0; /* Success */
}

static int sc0710_i2c_writeread(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	int ret;
	mutex_lock(&dev->signalMutex);
	ret = __sc0710_i2c_writeread(dev, devaddr8bit, wbuf, wlen, rbuf, rlen);
	mutex_unlock(&dev->signalMutex);
	return ret;
}

/* Atomic DMA restart on signal restoration or resolution/refresh change.
 * Serializes with the DMA service thread via kthread_dma_lock so that
 * dequeue cannot race with stop/resize/start.
 */
void sc0710_reset_dma_frame_sync(struct sc0710_dev *dev)
{
	struct sc0710_dma_channel *ch;
	struct sc0710_dma_descriptor_chain *chain;
	struct sc0710_dma_descriptor_chain_allocation *dca;
	int ch_idx, i, j;
	int retry;
	int dma_was_running = 0;
	int has_streaming_clients = 0;

	if (!dev->fmt) {
		printk(KERN_INFO "%s: No format detected, skipping DMA reset\n", dev->name);
		return;
	}

	for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
		ch = &dev->channel[ch_idx];

		if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
			continue;

		if (ch->state == STATE_RUNNING)
			dma_was_running = 1;

		if (atomic_read(&ch->streaming_refcount) > 0)
			has_streaming_clients = 1;
	}

	if (!has_streaming_clients) {
		printk(KERN_INFO "%s: No streaming clients, skipping DMA start\n", dev->name);
		return;
	}

	printk(KERN_INFO "%s: Signal restoration - DMA was %s, have streaming clients\n",
		dev->name, dma_was_running ? "running" : "stopped");

	/* Hold the DMA service lock for the entire stop/resize/start
	 * sequence.  The DMA thread checks reconfig_in_progress under
	 * the same lock, so service cannot run while we reconfigure.
	 */
	mutex_lock(&dev->kthread_dma_lock);
	dev->reconfig_in_progress = 1;

	/* Phase 1: Stop video DMA channels */
	if (dma_was_running) {
		for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
			ch = &dev->channel[ch_idx];

			if (!ch->enabled || ch->state != STATE_RUNNING)
				continue;

			if (ch->mediatype != CHTYPE_VIDEO)
				continue;

			mutex_lock(&ch->lock);

			printk(KERN_INFO "%s: Stopping DMA channel %d for resync\n",
				dev->name, ch_idx);

			sc_write(dev, 1, ch->reg_dma_control_w1c, 0x00000001);

			usleep_range(5000, 6000);

			timer_delete_sync(&ch->timeout);

			mb();

			for (i = 0; i < ch->numDescriptorChains; i++) {
				chain = &ch->chains[i];
				for (j = 0; j < chain->numAllocations; j++) {
					dca = &chain->allocations[j];
					if (dca->wbm[0])
						*(dca->wbm[0]) = 0;
					if (dca->wbm[1])
						*(dca->wbm[1]) = 0;
				}
			}

			wmb();

			ch->dma_completed_descriptor_count_last = 0;
			ch->state = STATE_STOPPED;

			mutex_unlock(&ch->lock);
		}
	}

	/* Phase 2: Resize DMA buffers for the new resolution */
	sc0710_dma_channels_resize(dev);

	/* Phase 3: Drop first frames after restart — the FPGA may
	 * begin capturing mid-frame, producing a torn image.
	 */
	for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
		ch = &dev->channel[ch_idx];
		if (ch->enabled && ch->mediatype == CHTYPE_VIDEO) {
			ch->skip_next_frames = 3;
			ch->tear_validation_frames_left = dma_resync_validate_frames;
			ch->tear_streak_count = 0;
			ch->tear_last_line = -1;
		}
	}

	/* Phase 4: Full restart via the canonical path (prep, pipeline
	 * registers, enable, channel start).  This uses the single
	 * authoritative sc0710_program_pipeline_regs() so that the
	 * register sequence is never partially applied.
	 * Verify video DMA run bit and retry once if needed.
	 */
	for (retry = 0; retry < 2; retry++) {
		int dma_running_ok = 1;

		sc0710_dma_channels_start(dev);

		for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
			u32 dma_ctrl;

			ch = &dev->channel[ch_idx];
			if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
				continue;
			if (atomic_read(&ch->streaming_refcount) <= 0)
				continue;

			dma_ctrl = sc_read(dev, 1, ch->reg_dma_control);
			if (!(dma_ctrl & 0x00000001)) {
				dma_running_ok = 0;
				printk(KERN_WARNING "%s: DMA channel %d not running after restart (ctrl=%08x)\n",
				       dev->name, ch_idx, dma_ctrl);
				break;
			}
			ch->dma_last_completion_jiffies = jiffies;
		}

		if (dma_running_ok)
			break;

		if (retry == 0) {
			printk(KERN_WARNING "%s: Retrying DMA restart after failed run-state verify\n",
			       dev->name);
			for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
				ch = &dev->channel[ch_idx];
				if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
					continue;
				mutex_lock(&ch->lock);
				sc_write(dev, 1, ch->reg_dma_control_w1c, 0x00000001);
				ch->state = STATE_STOPPED;
				mutex_unlock(&ch->lock);
			}
			usleep_range(5000, 6000);
		}
	}

	dev->reconfig_in_progress = 0;
	mutex_unlock(&dev->kthread_dma_lock);

	printk(KERN_INFO "%s: DMA restarted after signal restoration\n", dev->name);
}



int sc0710_i2c_read_hdmi_status(struct sc0710_dev *dev)
{
	int ret;
	int i;
	int pass;
	int attempt;
	u8 wbuf[1]    = { 0x00 /* Subaddress */ };
	u8 rbuf[0x14] = { 0    /* response buffer */};
	u32 was_locked;
	int signal_locked;
	int raw_locked;
	int refresh_only_change = 0;
	u32 new_pixelLineH = 0, new_pixelLineV = 0;

	/* We're going to update dev->fmt and other shared state, so take the lock early 
       Use trylock or lock - check precedent. core.c calls this with kthread_hdmi_lock held,
       but dev->signalMutex protects the fmt.
    */
	mutex_lock(&dev->signalMutex);
	
	/* Remember previous lock state to detect signal restoration */
	was_locked = dev->locked;

	for (attempt = 0; attempt < SC0710_I2C_HDMI_RETRIES; attempt++) {
		ret = __sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU,
					     &wbuf[0], sizeof(wbuf),
					     &rbuf[0], sizeof(rbuf));
		if (ret == 0)
			break;

		if (attempt + 1 < SC0710_I2C_HDMI_RETRIES)
			usleep_range(SC0710_I2C_HDMI_RETRY_DELAY_US,
				     SC0710_I2C_HDMI_RETRY_DELAY_US + 5000);
	}

	if (ret < 0) {
		/* Preserve last known-good signal/debounce state on I2C errors. */
		mutex_unlock(&dev->signalMutex);
		printk_ratelimited(KERN_WARNING "%s: HDMI status read failed after retries (%d)\n",
				   dev->name, ret);
		return ret;
	}
	/* Lock detection differs by board.
	 * MK2: rbuf[8] is a dedicated lock flag (0 or 1).
	 * 4K Pro: rbuf[8] is part of the active resolution data, not a lock flag.
	 *         Use presence of timing data in [4:7] as the lock indicator instead.
	 *         The 4K Pro MCU intermittently returns all-zero responses even with
	 *         a valid signal, so require multiple consecutive dropouts before
	 *         declaring signal loss.
	 */


	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		raw_locked = (rbuf[4] | rbuf[5] | rbuf[6] | rbuf[7]) != 0;
	else
		raw_locked = rbuf[8] != 0;

	if (raw_locked) {
		dev->lock_dropout_count = 0;
		signal_locked = 1;
	} else if (was_locked && dev->lock_dropout_count < SC0710_LOCK_DROPOUT_MAX) {
		/* Hold locked state through transient dropouts (~1s at 200ms poll). */
		dev->lock_dropout_count++;
		mutex_unlock(&dev->signalMutex);
		return 0;
	} else {
		signal_locked = 0;
	}

	if (signal_locked) {

		dev->locked = 1;
		dev->cable_connected = 1;
		dev->unlocked_no_timing_count = 0;

		switch ((rbuf[0x0d] & 0x30) >> 4) {
		case 0x1: dev->colorimetry = BT_709;  break;
		case 0x2: dev->colorimetry = BT_601;  break;
		case 0x3: dev->colorimetry = BT_2020; break;
		default:  dev->colorimetry = BT_UNDEFINED;
		}

		switch (rbuf[0x0f]) {
		case 0x0: dev->colorspace = CS_YUV_YCRCB_422_420; break;
		case 0x1: dev->colorspace = CS_YUV_YCRCB_444;     break;
		case 0x2: dev->colorspace = CS_RGB_444;            break;
		default:  dev->colorspace = CS_UNDEFINED;
		}

		dev->eotf = EOTF_SDR;

		new_pixelLineV = rbuf[0x05] << 8 | rbuf[0x04];
		new_pixelLineH = rbuf[0x07] << 8 | rbuf[0x06];

		/* ---- Debounce path ----
		 * If a timing candidate is pending, compare the current
		 * reading against it.  Only proceed with a full reconfig
		 * after 2 consecutive matching polls (~400 ms).
		 */
		if (dev->timing_stable_count > 0) {
			if (new_pixelLineH == dev->pending_pixelLineH &&
			    new_pixelLineV == dev->pending_pixelLineV &&
			    rbuf[0x0c] == dev->pending_hint_interval &&
			    rbuf[0x0d] == dev->pending_hint_flags) {
				dev->timing_stable_count++;
			} else {
				dev->pending_pixelLineH = new_pixelLineH;
				dev->pending_pixelLineV = new_pixelLineV;
				dev->pending_hint_interval = rbuf[0x0c];
				dev->pending_hint_flags = rbuf[0x0d];
				dev->timing_stable_count = 1;
				mutex_unlock(&dev->signalMutex);
				return 0;
			}

			if (dev->timing_stable_count >= 2) {
				/* Confirmed stable — jump to the commit path */
				dev->timing_stable_count = 0;
				goto confirmed_timing_change;
			}

			mutex_unlock(&dev->signalMutex);
			return 0;
		}

		/* ---- Normal detection path ----
		 * First lock (!was_locked) or timing/rate change while
		 * locked enters the debounce — candidate is stored and we
		 * wait for confirmation on the next poll.
		 */
		if (!was_locked ||
		    (was_locked && dev->pixelLineH > 0 && dev->pixelLineV > 0 &&
		     (new_pixelLineH != dev->pixelLineH ||
		      new_pixelLineV != dev->pixelLineV ||
		      rbuf[0x0c] != dev->last_hint_interval ||
		      rbuf[0x0d] != dev->last_hint_flags))) {

			dev->pending_pixelLineH = new_pixelLineH;
			dev->pending_pixelLineV = new_pixelLineV;
			dev->pending_hint_interval = rbuf[0x0c];
			dev->pending_hint_flags = rbuf[0x0d];
			dev->timing_stable_count = 1;

			if (sc0710_debug_mode)
				printk(KERN_INFO "%s: HDMI %s, debouncing...\n",
					dev->name,
					was_locked ? "timing change" : "signal lock");

			mutex_unlock(&dev->signalMutex);
			return 0;
		}

		/* No change — keep hint tracking current */
		dev->last_hint_interval = rbuf[0x0c];
		dev->last_hint_flags = rbuf[0x0d];
		if (dev->fmt)
			dev->last_fmt = dev->fmt;

	} else {
		/* Clear any pending debounce — signal is gone */
		dev->timing_stable_count = 0;

		/* No signal detected - check if cable is connected.
		 * When a cable is connected (but no valid video signal),
		 * bytes 4-7 contain timing data from EDID negotiation.
		 * When no cable is connected, bytes 4-7 are all zero.
		 * 
		 * IMPORTANT: When receiving an unsupported timing (e.g., 4K@120Hz),
		 * the hardware may briefly lock and then unlock repeatedly.
		 * During unlock, rbuf[4-7] may be zero even though a cable IS connected.
		 * 
		 * State machine for cable detection:
		 * - If timing data present: cable connected, reset counter
		 * - If no timing but counter < threshold: assume cable still connected
		 * - If no timing and counter >= threshold: cable disconnected
		 * This allows transitioning from "No Signal" to "No Device" after
		 * confirming no activity for several consecutive polls.
		 */
		int timing_present = (rbuf[4] | rbuf[5] | rbuf[6] | rbuf[7]);
		

		if (sc0710_debug_mode) {
			printk(KERN_INFO "%s: DEBUG: rbuf[8]=%02x (lock), rbuf[4-7]=%02x %02x %02x %02x => timing_present=%d, was_locked=%d, count=%d\n",
				dev->name, rbuf[8], rbuf[4], rbuf[5], rbuf[6], rbuf[7], 
				timing_present, was_locked, dev->unlocked_no_timing_count);
		}
		
		/* Determine cable status using state machine */
		if (timing_present) {
			/* Timing data present - cable definitely connected */
			dev->cable_connected = 1;
			dev->unlocked_no_timing_count = 0;
			
			/* Valid "No Signal" state (Cable connected, but not locked) */
			dev->fmt = NULL;
			dev->locked = 0;

			dev->width = 0;
			dev->height = 0;
			dev->pixelLineH = 0;
			dev->pixelLineV = 0;
			dev->interlaced = 0;
			dev->colorimetry = BT_UNDEFINED;
			dev->colorspace = CS_UNDEFINED;
			dev->eotf = EOTF_SDR;
		} else {
			/* No timing data - increment counter */
			dev->unlocked_no_timing_count++;
			
			/* Require consecutive polls with no timing to confirm cable removal.
			 * This prevents false "No Device" during unsupported timing lock cycling.
			 * With ~200ms polling interval and threshold=6, this is ~1.2s confirmation.
			 */
			if (dev->unlocked_no_timing_count >= SC0710_NO_TIMING_THRESHOLD) {
				dev->cable_connected = 0;

				dev->fmt = NULL;
				dev->locked = 0;

				dev->width = 0;
				dev->height = 0;
				dev->pixelLineH = 0;
				dev->pixelLineV = 0;
				dev->interlaced = 0;
				dev->colorimetry = BT_UNDEFINED;
				dev->colorspace = CS_UNDEFINED;
				dev->eotf = EOTF_SDR;
			} else {
				/* Still in grace period - assume cable connected */
				dev->cable_connected = 1;
				if (sc0710_debug_mode) {
					printk(KERN_INFO "%s: No timing data, count=%d/%d, assuming cable still connected\n",
					       dev->name, dev->unlocked_no_timing_count,
					       SC0710_NO_TIMING_THRESHOLD);
				}
			}
		}
		
		if (sc0710_debug_mode) {
			printk(KERN_INFO "%s: STATUS: %s (cable_connected=%d)\n",
				dev->name,
				dev->cable_connected ? "NO SIGNAL (cable present)" : "NO DEVICE (cable unplugged)",
				dev->cable_connected);
		}
	}

	mutex_unlock(&dev->signalMutex);
	return 0; /* Success */

confirmed_timing_change:
	/* ---- Debounce confirmed: commit timing and trigger reconfig ----
	 * All operational fields are updated from the latest I2C response
	 * (which matched the pending candidate for 2 consecutive polls).
	 * dev->fmt is set BEFORE the DMA restart so that the resize step
	 * allocates chains for the correct framesize.  The DMA service
	 * thread is blocked by kthread_dma_lock inside reset, so it
	 * cannot observe the new fmt with stale DMA chains.
	 */
	{
		u32 fps_target = 0;
		u8 hint_interval = rbuf[0x0c];
		u8 hint_flags = rbuf[0x0d];
		u32 prev_pixelLineH = dev->pixelLineH;
		u32 prev_pixelLineV = dev->pixelLineV;
		u32 prev_width = dev->width;
		u32 prev_height = dev->height;
		u32 prev_interlaced = dev->interlaced;

		dev->last_hint_interval = hint_interval;
		dev->last_hint_flags = hint_flags;
		dev->pixelLineV = new_pixelLineV;
		dev->pixelLineH = new_pixelLineH;
		dev->width = rbuf[0x0b] << 8 | rbuf[0x0a];
		dev->height = rbuf[0x09] << 8 | rbuf[0x08];
		dev->interlaced = rbuf[0x0d] & 0x01;
		if (dev->interlaced)
			dev->height *= 2;

		if (was_locked &&
		    prev_pixelLineH == new_pixelLineH &&
		    prev_pixelLineV == new_pixelLineV &&
		    prev_width == dev->width &&
		    prev_height == dev->height &&
		    prev_interlaced == dev->interlaced)
			refresh_only_change = 1;

		if (sc0710_debug_mode) {
			printk(KERN_INFO "%s: HDMI raw: ", dev->name);
			for (i = 0; i < 0x14; i++)
				printk(KERN_CONT "%02x ", rbuf[i]);
			printk(KERN_CONT "\n");
		}

		if (hint_interval > 0 && hint_interval < 0xFF) {
			fps_target = 3600 / hint_interval;
			if (hint_interval == 0x78 && (hint_flags & 0x10))
				fps_target = 120;
		}

		/* Timing selection strategy:
		 * 0 = merge (static table first, dynamic fallback)
		 * 1 = procedural only (skip static table)
		 * 2 = static only (no dynamic fallback)
		 */
		if (procedural_timings != TIMING_MODE_PROCEDURAL_ONLY) {
			dev->fmt = sc0710_format_find_by_timing_and_rate(
					dev->pixelLineH, dev->pixelLineV, fps_target);
		} else {
			dev->fmt = NULL;
		}

		if (!dev->fmt &&
		    procedural_timings != TIMING_MODE_STATIC_ONLY &&
		    dev->width >= 320 && dev->height >= 200) {
			struct sc0710_format *dyn = &dev->dynamic_fmt;
			u32 fps_est = fps_target ? fps_target : 60;

			dyn->timingH    = dev->pixelLineH;
			dyn->timingV    = dev->pixelLineV;
			dyn->width      = dev->width;
			dyn->height     = dev->height;
			dyn->interlaced = dev->interlaced;
			dyn->fpsX100    = fps_est * 100;
			dyn->fpsnum     = fps_est * 1000;
			dyn->fpsden     = 1000;
			dyn->depth      = 8;
			dyn->framesize  = dev->width * 2 * dev->height;
			snprintf(dev->dynamic_fmt_name,
				 sizeof(dev->dynamic_fmt_name),
				 "%ux%u%s%u(dynamic)",
				 dev->width, dev->height,
				 dev->interlaced ? "i" : "p",
				 fps_est);
			dyn->name = dev->dynamic_fmt_name;

			memset(&dyn->dv_timings, 0, sizeof(dyn->dv_timings));
			dyn->dv_timings.type = V4L2_DV_BT_656_1120;
			dyn->dv_timings.bt.width  = dev->width;
			dyn->dv_timings.bt.height = dev->height;
			dyn->dv_timings.bt.interlaced =
				dev->interlaced ? V4L2_DV_INTERLACED
						: V4L2_DV_PROGRESSIVE;

			dev->fmt = dyn;
			printk(KERN_INFO "%s: Dynamic format: %s (timing %dx%d)\n",
			       dev->name, dyn->name,
			       dyn->timingH, dyn->timingV);
		}

		if (!dev->fmt) {
			printk(KERN_INFO "%s: Unknown timing %dx%d (add to formats table)\n",
				dev->name, dev->pixelLineH, dev->pixelLineV);
		} else {
			printk(KERN_INFO "%s: Detected timing %dx%d -> format: %s\n",
				dev->name, dev->pixelLineH, dev->pixelLineV,
				dev->fmt->name);
			dev->last_fmt = dev->fmt;
		}
	}

	mutex_unlock(&dev->signalMutex);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		struct sc0710_dma_channel *vch = &dev->channel[i];
		if (!vch->enabled || vch->mediatype != CHTYPE_VIDEO)
			continue;
		vch->tear_resync_retries_left = dma_resync_max_tear_retries;
	}

	printk(KERN_INFO "%s: Resynchronizing DMA frames\n", dev->name);
	sc0710_reset_dma_frame_sync(dev);

	/* Refresh-rate-only switches are more likely to leave DMA mis-phased.
	 * Run extra resync passes with a short delay to get independent lock attempts.
	 */
	if (refresh_only_change && refresh_rate_resync_passes > 1) {
		for (pass = 1; pass < refresh_rate_resync_passes; pass++) {
			msleep(refresh_rate_resync_delay_ms);
			printk(KERN_INFO "%s: Refresh-rate change follow-up resync pass %d/%u\n",
			       dev->name, pass + 1, refresh_rate_resync_passes);
			sc0710_reset_dma_frame_sync(dev);
		}
	}

	if (!auto_scaler && dev->scaler_mode == SCALER_MODE_DISABLED)
		sc0710_video_notify_source_change(dev);

	return 0;
}

int sc0710_i2c_read_status2(struct sc0710_dev *dev)
{
	int ret, i;
	u8 wbuf[1]    = { 0x1a /* Subaddress */ };
	u8 rbuf[0x10] = { 0    /* response buffer */};

	ret = sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, &wbuf[0], sizeof(wbuf), &rbuf[0], sizeof(rbuf));
	if (ret < 0) {
		printk("%s ret = %d\n", __func__, ret);
		return -1;
	}

	if (sc0710_debug_mode) {
		printk("%s status2: ", dev->name);
		for (i = 0; i < sizeof(rbuf); i++)
			printk("%02x ", rbuf[i]);
		printk("\n");
	}

	return 0; /* Success */
}

int sc0710_i2c_read_status3(struct sc0710_dev *dev)
{
	int ret, i;
	u8 wbuf[1]    = { 0x2a /* Subaddress */ };
	u8 rbuf[0x10] = { 0    /* response buffer */};

	ret = sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, &wbuf[0], sizeof(wbuf), &rbuf[0], sizeof(rbuf));
	if (ret < 0) {
		printk("%s ret = %d\n", __func__, ret);
		return -1;
	}

	if (sc0710_debug_mode) {
		printk("%s status3: ", dev->name);
		for (i = 0; i < sizeof(rbuf); i++)
			printk("%02x ", rbuf[i]);
		printk("\n");
	}

	return 0; /* Success */
}

/* User video controls for brightness, contrast, saturation and hue. */
int sc0710_i2c_read_procamp(struct sc0710_dev *dev)
{
	int ret, i;
	u8 wbuf[1]    = { 0x12 /* Subaddress */ };
	u8 rbuf[0x05] = { 0    /* response buffer */};

	ret = sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, &wbuf[0], sizeof(wbuf), &rbuf[0], sizeof(rbuf));
	if (ret < 0) {
		printk("%s ret = %d\n", __func__, ret);
		return -1;
	}

	dev->brightness = rbuf[1];
	dev->contrast   = rbuf[2];
	dev->saturation = rbuf[3];
	dev->hue        = (s8)rbuf[4];

	if (sc0710_debug_mode) {
		printk("%s procamp: ", dev->name);
		for (i = 0; i < sizeof(rbuf); i++)
			printk("%02x ", rbuf[i]);
		printk("\n");

		printk("%s procamp: brightness %d contrast %d saturation %d hue %d\n",
			dev->name,
			dev->brightness,
			dev->contrast,
			dev->saturation,
			dev->hue);
	}

	return 0; /* Success */
}

/* Factory EDID base block bytes 0x00-0x5F.
 * These bytes are volatile and lost on cold boot (read as 0xFF).
 * Bytes 0x60-0xFF persist across power cycles.
 * The Windows driver writes these on every load.
 */
static const u8 factory_edid_base[96] = {
	/* 00 */ 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
	/* 08 */ 0x14, 0xE1, 0x12, 0x00, 0x06, 0x00, 0x00, 0x00,
	/* 10 */ 0x2F, 0x21, 0x01, 0x03, 0x80, 0x80, 0x48, 0x78,
	/* 18 */ 0x2A, 0xDA, 0xFF, 0xA3, 0x58, 0x4A, 0xA2, 0x29,
	/* 20 */ 0x17, 0x49, 0x4B, 0x20, 0x08, 0x00, 0x31, 0x40,
	/* 28 */ 0x61, 0x40, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	/* 30 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x08, 0xE8,
	/* 38 */ 0x00, 0x30, 0xF2, 0x70, 0x5A, 0x80, 0xB0, 0x58,
	/* 40 */ 0x8A, 0x00, 0xBA, 0x88, 0x21, 0x00, 0x00, 0x1E,
	/* 48 */ 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
	/* 50 */ 0x58, 0x2C, 0x45, 0x00, 0xBA, 0x88, 0x21, 0x00,
	/* 58 */ 0x00, 0x1E, 0x6F, 0xC2, 0x00, 0xA0, 0xA0, 0xA0,
};

static void sc0710_write_factory_edid(struct sc0710_dev *dev)
{
	#define I2C_EDID_EEPROM (0x50 << 1)
	u8 w[1] = { 0x00 };
	u8 r[8] = { 0 };
	int i, ret;

	__sc0710_i2c_writeread(dev, I2C_EDID_EEPROM, w, sizeof(w), r, sizeof(r));

	if (r[0] != 0x00 || r[1] != 0xFF) {
		printk(KERN_INFO "%s: EDID header invalid (%02x %02x), writing factory data\n",
			dev->name, r[0], r[1]);

		for (i = 0; i < 96; i += 8) {
			u8 wr[10];
			int j;
			wr[0] = 0x00;
			wr[1] = i;
			for (j = 0; j < 8; j++)
				wr[j + 2] = factory_edid_base[i + j];
			ret = sc0710_i2c_write(dev, I2C_EDID_EEPROM, wr, 10);
			if (ret < 0)
				break;
			msleep(10);
		}
	}
}

/* Wait for 4KP FPGA pipeline to become active before DMA start.
 * Pipeline registers are configured early in card_setup().
 * Caller must hold signalMutex.
 */
int sc0710_4kp_wait_pipeline(struct sc0710_dev *dev)
{
	u32 a8;
	int i;
	u8 wbuf[1] = { 0x00 };
	u8 rbuf[16];

	a8 = sc_read(dev, 0, 0xa8);
	if (a8 != 0) {
		printk(KERN_INFO "%s: A8 already active (%08x), TX ok\n", dev->name, a8);
		return 0;
	}

	/* MCU TX status for diagnostics */
	wbuf[0] = 0x10;
	__sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, wbuf, 1, rbuf, 16);
	printk(KERN_INFO "%s: MCU status [13-15]: %02x %02x %02x\n",
		dev->name, rbuf[3], rbuf[4], rbuf[5]);

	/* Poll A8 — 4KP FPGA pipeline may need time to become active
	 * after D0/EC register writes. Typically activates within 100ms.
	 */
	for (i = 0; i < 20; i++) {
		msleep(100);
		a8 = sc_read(dev, 0, 0xa8);
		if (a8 != 0) {
			printk(KERN_INFO "%s: A8 active after %dms: %08x\n",
				dev->name, (i + 1) * 100, a8);
			return 0;
		}
	}

	printk(KERN_WARNING "%s: A8 still 0 after 2s — DMA may stall\n", dev->name);
	return 0;
}

int sc0710_i2c_initialize(struct sc0710_dev *dev)
{
	if (dev->board != SC0710_BOARD_ELGATEO_4KP)
		return 0;

	msleep(500);

	mutex_lock(&dev->signalMutex);

	/* Write factory EDID if missing (lost on cold boot) */
	sc0710_write_factory_edid(dev);

	mutex_unlock(&dev->signalMutex);

	return 0;
}

