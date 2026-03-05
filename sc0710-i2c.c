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
		if (time_after(jiffies, timeout)) {
			return 0;
		}
		v = sc_read(dev, 0, BAR0_3104);
		if ((v == 0x00000044) || (v == 0x00000040))
			break;
		udelay(50);
		cnt--;
	}
	//dprintk(0, "Read 3104 %08x at cnt %d -- 44?\n", v, cnt);
	if (cnt <= 0) {
		return 0; /* Continue anyway — 4K Pro may not set bus-busy */
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
		if (time_after(jiffies, timeout)) {
			return 0;
		}
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

/* Helper to fully restart DMA on signal restoration to fix frame alignment.
 * Also handles starting DMA if streaming started without signal.
 */
static void sc0710_reset_dma_frame_sync(struct sc0710_dev *dev)
{
	struct sc0710_dma_channel *ch;
	struct sc0710_dma_descriptor_chain *chain;
	struct sc0710_dma_descriptor_chain_allocation *dca;
	int ch_idx, i, j;
	int dma_was_running = 0;
	int has_streaming_clients = 0;

	if (!dev->fmt) {
		printk(KERN_INFO "%s: No format detected, skipping DMA reset\n", dev->name);
		return;
	}

	/* Check video channel status */
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

	/* Phase 1: Stop DMA if it was running */
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
			
			/* Stop the DMA hardware */
			sc_write(dev, 1, ch->reg_dma_control_w1c, 0x00000001);
			
			/* Longer delay to ensure all in-flight DMA transactions complete.
			 * This prevents race conditions where DMA completion processing
			 * could occur with stale buffer state during resize.
			 */
			usleep_range(5000, 6000);
			
			/* Delete timeout timer to prevent it firing with stale buffer
			 * state during resize operations.
			 */
			timer_delete_sync(&ch->timeout);
			
			/* Memory barrier to ensure DMA stop is visible to all CPUs
			 * before we clear the writeback metadata.
			 */
			mb();
			
			/* Clear all writeback metadata */
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
			
			/* Write memory barrier to ensure metadata clear is visible
			 * before we proceed with resize.
			 */
			wmb();
			
			/* Reset descriptor counter */
			ch->dma_completed_descriptor_count_last = 0;
			sc_write(dev, 1, ch->reg_dma_completed_descriptor_count, 1);
			sc_write(dev, 1, ch->reg_sg_start_h, ch->pt_dma >> 32);
			sc_write(dev, 1, ch->reg_sg_start_l, ch->pt_dma);
			sc_write(dev, 1, ch->reg_sg_adj, 0);
			
			/* Update state so resize() can proceed */
			ch->state = STATE_STOPPED;
			
			mutex_unlock(&ch->lock);
		}
	}

	/* Phase 2: Resize DMA buffers if needed (for resolution changes) */
	sc0710_dma_channels_resize(dev);

	/* Phase 3: Program hardware registers */
	if (dev->fmt) {
		sc_write(dev, 0, BAR0_00C8, dev->fmt->height);
		printk(KERN_INFO "%s: Reprogrammed height register to %d\n",
			dev->name, dev->fmt->height);
	}
	sc_write(dev, 0, BAR0_00D0, 0x4100);
	sc_write(dev, 0, 0xcc, 0);
	sc_write(dev, 0, 0xdc, 0);
	sc_write(dev, 0, BAR0_00D0, 0x4300);
	sc_write(dev, 0, BAR0_00D0, 0x4100);

	/* Small delay before restart */
	msleep(10);

	/* Phase 4: Start DMA */
	sc0710_dma_channels_start(dev);
	printk(KERN_INFO "%s: DMA started after signal restoration\n", dev->name);
}



