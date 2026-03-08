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

int sc0710_dma_channels_resize(struct sc0710_dev *dev)
{
	printk(KERN_ERR "%s()\n", __func__);
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		sc0710_dma_channel_resize(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		/* Audio uses fixed buffer size, do not resize as it may be active via ALSA */
		/* sc0710_dma_channel_resize(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO); */
		break;
	}

	return 0;
}

int sc0710_dma_channels_alloc(struct sc0710_dev *dev)
{
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		sc0710_dma_channel_alloc(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		sc0710_dma_channel_alloc(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO);
		break;
	}

	return 0;
}

void sc0710_dma_channels_free(struct sc0710_dev *dev)
{
	int i;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		sc0710_dma_channel_free(dev, i);
	}
}

void sc0710_dma_channels_stop(struct sc0710_dev *dev)
{
	int i, ret;

	printk("%s()\n", __func__);

	sc_clr(dev, 0, BAR0_00D0, 0x0001);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_stop(&dev->channel[i]);
	}
}

int sc0710_dma_channels_start(struct sc0710_dev *dev)
{
	int i, ret;

	printk("%s()\n", __func__);

	/* Wait for 4KP FPGA pipeline to become active before DMA start */
	if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
		mutex_lock(&dev->signalMutex);
		sc0710_4kp_wait_pipeline(dev);
		mutex_unlock(&dev->signalMutex);
	}

	/* Prepare all DMA channels to start */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_start_prep(&dev->channel[i]);
	}

	/* TODO: What do these registers do? Any documentation? */
	/* Digging into the reference drivers for the SCxxxx cards available
	 * from the CM's website, the hardware supports a video scaler.
	 * I'm guessing that this is setting - maybe - a scaler? */

	/* Set the height register to the incoming signal format height */
	if (dev->fmt) {
		sc_write(dev, 0, BAR0_00C8, dev->fmt->height);
	} else {
		sc_write(dev, 0, BAR0_00C8, 0x438); /* 1080 default */
	}

	/* Set scaler output height for 4K Pro (always 1080p output).
	 * Without this, the FPGA scaler produces no output and the
	 * XDMA C2H engine gets no AXI-Stream data.
	 */
	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, BAR0_00D8, 0x438);

	sc_write(dev, 0, BAR0_00D0, 0x4100);
	sc_write(dev, 0, 0xCC, 0x00000000);
	/* DC: MK2 uses 0 (no scaler). 4K Pro: FPGA auto-populates to 0x1050. */
	if (dev->board != SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, BAR0_00DC, 0x00000000);
	sc_write(dev, 0, BAR0_00D0, 0x4300);
	sc_write(dev, 0, BAR0_00D0, 0x4100);

	/* Enable the pipeline BEFORE starting DMA.
	 * On 4K Pro, A8 takes ~100ms to become non-zero after D0|=1.
	 * The XDMA C2H engine stalls if started without stream data.
	 */
	sc_set(dev, 0, BAR0_00D0, 0x0001);

	/* Enable scaler-to-DMA data path (4K Pro only). */
	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, 0xEC, 0x00000001);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
		int poll;
		u32 a8;
		for (poll = 0; poll < 20; poll++) {
			msleep(100);
			a8 = sc_read(dev, 0, 0xa8);
			if (a8 != 0) {
				printk(KERN_INFO "%s: A8 active after %dms: %08x\n",
					dev->name, (poll + 1) * 100, a8);
				break;
			}
		}
		if (a8 == 0)
			printk(KERN_WARNING "%s: A8 still 0 after 2s — DMA may stall\n",
				dev->name);
	}

	/* Start all DMA channels after pipeline is active. */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_start(&dev->channel[i]);
	}

	return 0;
}

/* Called every 2m in polled DMA mode, check
 * each dma channel. If writeback metadata suggests a transfer
 * has completed, process it and hand the audio/video to linux
 * subsystems.
 */
int sc0710_dma_channels_service(struct sc0710_dev *dev)
{
	int i, ret;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_service(&dev->channel[i]);
	}

	return 0;
}
