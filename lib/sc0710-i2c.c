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
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/vmalloc.h>
#include <linux/ctype.h>
#include <linux/iopoll.h>
#include <asm/io.h>

#include "sc0710.h"

#define I2C_DEV__ARM_MCU (0x32 << 1)
#define I2C_DEV__MCU_FN  (0x33 << 1) /* MCU function-call port */
#define I2C_EDID_EEPROM  (0x50 << 1)

#define SC0710_I2C_HDMI_RETRIES 3
#define SC0710_I2C_HDMI_RETRY_DELAY_US 50000
#define SC0710_LOCK_DROPOUT_MAX 5
#define SC0710_NO_TIMING_THRESHOLD 6

/* Recover the AXI IIC after an interrupted or failed transaction.
 * The RX FIFO has no reset bit, so an aborted read can leave stale bytes
 * that would offset every subsequent read. SOFTR clears the state machine
 * and both FIFOs. Caller holds signalMutex.
 */
static void sc0710_i2c_bus_reset(struct sc0710_dev *dev)
{
	sc_write(dev, 0, BAR0_3040, 0x0000000a); /* SOFTR: reset state machine + FIFOs */
	udelay(10);
	sc_write(dev, 0, BAR0_3100, 0x00000002); /* TX_FIFO Reset */
	sc_write(dev, 0, BAR0_3100, 0x00000001); /* AXI IIC Enable */
}

/* Wait until the TX FIFO has fully drained (SR bit 7 set), so the caller's
 * whole transaction, STOP-flagged byte included, is on the wire before the
 * next transaction's TX-FIFO reset can flush it. A flushed STOP truncates
 * the transaction: observed on the real card, where a truncated EDID page
 * write wedged the MCU's EEPROM parser until power-off.
 * The timeout is generous to tolerate MCU clock stretching.
 */
static int sc0710_i2c_wait_tx_drained(struct sc0710_dev *dev)
{
	u32 v;

	return read_poll_timeout(sc_read, v, v & 0x80, 20, 20000, false,
				 dev, 0, BAR0_3104);
}

/* Wait for the bus to go idle (SR bit 2, BB, clear). TX-FIFO-empty only means
 * the STOP byte entered the shifter; the STOP condition itself is still on
 * the wire, and a transaction started before it completes chops it off. The
 * vendor polls for idle (SR = 0xc0) before every page write in traced
 * sessions. */
static int sc0710_i2c_wait_bus_idle(struct sc0710_dev *dev)
{
	u32 v;

	return read_poll_timeout(sc_read, v, !(v & 0x04), 20, 20000, false,
				 dev, 0, BAR0_3104);
}

/* Read one byte from the RX FIFO, waiting up to ~3.2 ms for it to arrive.
 * Returns the byte (0..255) or -ETIMEDOUT: never a byte read from an
 * empty FIFO, which the controller answers with meaningless filler. */
static int busread(struct sc0710_dev *dev)
{
	u32 v;
	int ret;

	/* RX_FIFO_Empty (bit 6) clear means data available.
	 * MK2 sees 0x8C/0xAC; 4K Pro may differ in bus-busy/SRW bits. */
	ret = read_poll_timeout(sc_read, v, !(v & 0x40), 100, 3200, false,
				dev, 0, BAR0_3104);
	if (ret < 0)
		return -ETIMEDOUT;
	return sc_read(dev, 0, BAR0_310C) & 0xFF;
}

/* Standalone I2C read: its own START, a STOP-flagged read length, no write
 * phase. Also serves as the read phase of the writeread transaction, which
 * enters here mid-transaction after its repeated START, so this must not
 * wait for bus idle itself. Caller holds signalMutex. */
static int __sc0710_i2c_read_once(struct sc0710_dev *dev, u8 devaddr8bit, u8 *rbuf, int rlen)
{
	u32 v;
	int cnt;
	unsigned long timeout = jiffies + msecs_to_jiffies(500);

	sc_write(dev, 0, BAR0_3120, 0x0000000f);
	sc_write(dev, 0, BAR0_3100, 0x00000002); /* TX_FIFO Reset */
	sc_write(dev, 0, BAR0_3100, 0x00000000);
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 8) /* Start Bit */ | (devaddr8bit | 1));
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 9) /* Stop Bit */ | rlen);
	sc_write(dev, 0, BAR0_3100, 0x00000001);

	/* Read the reply. Fail the whole transaction unless every byte actually
	 * arrived: never hand fabricated bytes to callers with rc 0. */
	cnt = 0;
	while (cnt < rlen) {
		int b;
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: I2C timeout reading data\n", __func__);
			return -ETIMEDOUT;
		}
		b = busread(dev);
		if (b < 0) {
			printk_ratelimited(KERN_WARNING "%s: RX byte %d/%d never arrived\n",
				__func__, cnt, rlen);
			return -ETIMEDOUT;
		}
		*(rbuf + cnt) = b;
		cnt++;
	}
	v = sc_read(dev, 0, BAR0_3104);
	/* Completion: TX_FIFO_Empty (bit 7) + RX_FIFO_Empty (bit 6) = all done.
	 * MK2 sees 0xC8/0xCC; 4K Pro may differ in SRW/BB bits.
	 */
	if ((v & 0xC0) != 0xC0) {
		printk_ratelimited(KERN_WARNING "%s: I2C completion check failed (SR 0x%08x)\n",
			__func__, v);
		return -EIO;
	}

	return 0; /* Success */
}

/* Standalone-read wrapper: enter with the bus idle, reset the engine on
 * failure so stale RX bytes can't offset the next read. */