int sc0710_i2c_read_hdmi_status(struct sc0710_dev *dev)
{
	int ret;
	int i;
	u8 wbuf[1]    = { 0x00 /* Subaddress */ };
	u8 rbuf[0x14] = { 0    /* response buffer */};
	u32 was_locked;
	int signal_locked;

	/* We're going to update dev->fmt and other shared state, so take the lock early 
       Use trylock or lock - check precedent. core.c calls this with kthread_hdmi_lock held,
       but dev->signalMutex protects the fmt.
    */
	mutex_lock(&dev->signalMutex);
	
	/* Remember previous lock state to detect signal restoration */
	was_locked = dev->locked;

	ret = __sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, &wbuf[0], sizeof(wbuf), &rbuf[0], sizeof(rbuf));
	if (ret < 0) {
		mutex_unlock(&dev->signalMutex);
		printk("%s ret = %d\n", __func__, ret);
		return -1;
	}
	/* Lock detection differs by board.
	 * MK2: rbuf[8] is a dedicated lock flag (0 or 1).
	 * 4K Pro: rbuf[8] is part of the active resolution data, not a lock flag.
	 *         Use presence of timing data in [4:7] as the lock indicator instead.
	 *         The 4K Pro MCU intermittently returns all-zero responses even with
	 *         a valid signal, so require multiple consecutive dropouts before
	 *         declaring signal loss.
	 */


	if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
		int raw_locked = (rbuf[4] | rbuf[5] | rbuf[6] | rbuf[7]) != 0;
		if (raw_locked) {
			dev->lock_dropout_count = 0;
			signal_locked = 1;
		} else if (was_locked && dev->lock_dropout_count < 5) {
			/* Hold locked state through transient dropouts (~1s at 200ms poll) */
			dev->lock_dropout_count++;
			mutex_unlock(&dev->signalMutex);
			return 0;
		} else {
			signal_locked = 0;
		}
	} else {
		signal_locked = rbuf[8] != 0;
	}

	if (signal_locked) {
		u32 new_pixelLineH, new_pixelLineV;
		int timing_changed = 0;

		dev->locked = 1;
		
		/* If we have a lock, a cable is definitely connected */
		dev->cable_connected = 1;
		dev->unlocked_no_timing_count = 0; /* Reset counter on lock */
		
		switch ((rbuf[0x0d] & 0x30) >> 4) {
		case 0x1:
			dev->colorimetry = BT_709;
			break;
		case 0x2:
			dev->colorimetry = BT_601;
			break;
		case 0x3:
			dev->colorimetry = BT_2020;
			break;
		default:
			dev->colorimetry = BT_UNDEFINED;
		}

		switch (rbuf[0x0f]) {
		case 0x0:
			dev->colorspace = CS_YUV_YCRCB_422_420;
			break;
		case 0x1:
			dev->colorspace = CS_YUV_YCRCB_444;
			break;
		case 0x2:
			dev->colorspace = CS_RGB_444;
			break;
		default:
			dev->colorspace = CS_UNDEFINED;
		}

		/* Default EOTF to SDR - safer than assuming HDR.
		 * TODO: Parse actual EOTF from HDR DR InfoFrame if available.
		 * HDR DR InfoFrame EOTF field: 0=SDR, 2=SMPTE 2084/PQ, 3=HLG
		 */
		dev->eotf = EOTF_SDR;


		/* Save old timings to detect changes */
		new_pixelLineV = rbuf[0x05] << 8 | rbuf[0x04];
		new_pixelLineH = rbuf[0x07] << 8 | rbuf[0x06];
		
		/* Detect timing change (quick replug or resolution change) */
		if (was_locked && dev->pixelLineH > 0 && dev->pixelLineV > 0) {
			if (new_pixelLineH != dev->pixelLineH || new_pixelLineV != dev->pixelLineV ||
			    rbuf[0x0c] != dev->last_hint_interval || rbuf[0x0d] != dev->last_hint_flags) {
				timing_changed = 1;
				if (sc0710_debug_mode) {
					printk(KERN_INFO "%s: HDMI timing/rate changed (%dx%d@%x/%x -> %dx%d@%x/%x)\n",
						dev->name, dev->pixelLineH, dev->pixelLineV, dev->last_hint_interval, dev->last_hint_flags,
						new_pixelLineH, new_pixelLineV, rbuf[0x0c], rbuf[0x0d]);
				}
			}
		}
		
		dev->last_hint_interval = rbuf[0x0c];
		dev->last_hint_flags = rbuf[0x0d];

		dev->width = rbuf[0x0b] << 8 | rbuf[0x0a];
		dev->height = rbuf[0x09] << 8 | rbuf[0x08];
		dev->pixelLineV = new_pixelLineV;
		dev->pixelLineH = new_pixelLineH;

		dev->interlaced = rbuf[0x0d] & 0x01;
		if (dev->interlaced)
			dev->height *= 2;




		if (timing_changed || !was_locked) {
			u32 fps_target = 0;
			u8 hint_interval = rbuf[0x0c];
			u8 hint_flags = rbuf[0x0d];

			/* DEBUG: Print raw I2C response on change */
			if (sc0710_debug_mode) {
				printk(KERN_INFO "%s: HDMI raw: ", dev->name);
				for (i = 0; i < 0x14; i++)
					printk(KERN_CONT "%02x ", rbuf[i]);
				printk(KERN_CONT "\n");
			}

			/* Differentiate FPS based on I2C hints:
			 * Byte 12 (0x0C) appears to be frame interval (approx 3600/FPS).
			 * Byte 13 (0x0D) also varies for 120Hz.
			 */
			if (hint_interval == 0x78) { /* ~120 -> 30Hz or 120Hz */
				if (hint_flags == 0x10)
					fps_target = 120; /* 1080p120 */
				else
					fps_target = 30;  /* 1080p30 (flags=0x50) */
			} else if (hint_interval == 0x3C) { /* ~60 -> 60Hz */
				fps_target = 60;
			}

			/* Use rate hint to differentiate modes (e.g. 1080p30 vs 1080p120) */
			dev->fmt = sc0710_format_find_by_timing_and_rate(dev->pixelLineH, dev->pixelLineV, fps_target);
		}
		
		/* Debug: show timing when format not found */
		if (!dev->fmt) {
			printk(KERN_INFO "%s: Unknown timing %dx%d (add to formats table)\n",
				dev->name, dev->pixelLineH, dev->pixelLineV);
		}
		
		/* Log format detection on timing change or signal restore */
		if (timing_changed || !was_locked) {
			if (dev->fmt) {
				printk(KERN_INFO "%s: Detected timing %dx%d -> format: %s\n",
					dev->name, dev->pixelLineH, dev->pixelLineV, dev->fmt->name);
			}
		}
		
		/* Save last known format for placeholder rendering */
		if (dev->fmt)
			dev->last_fmt = dev->fmt;
		
		/* Detect signal restoration (unlocked -> locked transition) OR timing change */
		if ((!was_locked && dev->locked) || timing_changed) {
			printk(KERN_INFO "%s: HDMI signal %s, waiting for stabilization...\n",
				dev->name, timing_changed ? "timing changed" : "restored");
			mutex_unlock(&dev->signalMutex);
			
			/* Wait for HDMI signal to stabilize.
			 * A 300ms delay gives the source time to fully establish the link.
			 * Shorter delays can result in processing during signal transition,
			 * leading to format/buffer mismatches and potential kernel panics.
			 */
			msleep(300);
			
			printk(KERN_INFO "%s: Resynchronizing DMA frames\n", dev->name);
			sc0710_reset_dma_frame_sync(dev);
			return 0; /* Success */
		}
	} else {
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
			
			/* Require 3 consecutive polls with no timing to confirm cable removal.
			 * This prevents false "No Device" during unsupported timing lock cycling.
			 * With ~200ms polling interval, this is about 600ms confirmation time.
			 */
			if (dev->unlocked_no_timing_count >= 3) {
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
					printk(KERN_INFO "%s: No timing data, count=%d/3, assuming cable still connected\n",
						dev->name, dev->unlocked_no_timing_count);
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

/* Send init commands to MCU/LT6911 and check pipeline status before DMA start.
 * These are the original cfg_unknownpart/cfg_unknownpart2 commands from the
 * upstream MK2 driver (were #if 0'd out). Note: sc0710_i2c_write() skips
 * wbuf[0], so wbuf[0] is a dummy byte.
 *
 * Caller must hold signalMutex.
 */
int sc0710_lt6911_enable_output(struct sc0710_dev *dev)
{
	int ret;
	u8 mcu_cmd[2] = { 0x10, 0x01 };
	u8 lt_cmd[5] = { 0xAB, 0x03, 0x12, 0x34, 0x57 };

	/* cfg_unknownpart2: write 0x01 to MCU (0x64).
	 * wbuf[0]=dummy, wbuf[1]=0x01 sent on wire.
	 */
	ret = sc0710_i2c_write(dev, I2C_DEV__ARM_MCU, mcu_cmd, sizeof(mcu_cmd));
	if (ret < 0)
		printk(KERN_WARNING "%s: MCU init cmd failed: %d\n", dev->name, ret);

	msleep(50);

	/* cfg_unknownpart: write {0x03, 0x12, 0x34, 0x57} to LT6911 proxy (0x66).
	 * wbuf[0]=dummy, wbuf[1..4] sent on wire as sub=0x03 data=0x12,0x34,0x57.
	 */
	ret = sc0710_i2c_write(dev, I2C_DEV__UNKNOWN, lt_cmd, sizeof(lt_cmd));
	if (ret < 0)
		printk(KERN_WARNING "%s: LT6911 init cmd failed: %d\n", dev->name, ret);

	msleep(200);

	{
		u32 a8 = sc_read(dev, 0, 0xa8);
		printk(KERN_INFO "%s: Pipeline after init cmds: A8=%08x\n", dev->name, a8);
	}

	return 0;
}

int sc0710_i2c_initialize(struct sc0710_dev *dev)
{
	int ret;

	if (dev->board != SC0710_BOARD_ELGATEO_4KP)
		return 0;

	mutex_lock(&dev->signalMutex);

	/* Check A8 BEFORE any commands - detect residual state from previous load */
	{
		u32 a8_pre = sc_read(dev, 0, 0xa8);
		printk(KERN_INFO "%s: A8 pre-init (residual): %08x\n", dev->name, a8_pre);
	}

	/* Wait for MCU to be ready after PCI enumeration */
	msleep(500);

	/* Send init commands to MCU and LT6911 proxy.
	 * sc0710_i2c_write() skips wbuf[0], so wbuf[0] is a dummy byte.
	 */
	{
		u8 mcu_cmd_raw[2] = { 0x00, 0x01 };
		u8 lt_cmd[5] = { 0x00, 0x03, 0x12, 0x34, 0x57 };
		int poll;
		u32 a8;

		/* cfg_unknownpart2: send 0x01 to MCU */
		ret = sc0710_i2c_write(dev, I2C_DEV__ARM_MCU, mcu_cmd_raw, sizeof(mcu_cmd_raw));
		if (ret < 0)
			printk(KERN_WARNING "%s: MCU init cmd failed: %d\n", dev->name, ret);

		msleep(50);

		/* cfg_unknownpart: send {0x03, 0x12, 0x34, 0x57} to 0x66 */
		ret = sc0710_i2c_write(dev, I2C_DEV__UNKNOWN, lt_cmd, sizeof(lt_cmd));
		if (ret < 0)
			printk(KERN_WARNING "%s: LT6911 init cmd failed: %d\n", dev->name, ret);

		/* Poll A8 for up to 10 seconds - LT6911 may need time to start output */
		for (poll = 0; poll < 20; poll++) {
			msleep(500);
			a8 = sc_read(dev, 0, 0xa8);
			if (a8 != 0) {
				printk(KERN_INFO "%s: A8 active after %dms: %08x\n",
					dev->name, (poll + 1) * 500, a8);
				break;
			}
		}
	}

	mutex_unlock(&dev->signalMutex);

	return 0;
}

