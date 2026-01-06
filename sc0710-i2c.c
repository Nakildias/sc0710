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

#if 0
static int didack(struct sc0710_dev *dev)
{
	u32 v;
	int cnt = 16;

	while (cnt-- > 0) {
		v = sc_read(dev, 0, BAR0_3104);
		if ((v == 0x44) || (v == 0xc0)) {
			return 1; /* device Ack'd */
		}
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
		if ((v == 0x0000008c) || (v == 0x000000ac))
			break;
		usleep_range(100, 150); /* More scheduler-friendly than udelay */
	}

	v = sc_read(dev, 0, BAR0_310C);
//printk("readbus ret 0x%02x\n", v);
	return v;
}

#if 0
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

	/* Wait for the device ack */
	while (cnt > 0) {
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: I2C timeout waiting for device ack\n", __func__);
			return -ETIMEDOUT;
		}
		v = sc_read(dev, 0, BAR0_3104);
		if (v == 0x00000044)
			break;
		usleep_range(50, 80);
		cnt--;
	}
	//dprintk(0, "Read 3104 %08x at cnt %d -- 44?\n", v, cnt);
	if (cnt <= 0) {
		return -ETIMEDOUT;
	}

	/* Write out subaddress (single byte) */
	/* TODO: We only suppport single byte sub-addresses. */
	sc_write(dev, 0, BAR0_3108, 0x00000000 | i2c_subaddr);

	/* Wait for the device ack */
	cnt = 16;
	while (cnt > 0) {
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: I2C timeout waiting for subaddr ack\n", __func__);
			return -ETIMEDOUT;
		}
		v = sc_read(dev, 0, BAR0_3104);
		if (v == 0x000000c4)
			break;
		usleep_range(50, 80);
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
	if (v != 0xc8) {
		printk("3104 %08x --- c8?\n", sc_read(dev, 0, BAR0_3104));
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
			
			/* Small delay to let hardware quiesce */
			usleep_range(2000, 3000);
			
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
	u8 rbuf[0x1a] = { 0    /* response buffer */};
	u32 was_locked;

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
#if 0
	printk("%s    hdmi: ", dev->name);
	for (i = 0; i < sizeof(rbuf); i++)
		printk("%02x ", rbuf[i]);
	printk("\n");
#endif
	if (rbuf[8]) {
		u32 new_pixelLineH, new_pixelLineV;
		int timing_changed = 0;
		
		dev->locked = 1;
		
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

		/* Save old timings to detect changes */
		new_pixelLineV = rbuf[0x05] << 8 | rbuf[0x04];
		new_pixelLineH = rbuf[0x07] << 8 | rbuf[0x06];
		
		/* Detect timing change (quick replug or resolution change) */
		if (was_locked && dev->pixelLineH > 0 && dev->pixelLineV > 0) {
			if (new_pixelLineH != dev->pixelLineH || new_pixelLineV != dev->pixelLineV) {
				timing_changed = 1;
				printk(KERN_INFO "%s: HDMI timing changed (%dx%d -> %dx%d)\n",
					dev->name, dev->pixelLineH, dev->pixelLineV,
					new_pixelLineH, new_pixelLineV);
			}
		}

		dev->width = rbuf[0x0b] << 8 | rbuf[0x0a];
		dev->height = rbuf[0x09] << 8 | rbuf[0x08];
		dev->pixelLineV = new_pixelLineV;
		dev->pixelLineH = new_pixelLineH;

		dev->interlaced = rbuf[0x0d] & 0x01;
		if (dev->interlaced)
			dev->height *= 2;

		dev->fmt = sc0710_format_find_by_timing(dev->pixelLineH, dev->pixelLineV);
		
		/* Save last known format for placeholder rendering */
		if (dev->fmt)
			dev->last_fmt = dev->fmt;
		
		/* Detect signal restoration (unlocked -> locked transition) OR timing change */
		if ((!was_locked && dev->locked) || timing_changed) {
			printk(KERN_INFO "%s: HDMI signal %s, waiting for stabilization...\n",
				dev->name, timing_changed ? "timing changed" : "restored");
			mutex_unlock(&dev->signalMutex);
			
			/* Wait for HDMI signal to stabilize.
			 * A 150ms delay gives the source time to fully establish the link.
			 */
			msleep(150);
			
			printk(KERN_INFO "%s: Resynchronizing DMA frames\n", dev->name);
			sc0710_reset_dma_frame_sync(dev);
			return 0; /* Success */
		}
	} else {
		dev->fmt = NULL;
		dev->locked = 0;
		dev->width = 0;
		dev->height = 0;
		dev->pixelLineH = 0;
		dev->pixelLineV = 0;
		dev->interlaced = 0;
		dev->colorimetry = BT_UNDEFINED;
		dev->colorspace = CS_UNDEFINED;
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

	printk("%s status2: ", dev->name);
	for (i = 0; i < sizeof(rbuf); i++)
		printk("%02x ", rbuf[i]);
	printk("\n");

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

	printk("%s status3: ", dev->name);
	for (i = 0; i < sizeof(rbuf); i++)
		printk("%02x ", rbuf[i]);
	printk("\n");

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

	return 0; /* Success */
}

#if 0
static int sc0710_i2c_cfg_unknownpart(struct sc0710_dev *dev)
{
	int ret;
	u8 wbuf[5] = { 0xab, 0x03, 0x12, 0x34, 0x57 };

	ret = sc0710_i2c_write(dev, I2C_DEV__UNKNOWN, &wbuf[0], sizeof(wbuf));
	if (ret < 0) {
		printk("%s ret = %d\n", __func__, ret);
		return -1;
	}

	printk("%s() success\n", __func__);
	return 0; /* Success */
}

static int sc0710_i2c_cfg_unknownpart2(struct sc0710_dev *dev)
{
	int ret;
	u8 wbuf[2] = { 0x10, 0x01 };

	ret = sc0710_i2c_write(dev, I2C_DEV__ARM_MCU, &wbuf[0], sizeof(wbuf));
	if (ret < 0) {
		printk("%s ret = %d\n", __func__, ret);
		return -1;
	}

	printk("%s() success\n", __func__);
	return 0; /* Success */
}
#endif

int sc0710_i2c_initialize(struct sc0710_dev *dev)
{
	//sc0710_i2c_cfg_unknownpart2(dev);

	return 0; /* Success */
}