static int __sc0710_i2c_read(struct sc0710_dev *dev, u8 devaddr8bit, u8 *rbuf, int rlen)
{
	int ret;

	if (sc0710_i2c_wait_bus_idle(dev) < 0)
		ret = -EIO;
	else
		ret = __sc0710_i2c_read_once(dev, devaddr8bit, rbuf, rlen);
	if (ret < 0)
		sc0710_i2c_bus_reset(dev);
	return ret;
}

#if 1 /* Enable I2C write for MCU commands */
/* Assumes 8 bit device address; every byte of wbuf goes on the wire.
 * One byte in flight at a time: drain the FIFO before each push and after
 * the STOP-flagged one, so the transaction is fully on the wire when we
 * return and the next transaction's FIFO reset can't truncate it. */
static int __sc0710_i2c_write_once(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen)
{
	int i;
	u32 v;

	/* Enter and leave with the bus idle, like the vendor's traced writes:
	 * a transaction started while the previous STOP is still shifting out
	 * gets that STOP chopped by the TX-FIFO reset below. */
	if (sc0710_i2c_wait_bus_idle(dev) < 0)
		return -EIO;

	/* Write out to the i2c bus master a reset, then write length and device address */
	sc_write(dev, 0, BAR0_3100, 0x00000002); /* TX_FIFO Reset */
	sc_write(dev, 0, BAR0_3100, 0x00000001); /* AXI IIC Enable */
	sc_write(dev, 0, BAR0_3108, 0x00000000 | (1 << 8) /* Start Bit */ | devaddr8bit);

	for (i = 0; i < wlen; i++) {
		if (sc0710_i2c_wait_tx_drained(dev) < 0)
			return -EIO;
		v = 0x00000000 | *(wbuf + i);
		if (i == (wlen - 1))
			v |= (1 << 9); /* Stop Bit */
		sc_write(dev, 0, BAR0_3108, v);
	}
	if (sc0710_i2c_wait_tx_drained(dev) < 0)
		return -EIO;
	if (sc0710_i2c_wait_bus_idle(dev) < 0)
		return -EIO;

	return 0; /* Success */
}

/* A failed write can leave the IIC engine mid-transaction; reset it so the
 * next transaction starts clean instead of reading the wreckage. */
