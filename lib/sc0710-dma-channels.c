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

#include "sc0710.h"

int sc0710_dma_channels_resize(struct sc0710_dev *dev)
{
	int ret = 0;

	printk(KERN_DEBUG "%s()\n", __func__);
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		ret = sc0710_dma_channel_resize(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		/* Audio uses fixed buffer size, do not resize as it may be active via ALSA */
		/* sc0710_dma_channel_resize(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO); */
		break;
	}

	return ret;
}

int sc0710_dma_channels_alloc(struct sc0710_dev *dev)
{
	int ret = 0;

	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		ret = sc0710_dma_channel_alloc(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		if (ret == 0)
			ret = sc0710_dma_channel_alloc(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO);
		if (ret < 0)
			sc0710_dma_channels_free(dev);
		break;
	}

	return ret;
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

	printk(KERN_DEBUG "%s()\n", __func__);

	sc_clr(dev, 0, BAR0_00D0, 0x0001);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_stop(&dev->channel[i]);
	}
}

/* Program the FPGA pipeline registers (height, scaler). GO (D0 bit 0) is NOT
 * set here: the vendor driver sets it only after the XDMA engines are running
 * (GO-last on every traced vendor-driver session start), so the caller does it.
 * This is the single authoritative place for the register sequence.
 * Called from both normal stream start and DMA resync paths.
 */
void sc0710_program_pipeline_regs(struct sc0710_dev *dev)
{
	if (dev->fmt)
		sc_write(dev, 0, BAR0_00C8, dev->fmt->height);
	else
		sc_write(dev, 0, BAR0_00C8, 0x438); /* 1080 default */

	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, BAR0_00D8, 0x438);

	sc_write(dev, 0, BAR0_00D0, 0x4100);
	sc_write(dev, 0, 0xCC, 0x00000000);
	if (dev->board != SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, BAR0_00DC, 0x00000000);
	sc_write(dev, 0, BAR0_00D0, 0x4300);
	sc_write(dev, 0, BAR0_00D0, 0x4100);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc_write(dev, 0, 0xEC, 0x00000001);
}

int sc0710_dma_channels_start(struct sc0710_dev *dev)
{
	int i, ret;

	printk(KERN_DEBUG "%s()\n", __func__);

	/* Bail before touching the pipeline if any enabled channel can't start
	 * (ring missing after a failed resize): GO must never be set over a
	 * channel whose descriptor pointer would be 0. */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_start_prep(&dev->channel[i]);
		if (ret < 0) {
			printk(KERN_ERR "%s: channel %d start prep failed (%d), aborting session start\n",
				dev->name, i, ret);
			return ret;
		}
	}

	sc0710_program_pipeline_regs(dev);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_start(&dev->channel[i]);
	}

	/* GO last, once the XDMA engines are running, matching the session
	 * template observed in every traced vendor-driver start (stop ->
	 * rebuild -> program -> run engines -> GO) so frames can't flow into a
	 * stopped engine. */
	sc_set(dev, 0, BAR0_00D0, 0x0001);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		sc0710_4kp_wait_pipeline(dev);

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
		if (!dev->channel[i].enabled)
			continue;
		ret = sc0710_dma_channel_service(&dev->channel[i]);
	}

	return 0;
}