static int sc0710_i2c_write(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen)
{
	int ret = __sc0710_i2c_write_once(dev, devaddr8bit, wbuf, wlen);
	if (ret < 0)
		sc0710_i2c_bus_reset(dev);
	return ret;
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

static int __sc0710_i2c_writeread_once(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	u32 v;
	u8 i2c_devaddr = devaddr8bit; /* From dev 64, read 0x1a bytes from subaddress 0 */
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
	return __sc0710_i2c_read_once(dev, i2c_devaddr, rbuf, rlen);
}

/* A timed-out or incomplete read leaves stale bytes in the RX FIFO that
 * would offset every subsequent read; reset the engine on any failure. */
static int __sc0710_i2c_writeread(struct sc0710_dev *dev, u8 devaddr8bit, u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	int ret = __sc0710_i2c_writeread_once(dev, devaddr8bit, wbuf, wlen, rbuf, rlen);
	if (ret < 0)
		sc0710_i2c_bus_reset(dev);
	return ret;
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

	/* Hold the DMA service lock for the entire sequence, including the
	 * fmt/state/refcount snapshot: STREAMON and STREAMOFF take the same
	 * lock for their DMA transitions, so a client can't appear or vanish
	 * between the snapshot and the restart below (a stale snapshot could
	 * leave DMA running with zero clients).  The DMA thread checks
	 * reconfig_in_progress under the same lock, so service cannot run
	 * while we reconfigure.
	 */
	mutex_lock(&dev->kthread_dma_lock);

	if (!dev->fmt) {
		printk(KERN_INFO "%s: No format detected, skipping DMA reset\n", dev->name);
		goto out_unlock;
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
		goto out_unlock;
	}

	printk(KERN_INFO "%s: Signal restoration - DMA was %s, have streaming clients\n",
		dev->name, dma_was_running ? "running" : "stopped");

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

	/* Phase 2: Resize DMA buffers for the new resolution.
	 * On failure leave the channels stopped: restarting over a missing ring
	 * would hand the engine a NULL descriptor pointer. The next timing
	 * change or STREAMON retries the resize. */
	if (sc0710_dma_channels_resize(dev) < 0) {
		printk(KERN_ERR "%s: DMA resize failed during resync; leaving channels stopped\n",
			dev->name);

		/* Phase 1 stopped the video XDMA engines; clear GO to match
		 * (mirrors sc0710_dma_channels_stop). */
		sc_clr(dev, 0, BAR0_00D0, 0x0001);

		/* Phase 1 deleted the frame timers; re-arm them so streaming
		 * clients get placeholder frames instead of blocking in DQBUF.
		 * Audio has no timer (only video registration sets one up). */
		for (ch_idx = 0; ch_idx < SC0710_MAX_CHANNELS; ch_idx++) {
			ch = &dev->channel[ch_idx];
			if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
				continue;
			if (atomic_read(&ch->streaming_refcount) <= 0)
				continue;
			mutex_lock(&ch->lock);
			mod_timer(&ch->timeout, jiffies + VBUF_TIMEOUT);
			mutex_unlock(&ch->lock);
		}

		dev->reconfig_in_progress = 0;
		mutex_unlock(&dev->kthread_dma_lock);
		return;
	}

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
	return;

out_unlock:
	mutex_unlock(&dev->kthread_dma_lock);
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

		/* Upper bounds: a glitched read can hand back 0xFF filler
		 * with rc 0, and 0xFFFF-ish dims would u32-overflow framesize
		 * (w*2*h) into a tiny value handed to DMA sizing. 4096 covers
		 * everything the card can emit (DCI 4K), interlace-doubling
		 * included, and keeps w*2*h far below overflow. */
		if (!dev->fmt &&
		    procedural_timings != TIMING_MODE_STATIC_ONLY &&
		    dev->width >= 320 && dev->height >= 200 &&
		    dev->width <= 4096 && dev->height <= 4096) {
			struct sc0710_format *dyn;
			u32 fps_est = fps_target ? fps_target : 60;

			/* Build into the slot dev->fmt does NOT point at, publish
			 * last: a holder of the previous dynamic fmt sees stable
			 * (possibly stale) data, never a torn rewrite. */
			dev->dynamic_fmt_idx ^= 1;
			dyn = &dev->dynamic_fmt[dev->dynamic_fmt_idx];

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
			snprintf(dev->dynamic_fmt_name[dev->dynamic_fmt_idx],
				 sizeof(dev->dynamic_fmt_name[0]),
				 "%ux%u%s%u(dynamic)",
				 dev->width, dev->height,
				 dev->interlaced ? "i" : "p",
				 fps_est);
			dyn->name = dev->dynamic_fmt_name[dev->dynamic_fmt_idx];

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
		/* Called every HDMI-poll tick; don't flood the log on a sick bus. */
		printk_ratelimited("%s ret = %d\n", __func__, ret);
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

/* Factory EDID base block bytes 0x00-0x5F, matching the stock EDID that
 * Elgato Studio programs. Written only when the EEPROM header is missing or
 * corrupt (e.g. after an interrupted page write); the EEPROM itself is
 * non-volatile, persisting across AC power loss in traced sessions.
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

/* An EDID block (base or extension) is valid when its 128 bytes sum to 0 mod 256. */
static bool sc0710_edid_block_checksum(const u8 *block)
{
	u8 sum = 0;
	int i;
	for (i = 0; i < 128; i++)
		sum += block[i];
	return sum == 0;
}

/* The 8-byte EDID base-block header magic (needs p[0..7] readable). */
bool sc0710_edid_header_valid(const u8 *p)
{
	return p[0] == 0x00 && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF &&
	       p[4] == 0xFF && p[5] == 0xFF && p[6] == 0xFF && p[7] == 0x00;
}

/* The 4K Pro "EEPROM" is a 512-byte space emulated by the MCU, addressed in
 * 16-byte pages by a single offset byte that carries the HIGH address bits in
 * its LOW nibble (decoded from a traced Windows-driver EDID-switch session):
 *     addr = ((offset & 0x0f) << 8) | (offset & 0xf0)
 * so linear offsets land in the wrong page: offset byte 0x01 is byte 0x100,
 * not byte 1. Every traced transaction moves exactly 16 data bytes. */
static u8 sc0710_edid_offset_byte(int addr)
{
	return ((addr >> 8) & 0x0f) | (addr & 0xf0);
}

/* A full EDID image is valid when it carries the header magic, is whole
 * 128-byte blocks within the 512-byte EEPROM, declares exactly its own
 * extension blocks in byte 126 (a truncated image would otherwise be
 * zero-padded into "valid" all-zero extensions the source then reads),
 * and every block checksums. */
static bool sc0710_edid_image_valid(const u8 *edid, size_t len)
{
	size_t i;
	bool valid = len >= 128 && len <= 512 && (len % 128) == 0 &&
		sc0710_edid_header_valid(edid) &&
		edid[126] == len / 128 - 1;

	for (i = 0; valid && i < len; i += 128)
		valid = sc0710_edid_block_checksum(edid + i);
	return valid;
}

/* Write `len` EDID bytes from address 0, 16 bytes per I2C transaction as the
 * Windows driver does, zero-filling up to `total` so a shorter profile clears
 * stale extension blocks (Elgato Studio pads to 512 the same way). No delay or
 * ACK-polling between pages: the traced writes have none (no real EEPROM
 * write cycle behind the MCU). On-wire bytes are [offset][16 data].
 * Caller holds signalMutex.
 *
 * 4K Pro ONLY: the single wire-level writer under the whole EDID stack
 * (factory restore and edid= profiles both land here). The nibble-paged
 * offset encoding is the 4KP MCU's; on an MK2's real, linearly-addressed
 * EEPROM these offsets would scribble garbage. */
static int sc0710_write_edid_bytes(struct sc0710_dev *dev, const u8 *edid, int len, int total)
{
	int addr, j, ret;

	if (WARN_ON_ONCE(dev->board != SC0710_BOARD_ELGATEO_4KP))
		return -ENODEV;

	for (addr = 0; addr < total; addr += 16) {
		u8 wr[17];
		wr[0] = sc0710_edid_offset_byte(addr);
		for (j = 0; j < 16; j++)
			wr[j + 1] = (addr + j < len) ? edid[addr + j] : 0x00;
		ret = sc0710_i2c_write(dev, I2C_EDID_EEPROM, wr, 17);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Read one 16-byte EEPROM page at addr (the offset-then-read shape of the
 * Windows driver's verify sweeps). Caller holds signalMutex. 4K Pro only
 * (nibble-paged offsets), but read-only; misuse can't damage anything. */
static int sc0710_read_edid_page(struct sc0710_dev *dev, int addr, u8 *rd)
{
	u8 off = sc0710_edid_offset_byte(addr);

	return __sc0710_i2c_writeread(dev, I2C_EDID_EEPROM, &off, 1, rd, 16);
}

/* Compare the EEPROM against this (zero-padded to `total`) image.
 * Returns 1 if identical, 0 on any difference, negative on read error.
 * `logerror` reports the first mismatch at KERN_ERR.
 * Caller holds signalMutex. */
static int sc0710_edid_pages_match(struct sc0710_dev *dev, const u8 *edid, int len, int total, bool logerror)
{
	int addr, j, ret;

	for (addr = 0; addr < total; addr += 16) {
		u8 rd[16];

		ret = sc0710_read_edid_page(dev, addr, rd);
		if (ret < 0)
			return ret;
		for (j = 0; j < 16; j++) {
			u8 want = (addr + j < len) ? edid[addr + j] : 0x00;
			if (rd[j] != want) {
				if (logerror)
					printk(KERN_ERR "%s: EDID verify mismatch at 0x%03x: wrote %02x, read %02x\n",
						dev->name, addr + j, want, rd[j]);
				return 0;
			}
		}
	}
	return 1;
}

/* Read the EEPROM back and compare against what sc0710_write_edid_bytes(dev,
 * edid, len, total) wrote. Returns 0 on match, -EIO on mismatch (loudly).
 * Caller holds signalMutex. */
static int sc0710_verify_edid_bytes(struct sc0710_dev *dev, const u8 *edid, int len, int total)
{
	int ret = sc0710_edid_pages_match(dev, edid, len, total, true);

	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

/* Program the EEPROM with this EDID image, zero-padded to the full 512 bytes.
 * Skips the write (and the MCU's HPD bounce) when the image is already current;
 * a failed compare read aborts rather than writing through a bus that just
 * failed. Shared by the edid= path and VIDIOC_S_EDID.
 * Caller holds signalMutex. 4K Pro only (enforced by the page writer). */
static int sc0710_edid_program(struct sc0710_dev *dev, const u8 *edid, int len)
{
	int ret;

	ret = sc0710_edid_pages_match(dev, edid, len, 512, false);
	if (ret < 0)
		return ret;
	if (ret == 1) {
		printk(KERN_INFO "%s: EDID already current, skipping write\n", dev->name);
		return 0;
	}

	printk(KERN_INFO "%s: writing EDID (%d bytes) to EEPROM\n", dev->name, len);
	ret = sc0710_write_edid_bytes(dev, edid, len, 512);
	if (ret == 0)
		ret = sc0710_verify_edid_bytes(dev, edid, len, 512);
	if (ret == 0)
		printk(KERN_INFO "%s: EDID verified; the MCU re-raises HPD so the "
			"source re-reads it\n", dev->name);
	return ret;
}

static void sc0710_write_factory_edid(struct sc0710_dev *dev)
{
	u8 w[1] = { 0x00 };
	u8 r[8] = { 0 };

	/* A failed read means the bus is sick, not that the EDID is missing;
	 * writing through it would only dig deeper. */
	if (__sc0710_i2c_writeread(dev, I2C_EDID_EEPROM, w, sizeof(w), r, sizeof(r)) < 0) {
		printk(KERN_WARNING "%s: EDID header read failed, skipping factory-EDID check\n",
			dev->name);
		return;
	}

	if (!sc0710_edid_header_valid(r)) {
		printk(KERN_INFO "%s: EDID header invalid (%02x %02x ...), writing factory data\n",
			dev->name, r[0], r[1]);
		/* No zero-fill past the 96 bytes: 0x60-0xFF persist and complete the block. */
		sc0710_write_edid_bytes(dev, factory_edid_base, sizeof(factory_edid_base),
			sizeof(factory_edid_base));
	}
}

/* Present a specific EDID to the HDMI source: load /lib/firmware/sc0710/edid/<name>.bin
 * (installed from the Elgato Studio package by scripts/extract-firmware.sh) and
 * write it to the EEPROM, overriding whatever is there. Selected by the edid=
 * module param. Returns 0 on success or negative on error (caller then falls back
 * to the factory EDID). Caller holds signalMutex.
 *
 * No HPD kick is needed: the MCU drops and re-raises HPD toward the source by
 * itself after an EEPROM write. In the traced Windows-driver EDID switch the
 * source re-locked at the new mode within ~1 s of the last page, with no MCU
 * command in between. */
static int sc0710_write_edid_profile(struct sc0710_dev *dev, const char *name)
{
	u8 *edid;
	size_t len = 0;
	char rel[64];
	int i, ret;

	/* Profile names are plain tokens (extract-firmware.sh generates them);
	 * refuse anything that could escape the firmware directory. */
	for (i = 0; name[i]; i++) {
		if (!isalnum(name[i]) && name[i] != '-') {
			printk(KERN_ERR "%s: invalid EDID profile name '%s'\n",
				dev->name, name);
			return -EINVAL;
		}
	}

	snprintf(rel, sizeof(rel), "edid/%s.bin", name);
	edid = sc0710_firmware_load(dev, rel, &len);
	if (!edid) {
		printk(KERN_ERR "%s: EDID profile '%s' not found "
			"(/lib/firmware/sc0710/edid/%s.bin); install the profiles "
			"with scripts/extract-firmware.sh\n", dev->name, name, name);
		return -ENOENT;
	}

	if (!sc0710_edid_image_valid(edid, len)) {
		printk(KERN_ERR "%s: EDID profile '%s' is not a valid EDID (%zu bytes)\n",
			dev->name, name, len);
		ret = -EINVAL;
	} else {
		printk(KERN_INFO "%s: EDID profile '%s' (%zu bytes)\n",
			dev->name, name, len);
		ret = sc0710_edid_program(dev, edid, (int)len);
	}

	vfree(edid);
	return ret;
}

/* Wait for the 4KP FPGA pipeline to report active (A8 != 0) after GO.
 * Runs after the engines are started and GO is set; with GO-last ordering the
 * pipeline can't be active any earlier. Diagnostic-grade: the vendor driver
 * never reads A8 in traced sessions, so a timeout here is
 * a warning, not a failure. The MCU diagnostic read serializes itself on
 * signalMutex; no caller of the channels_start path holds it, but the
 * HDMI poll thread's I2C runs concurrently on the same AXI IIC engine.
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

	/* MCU TX status for diagnostics. On failure rbuf holds nothing useful. */
	wbuf[0] = 0x10;
	if (sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, wbuf, 1, rbuf, 16) == 0)
		printk(KERN_INFO "%s: MCU status [13-15]: %02x %02x %02x\n",
			dev->name, rbuf[3], rbuf[4], rbuf[5]);
	else
		printk(KERN_INFO "%s: MCU status read failed\n", dev->name);

	/* Poll A8: the 4KP FPGA pipeline may need time to become active after
	 * the D0/EC register writes. Typically activates within 100ms. The
	 * budget is short because this runs under kthread_dma_lock, where it
	 * stalls STREAMON/STREAMOFF; the run-bit verify in the resync path is
	 * the functional check, this poll is diagnostic only.
	 */
	for (i = 0; i < 5; i++) {
		msleep(100);
		a8 = sc_read(dev, 0, 0xa8);
		if (a8 != 0) {
			printk(KERN_INFO "%s: A8 active after %dms: %08x\n",
				dev->name, (i + 1) * 100, a8);
			return 0;
		}
	}

	printk(KERN_WARNING "%s: A8 still 0 after 500ms, DMA may stall\n", dev->name);
	return 0;
}

/* MCU function call: a STOP-terminated command write, then the MCU's fixed
 * 3-byte ack read as a standalone transaction, polled over a bounded window
 * because the ack arrival time is not known precisely.
 * On an ack mismatch do not re-read: whether the ack survives a second read
 * is unobserved in traced vendor-driver sessions.
 * Caller holds signalMutex so the HDMI keepalive cannot interleave.
 */
static int sc0710_4kp_mcu_call(struct sc0710_dev *dev, u8 fn, const u8 *expected_ack)
{
	u8 cmd[5] = { 0xab, 0x03, 0x12, 0x34, fn };
	u8 ack[3];
	int attempt, ret;

	ret = sc0710_i2c_write(dev, I2C_DEV__MCU_FN, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	msleep(20);
	for (attempt = 0; attempt < 5; attempt++) {
		if (attempt)
			msleep(50);
		ret = __sc0710_i2c_read(dev, I2C_DEV__MCU_FN, ack, sizeof(ack));
		if (ret == 0)
			break;
	}
	if (ret < 0) {
		printk(KERN_ERR "%s: MCU function 0x%02x: no ack (%d)\n",
			dev->name, fn, ret);
		return ret;
	}
	if (memcmp(ack, expected_ack, sizeof(ack))) {
		printk(KERN_ERR "%s: MCU function 0x%02x: ack %02x %02x %02x, expected %02x %02x %02x\n",
			dev->name, fn, ack[0], ack[1], ack[2],
			expected_ack[0], expected_ack[1], expected_ack[2]);
		sc0710_i2c_bus_reset(dev);
		return -EIO;
	}
	return 0;
}

/* EDID source selectors, indexed by enum sc0710_edid_source.
 * One call switches the source; the MCU bounces HPD toward the source by
 * itself and the source renegotiates within ~10 s.
 * The MCU offers no readback for the current selection.
 * Shared by both boards: the MK2 MCU speaks the same protocol with the
 * same fn codes and ack signatures, verified on real hardware (probe
 * tiers 2-9; GPU-side EDID captures confirmed the switches take effect).
 * The MK2 uses the polled-ack call variant below.
 */
static const struct {
	u8 fn;
	u8 ack[3];
	const char *name;
} sc0710_edid_sources[] = {
	[SC0710_EDID_SOURCE_INTERNAL] = { 0x5f, { 0x11, 0x22, 0x66 }, "internal" },
	[SC0710_EDID_SOURCE_DISPLAY]  = { 0x60, { 0x11, 0x22, 0x77 }, "display" },
	[SC0710_EDID_SOURCE_MERGED]   = { 0x61, { 0x11, 0x22, 0x88 }, "merged" },
};

/* Caller holds signalMutex. */
static int __sc0710_4kp_set_edid_source(struct sc0710_dev *dev, u32 src)
{
	int ret;

	if (src >= ARRAY_SIZE(sc0710_edid_sources))
		return -EINVAL;

	ret = sc0710_4kp_mcu_call(dev, sc0710_edid_sources[src].fn,
				  sc0710_edid_sources[src].ack);
	if (ret == 0)
		printk(KERN_INFO "%s: EDID source set to %s\n",
			dev->name, sc0710_edid_sources[src].name);
	return ret;
}

/* MK2 variant of the MCU function call. Same command bytes and ack
 * signatures as the 4K Pro, but the MK2's response buffer updates
 * asynchronously: it keeps returning the PREVIOUS response (with a clean
 * rc) until the MCU finishes processing, up to ~2 s observed on real
 * hardware. So poll until the expected ack appears instead of judging the
 * first read; a stale ack is progress-pending, not failure.
 * Caller holds signalMutex. */
static int sc0710_mk2_mcu_call(struct sc0710_dev *dev, u8 fn, const u8 *expected_ack)
{
	u8 ack[3];
	u8 cmd[5] = { 0xab, 0x03, 0x12, 0x34, fn };
	unsigned long timeout;
	int ret;

	ret = sc0710_i2c_write(dev, I2C_DEV__MCU_FN, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	timeout = jiffies + msecs_to_jiffies(2500);
	for (;;) {
		msleep(100);
		memset(ack, 0, sizeof(ack));
		ret = __sc0710_i2c_read(dev, I2C_DEV__MCU_FN, ack, sizeof(ack));
		if (ret == 0 && memcmp(ack, expected_ack, sizeof(ack)) == 0)
			return 0;
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: MCU function 0x%02x: expected ack %3ph never arrived (last %3ph, rc=%d)\n",
				dev->name, fn, expected_ack, ack, ret);
			return -EIO;
		}
	}
}

/* Caller holds signalMutex. */
static int __sc0710_mk2_set_edid_source(struct sc0710_dev *dev, u32 src)
{
	int ret;

	if (src >= ARRAY_SIZE(sc0710_edid_sources))
		return -EINVAL;

	ret = sc0710_mk2_mcu_call(dev, sc0710_edid_sources[src].fn,
				  sc0710_edid_sources[src].ack);
	if (ret == 0)
		printk(KERN_INFO "%s: EDID source set to %s\n",
			dev->name, sc0710_edid_sources[src].name);
	return ret;
}

/* Shared entry for the EDID Source control: each board keeps its own
 * verified call variant underneath. */
int sc0710_set_edid_source(struct sc0710_dev *dev, u32 src)
{
	int ret;

	mutex_lock(&dev->signalMutex);
	if (dev->board == SC0710_BOARD_ELGATEO_4KP)
		ret = __sc0710_4kp_set_edid_source(dev, src);
	else if (dev->board == SC0710_BOARD_ELGATEO_4KP60_MK2)
		ret = __sc0710_mk2_set_edid_source(dev, src);
	else
		ret = -ENODEV;
	mutex_unlock(&dev->signalMutex);
	return ret;
}

/* Sanity-check the MCU before any EEPROM work. Against a deaf MCU (wedge
 * state: it NAKs every address until power-off) the read path fabricates
 * constant NONZERO filler from the empty RX FIFO while reporting success
 * (constant 0x80 observed on the real card), and writing through that
 * state is pointless. An all-zero blob is NOT a wedge: a freshly
 * cold-booted card with the HDMI source off legitimately reports zeros,
 * and edid= with no source is a valid use. Transient first-read hiccups
 * get retried. Caller holds signalMutex. */
static bool sc0710_mcu_sane(struct sc0710_dev *dev)
{
	u8 w[1] = { 0x00 };
	u8 blob[16];
	int attempt, i, rc = -EIO;

	for (attempt = 0; attempt < 3; attempt++) {
		if (attempt)
			msleep(100);
		memset(blob, 0, sizeof(blob));
		rc = __sc0710_i2c_writeread(dev, I2C_DEV__ARM_MCU, w, sizeof(w),
			blob, sizeof(blob));
		if (rc < 0)
			continue;
		for (i = 1; i < (int)sizeof(blob); i++) {
			if (blob[i] != blob[0])
				return true;
		}
		if (blob[0] == 0x00)
			return true;
		printk(KERN_ERR "%s: MCU status reads constant 0x%02x filler (wedge signature)\n",
			dev->name, blob[0]);
		return false;
	}
	printk(KERN_ERR "%s: MCU status read failed (%d) after 3 attempts\n", dev->name, rc);
	return false;
}

/* MK.2 EDID read, transcribed from ReadEDID() in the Windows kernel driver
 * SC0710.X64.SYS (@0x14024fd34). It is entirely MCU-mediated through the safe
 * I2C_DEV__MCU_FN (0x33) port -- no 0x50 EEPROM, no raw pokes -- so it is
 * usable any time the MCU is alive, with or without a live HDMI signal:
 *
 *   BEGIN: write [ab 03 12 34 <src>] to 0x33, poll a 3-byte read until it
 *          returns {51 10 35} ("EDID ready"); src selects which image
 *          (0x5c internal / 0x62 pass-monitor / 0x68 merged).
 *   BODY:  for each 16-byte offset: write [55 <off>] to set the read pointer,
 *          then read 16 bytes back from 0x33.
 *   END:   write [ab 03 12 34 cc], read 3 bytes ({51 10 31} = done).
 *
 * `start` and `len` are multiples of 16 within a single 256-byte image.
 * Caller holds signalMutex. */
#define SC0710_MK2_EDID_SRC_INTERNAL	0x5c
static int sc0710_mk2_read_edid(struct sc0710_dev *dev, u8 *buf,
				int start, int len)
{
	static const u8 begin_ack[3] = { 0x51, 0x10, 0x35 };
	u8 begin[5] = { 0xab, 0x03, 0x12, 0x34, SC0710_MK2_EDID_SRC_INTERNAL };
	u8 end[5]   = { 0xab, 0x03, 0x12, 0x34, 0xcc };
	unsigned long timeout;
	u8 ack[3];
	int off, ret;

	if (start < 0 || len <= 0 || (start & 15) || (len & 15) ||
	    start + len > 256)
		return -EINVAL;

	/* BEGIN: ask the MCU to expose the internal EDID for reading. */
	ret = sc0710_i2c_write(dev, I2C_DEV__MCU_FN, begin, sizeof(begin));
	if (ret < 0)
		return ret;

	timeout = jiffies + msecs_to_jiffies(2500);
	for (;;) {
		msleep(100);
		memset(ack, 0, sizeof(ack));
		ret = __sc0710_i2c_read(dev, I2C_DEV__MCU_FN, ack, sizeof(ack));
		if (ret == 0 && memcmp(ack, begin_ack, sizeof(ack)) == 0)
			break;
		if (time_after(jiffies, timeout)) {
			printk(KERN_ERR "%s: EDID read: MCU not ready (last %3ph, rc=%d)\n",
				dev->name, ack, ret);
			return -EIO;
		}
	}

	/* BODY: pull 16 bytes at a time. */
	for (off = start; off < start + len; off += 16) {
		u8 ptr[2] = { 0x55, (u8)off };

		ret = sc0710_i2c_write(dev, I2C_DEV__MCU_FN, ptr, sizeof(ptr));
		if (ret < 0)
			return ret;
		usleep_range(2000, 3000);
		ret = __sc0710_i2c_read(dev, I2C_DEV__MCU_FN,
					buf + (off - start), 16);
		if (ret < 0)
			return ret;
		usleep_range(2000, 3000);
	}

	/* END: best-effort close; the bytes are already in hand. */
	if (sc0710_i2c_write(dev, I2C_DEV__MCU_FN, end, sizeof(end)) == 0) {
		msleep(2);
		__sc0710_i2c_read(dev, I2C_DEV__MCU_FN, ack, sizeof(ack));
	}

	return 0;
}

/* Runtime EDID read for VIDIOC_G_EDID. Fills `len` bytes of the EDID image
 * from `start` (both multiples of 16). The 4K Pro reads its EEPROM directly;
 * the MK.2 reads the MCU-served internal image (256 bytes max). */
int sc0710_i2c_get_edid(struct sc0710_dev *dev, u8 *buf, int start, int len)
{
	int addr, ret = 0;

	mutex_lock(&dev->signalMutex);
	if (dev->board == SC0710_BOARD_ELGATEO_4KP60_MK2) {
		ret = sc0710_mk2_read_edid(dev, buf, start, len);
		mutex_unlock(&dev->signalMutex);
		return ret;
	}
	for (addr = start; addr + 16 <= start + len; addr += 16) {
		ret = sc0710_read_edid_page(dev, addr, buf + (addr - start));
		if (ret < 0)
			break;
	}
	mutex_unlock(&dev->signalMutex);
	return ret;
}

/* MK.2 custom EDID write (fn 0x59 UpdateEDID protocol); defined below. */
static int sc0710_mk2_write_edid(struct sc0710_dev *dev, const u8 *edid, int len);

/* Runtime EDID write for VIDIOC_S_EDID: validate, refuse a sick MCU, skip
 * if already current (no needless HPD bounce), else write and verify, the
 * same protections as the probe-time edid= path. On the 4K Pro the EEPROM is
 * the internal-mode slot, so the internal EDID source is forced first to make
 * the written EDID visible even if the card was left in another mode; the MCU
 * re-raises HPD itself. The MK.2 uses the fn 0x59 MCU write path instead. */
int sc0710_i2c_set_edid(struct sc0710_dev *dev, const u8 *edid, int len)
{
	int ret;

	if (!sc0710_edid_image_valid(edid, len))
		return -EINVAL;

	mutex_lock(&dev->signalMutex);

	if (dev->board == SC0710_BOARD_ELGATEO_4KP60_MK2) {
		/* MK.2: fn 0x59 UpdateEDID protocol to the frontend MCU, entirely
		 * on the safe 0x33 port. Verify afterward with VIDIOC_G_EDID. */
		ret = sc0710_mk2_write_edid(dev, edid, len);
	} else if (!sc0710_mcu_sane(dev)) {
		printk(KERN_ERR "%s: refusing EDID write, bus/MCU unhealthy\n", dev->name);
		ret = -EIO;
	} else {
		if (__sc0710_4kp_set_edid_source(dev, SC0710_EDID_SOURCE_INTERNAL) < 0)
			printk(KERN_WARNING "%s: could not force the internal EDID source, writing anyway\n",
				dev->name);
		ret = sc0710_edid_program(dev, edid, len);
	}

	mutex_unlock(&dev->signalMutex);
	return ret;
}

/* MK.2 EDID write, transcribed from UpdateEDID() in SC0710.X64.SYS
 * (@0x1402571d0) -- the normal (non-debug) write path, and the mirror of
 * sc0710_mk2_read_edid: fully MCU-mediated on the safe I2C_DEV__MCU_FN (0x33)
 * port, no 0x50 EEPROM. Frames are [ab 03 <op> 34 <fn>] (op 0x12 = command,
 * op 0xcc = commit):
 *
 *   BEGIN:  write [ab 03 12 34 59], poll a 3-byte read until {51 10 30}
 *           ("ready to receive"). fn 0x59 puts the MCU into EDID-receive mode.
 *   STREAM: write the 256-byte image as sixteen plain 16-byte writes to 0x33;
 *           the MCU tracks the offset itself.
 *   COMMIT: write [ab 03 cc 34 59], poll until {51 10 31} ("stored").
 *
 * SAFETY: streaming bare bytes to 0x33 is only safe AFTER the {51 10 30} ack
 * (the MCU is then expecting the stream); a bare write outside receive mode
 * wedges the MCU, so we abort without streaming if BEGIN never acks. */
static int sc0710_mk2_write_edid(struct sc0710_dev *dev, const u8 *edid, int len)
{
	static const u8 begin_ack[3] = { 0x51, 0x10, 0x30 };
	static const u8 end_ack[3]   = { 0x51, 0x10, 0x31 };
	u8 begin[5]  = { 0xab, 0x03, 0x12, 0x34, 0x59 };
	u8 commit[5] = { 0xab, 0x03, 0xcc, 0x34, 0x59 };
	unsigned long timeout;
	u8 ack[3];
	int off, rc, ready = 0;

	if (WARN_ON_ONCE(dev->board != SC0710_BOARD_ELGATEO_4KP60_MK2))
		return -ENODEV;
	/* The MCU streams a fixed 256-byte image (16 x 16-byte writes). */
	if (len != 256)
		return -EINVAL;

	/* BEGIN: request receive mode. */
	rc = sc0710_i2c_write(dev, I2C_DEV__MCU_FN, begin, sizeof(begin));
	if (rc < 0)
		return rc;
	printk(KERN_INFO "%s: MK2 EDID write: fn 0x59 begin\n", dev->name);

	timeout = jiffies + msecs_to_jiffies(5500);
	for (;;) {
		msleep(100);
		memset(ack, 0, sizeof(ack));
		rc = __sc0710_i2c_read(dev, I2C_DEV__MCU_FN, ack, sizeof(ack));
		if (rc == 0 && memcmp(ack, begin_ack, sizeof(ack)) == 0) {
			ready = 1;
			break;
		}
		if (time_after(jiffies, timeout))
			break;
	}
	if (!ready) {
		printk(KERN_ERR "%s: MK2 EDID write: MCU never entered receive mode (last %3ph); NOT streaming\n",
			dev->name, ack);
		return -EIO;
	}

	/* STREAM: sixteen 16-byte writes; the MCU advances its own pointer. */
	msleep(10);
	for (off = 0; off < len; off += 16) {
		rc = sc0710_i2c_write(dev, I2C_DEV__MCU_FN, (u8 *)edid + off, 16);
		if (rc < 0) {
			printk(KERN_ERR "%s: MK2 EDID write: chunk at %d failed (%d)\n",
				dev->name, off, rc);
			return rc;
		}
		msleep(10);
	}

	/* COMMIT: ask the MCU to persist, then wait for the stored ack. */
	msleep(300);
	sc0710_i2c_write(dev, I2C_DEV__MCU_FN, commit, sizeof(commit));
	timeout = jiffies + msecs_to_jiffies(2500);
	for (;;) {
		msleep(50);
		memset(ack, 0, sizeof(ack));
		rc = __sc0710_i2c_read(dev, I2C_DEV__MCU_FN, ack, sizeof(ack));
		if (rc == 0 && memcmp(ack, end_ack, sizeof(ack)) == 0) {
			printk(KERN_INFO "%s: MK2 EDID write: committed (fn 0x59)\n",
				dev->name);
			return 0;
		}
		if (time_after(jiffies, timeout))
			break;
	}
	printk(KERN_WARNING "%s: MK2 EDID write: streamed, commit ack not seen (last %3ph); verify via G_EDID\n",
		dev->name, ack);
	return 0;
}

int sc0710_i2c_initialize(struct sc0710_dev *dev)
{
	int ret;

	if (dev->board == SC0710_BOARD_ELGATEO_4KP60_MK2) {
		/* The edid= boot parameter loads from /lib/firmware and targets the
		 * 4K Pro EEPROM; the MK.2 has no such firmware install. Set a custom
		 * EDID on the MK.2 at runtime via VIDIOC_S_EDID (sc0710-cli
		 * --edid-config). */
		if (sc0710_edid_profile && sc0710_edid_profile[0])
			printk(KERN_INFO "%s: edid= is 4K-Pro-only; set a custom EDID on the "
				"MK.2 with VIDIOC_S_EDID (sc0710-cli --edid-config)\n", dev->name);
		/* The MCU keeps standby power across warm reboots, so the power-up
		 * EDID source is whatever was selected last. Force internal so the
		 * control's default reflects reality and the HDMI source always sees
		 * an EDID even with nothing on the passthrough output. */
		mutex_lock(&dev->signalMutex);
		sc0710_i2c_bus_reset(dev);
		if (__sc0710_mk2_set_edid_source(dev, SC0710_EDID_SOURCE_INTERNAL) < 0)
			printk(KERN_WARNING "%s: could not select the internal EDID source\n",
				dev->name);
		mutex_unlock(&dev->signalMutex);
		return 0;
	}
	if (dev->board != SC0710_BOARD_ELGATEO_4KP)
		return 0;

	msleep(500);

	mutex_lock(&dev->signalMutex);

	/* A previous driver instance (rmmod mid-poll) may have left the IIC
	 * engine mid-transaction with stale bytes in the RX FIFO; start clean. */
	sc0710_i2c_bus_reset(dev);

	if (!sc0710_mcu_sane(dev)) {
		printk(KERN_ERR "%s: MCU status read failed or returned constant filler; "
			"bus/MCU unhealthy, skipping all EDID/EEPROM writes "
			"(power the card off at the wall if this persists)\n", dev->name);
		mutex_unlock(&dev->signalMutex);
		return 0;
	}

	if (sc0710_edid_profile && sc0710_edid_profile[0]) {
		/* A specific profile was requested (edid=): write that full EDID.
		 * Fall back to the factory EDID only when the profile never made it
		 * to the wire (missing file, invalid name or image).
		 * After a wire failure the EEPROM may hold a partial image and the
		 * bus is sick, so report instead of writing more through it. */
		ret = sc0710_write_edid_profile(dev, sc0710_edid_profile);
		if (ret == -ENOENT || ret == -EINVAL) {
			printk(KERN_WARNING "%s: EDID profile '%s' unavailable, using factory EDID\n",
				dev->name, sc0710_edid_profile);
			sc0710_write_factory_edid(dev);
		} else if (ret < 0) {
			printk(KERN_ERR "%s: EDID write failed (%d); the EEPROM may hold a "
				"partial EDID. Reload with edid= once the bus is healthy, or "
				"power the card off at the wall\n", dev->name, ret);
		}
	} else {
		/* Write factory EDID if the EEPROM header is missing/corrupt */
		sc0710_write_factory_edid(dev);
	}

	mutex_unlock(&dev->signalMutex);

	return 0;
}

