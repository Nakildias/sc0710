/*
 *  Driver for the Elgato 4k60 Pro MK.2 and Elgato 4K Pro HDMI capture cards.
 *
 *  Copyright (c) 2021-2022 Steven Toth <stoth@kernellabs.com>
 *  Modifications Copyright (c) 2025-2026 Nakildias <nakildiaspro@gmail.com>
 *
 *  Based on the sc0710 driver by Steven Toth. Maintained as a community fork
 *  for Elgato capture cards on modern Linux kernels.
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

#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/idr.h>
#include "sc0710.h"

MODULE_DESCRIPTION("Elgato 4K60 Pro MK.2 / 4K Pro capture driver");
MODULE_AUTHOR("Steven Toth <stoth@kernellabs.com>, Nakildias <nakildiaspro@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION(SC0710_DRV_VERSION);

/* 1 = Basic device statistics
 * 2 = PCIe register dump for entire device
 */
unsigned int procfs_verbosity = 3;
module_param(procfs_verbosity, int, 0644);
MODULE_PARM_DESC(procfs_verbosity, "enable procfs debugging via /proc/sc0710");

unsigned int thread_hdmi_active = 1;
module_param(thread_hdmi_active, int, 0644);
MODULE_PARM_DESC(thread_hdmi_active, "should HDMI thread run");

unsigned int thread_dma_active = 1;
module_param(thread_dma_active, int, 0644);
MODULE_PARM_DESC(thread_dma_active, "should dma thread run");

unsigned int thread_hdmi_poll_interval_ms = 200;
module_param(thread_hdmi_poll_interval_ms, int, 0644);
MODULE_PARM_DESC(thread_hdmi_poll_interval_ms, "have the kernel thread poll hdmi every N ms (def:200)");

unsigned int thread_dma_poll_interval_ms = 2;
module_param(thread_dma_poll_interval_ms, int, 0644);
MODULE_PARM_DESC(thread_dma_poll_interval_ms, "have the kernel thread poll dma every N ms (def:2)");

unsigned int dma_status = 0;
module_param(dma_status, int, 0644);
MODULE_PARM_DESC(dma_status, "Manually start or stop dma activities (def:0 Stopped)");

unsigned int sc0710_debug_mode = 0;
module_param(sc0710_debug_mode, int, 0644);
MODULE_PARM_DESC(sc0710_debug_mode, "Enable debug logging (0=off, 1=on)");
EXPORT_SYMBOL(sc0710_debug_mode);

unsigned int scaler_mode = 0;
module_param(scaler_mode, int, 0644);
MODULE_PARM_DESC(scaler_mode, "Software scaler: 0=disabled, 1=upscale 4K, 2=downscale 1080P");

unsigned int auto_scaler = 1;
module_param(auto_scaler, int, 0644);
MODULE_PARM_DESC(auto_scaler, "Automatic safety scaler on mismatch: 0=off, 1=on");

unsigned int procedural_timings = TIMING_MODE_MERGE;
module_param(procedural_timings, int, 0644);
MODULE_PARM_DESC(procedural_timings,
	"Timing selection mode: 0=merge(static+procedural), 1=procedural-only, 2=static-only");

char *sc0710_edid_profile = "";
module_param_named(edid, sc0710_edid_profile, charp, 0444);
MODULE_PARM_DESC(edid,
	"EDID profile presented to the HDMI source, e.g. \"1440p\" (4K Pro only; "
	"on the MK.2 set a custom EDID at runtime via VIDIOC_S_EDID or "
	"sc0710-cli --edid-config). Loads "
	"/lib/firmware/sc0710/edid/<name>.bin, installed by scripts/extract-firmware.sh; "
	"empty = factory default.");

unsigned int dma_resync_validate_frames = 8;
module_param(dma_resync_validate_frames, int, 0644);
MODULE_PARM_DESC(dma_resync_validate_frames,
	"Frames to validate after resync before disabling tear detection");

unsigned int dma_resync_tear_streak_required = 2;
module_param(dma_resync_tear_streak_required, int, 0644);
MODULE_PARM_DESC(dma_resync_tear_streak_required,
	"Consecutive tear detections required before scheduling a re-resync");

unsigned int dma_resync_max_tear_retries = 5;
module_param(dma_resync_max_tear_retries, int, 0644);
MODULE_PARM_DESC(dma_resync_max_tear_retries,
	"Maximum tear-triggered DMA resync retries per timing change");

unsigned int refresh_rate_resync_passes = 2;
module_param(refresh_rate_resync_passes, int, 0644);
MODULE_PARM_DESC(refresh_rate_resync_passes,
	"Total DMA resync passes on refresh-rate-only switches (>=1)");

unsigned int refresh_rate_resync_delay_ms = 12;
module_param(refresh_rate_resync_delay_ms, int, 0644);
MODULE_PARM_DESC(refresh_rate_resync_delay_ms,
	"Delay between refresh-rate resync passes in milliseconds");

static unsigned int card[]  = {[0 ... (SC0710_MAXBOARDS - 1)] = UNSET };
module_param_array(card,  int, NULL, 0444);
MODULE_PARM_DESC(card, "card type");

#define dprintk(level, fmt, arg...)\
	do { if (sc0710_debug_mode >= level)\
		printk(KERN_DEBUG "%s: " fmt, dev->name, ## arg);\
	} while (0)

static DEFINE_MUTEX(devlist);
LIST_HEAD(sc0710_devlist);

/* dev->nr allocator.
 * nr indexes the SC0710_MAXBOARDS-sized module-param arrays and must stay
 * unique among live devices, so slots are reclaimed on remove and on every
 * probe-failure path. */
static DEFINE_IDA(sc0710_nr_ida);

#define SC0710_DMA_WATCHDOG_TIMEOUT_MS 3000

static void sc_andor(struct sc0710_dev *dev, int bar, u32 reg, u32 mask, u32 value)
{
	if (bar == 1 && dev->bar1_size && reg >= dev->bar1_size)
		return;
	u32 newval = (readl(dev->lmmio[bar]+((reg)>>2)) & ~(mask)) | ((value) & (mask));
	writel(newval, dev->lmmio[bar]+((reg)>>2));
}

u32 sc_read(struct sc0710_dev *dev, int bar, u32 reg)
{
	if (bar == 1 && dev->bar1_size && reg >= dev->bar1_size)
		return 0xFFFFFFFF;
	return readl(dev->lmmio[bar] + (reg >> 2));
}

void sc_write(struct sc0710_dev *dev, int bar, u32 reg, u32 value)
{
	if (bar == 1 && dev->bar1_size && reg >= dev->bar1_size)
		return;
	writel(value, dev->lmmio[bar] + (reg >>2));
}

void sc_set(struct sc0710_dev *dev, int bar, u32 reg, u32 bit)
{
	sc_andor(dev, bar, (reg), (bit), (bit));
}

void sc_clr(struct sc0710_dev *dev, int bar, u32 reg, u32 bit)
{
	sc_andor(dev, bar, (reg), (bit), 0);
}

static void sc0710_shutdown(struct sc0710_dev *dev)
{
	/* Disable all interrupts */
	/* Power down all function blocks */
}

static int get_resources(struct sc0710_dev *dev)
{
	int bar1_idx = sc0710_boards[dev->board].bar1_index;

	if (request_mem_region(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0), dev->name) == 0)
	{
		printk(KERN_ERR "%s: can't get var[0] memory @ 0x%llx\n",
			dev->name, (unsigned long long)pci_resource_start(dev->pci, 0));
		return -EBUSY;
	}

	if (request_mem_region(pci_resource_start(dev->pci, bar1_idx), pci_resource_len(dev->pci, bar1_idx), dev->name) == 0)
	{
		printk(KERN_ERR "%s: can't get bar[%d] memory @ 0x%llx\n",
			dev->name, bar1_idx, (unsigned long long)pci_resource_start(dev->pci, bar1_idx));
		release_mem_region(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0));
		return -EBUSY;
	}

	return 0;
}

static int sc0710_dev_setup(struct sc0710_dev *dev)
{
	int i;

	mutex_init(&dev->lock);
	mutex_init(&dev->signalMutex);

	atomic_inc(&dev->refcount);

	dev->nr = ida_alloc_max(&sc0710_nr_ida, SC0710_MAXBOARDS - 1, GFP_KERNEL);
	if (dev->nr < 0) {
		printk(KERN_ERR "sc0710: all %d board slots in use, refusing probe\n",
			SC0710_MAXBOARDS);
		return -ENODEV;
	}
	sprintf(dev->name, "sc0710[%d]", dev->nr);

	/* board config */
	dev->board = UNSET;
	if (card[dev->nr] < sc0710_bcount)
		dev->board = card[dev->nr];
	for (i = 0; UNSET == dev->board  &&  i < sc0710_idcount; i++)
		if (dev->pci->subsystem_vendor == sc0710_subids[i].subvendor &&
		    dev->pci->subsystem_device == sc0710_subids[i].subdevice)
			dev->board = sc0710_subids[i].card;
	if (UNSET == dev->board) {
		dev->board = SC0710_BOARD_UNKNOWN;
		sc0710_card_list(dev);
	}

	/* The keepalive thread needs a mutex */
	mutex_init(&dev->kthread_hdmi_lock);
	mutex_init(&dev->kthread_dma_lock);

	if (get_resources(dev) < 0) {
		printk(KERN_ERR "%s No more PCIe resources for "
		       "subsystem: %04x:%04x\n",
		       dev->name, dev->pci->subsystem_vendor,
		       dev->pci->subsystem_device);

		ida_free(&sc0710_nr_ida, dev->nr);
		return -ENODEV;
	}

	/* PCIe stuff */
	int bar1_idx = sc0710_boards[dev->board].bar1_index;
	dev->bar1_size = pci_resource_len(dev->pci, bar1_idx);

	dev->lmmio[0] = ioremap(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0));
	dev->bmmio[0] = (u8 __iomem *)dev->lmmio[0];
	dev->lmmio[1] = ioremap(pci_resource_start(dev->pci, bar1_idx), dev->bar1_size);
	dev->bmmio[1] = (u8 __iomem *)dev->lmmio[1];
	if (!dev->lmmio[0] || !dev->lmmio[1]) {
		printk(KERN_ERR "%s: ioremap failed, aborting probe\n", dev->name);
		if (dev->lmmio[0])
			iounmap(dev->lmmio[0]);
		if (dev->lmmio[1])
			iounmap(dev->lmmio[1]);
		dev->lmmio[0] = NULL;
		dev->lmmio[1] = NULL;
		release_mem_region(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0));
		release_mem_region(pci_resource_start(dev->pci, bar1_idx), pci_resource_len(dev->pci, bar1_idx));
		ida_free(&sc0710_nr_ida, dev->nr);
		return -ENOMEM;
	}

	printk(KERN_INFO "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name, dev->pci->subsystem_vendor,
	       dev->pci->subsystem_device, sc0710_boards[dev->board].name,
	       dev->board, card[dev->nr] == dev->board ?
	       "insmod option" : "autodetected");

	/* Initialize software scaler mode for eligible boards */
	if (sc0710_software_scaler_allowed(dev) && scaler_mode <= 2)
		dev->scaler_mode = (enum sc0710_scaler_mode)scaler_mode;
	else
		dev->scaler_mode = SCALER_MODE_DISABLED;

	return 0;
}

static void sc0710_dev_unregister(struct sc0710_dev *dev)
{
	int bar1_idx = sc0710_boards[dev->board].bar1_index;

	release_mem_region(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0));
	release_mem_region(pci_resource_start(dev->pci, bar1_idx), pci_resource_len(dev->pci, bar1_idx));

	if (!atomic_dec_and_test(&dev->refcount))
		return;

	sc0710_dma_channels_free(dev);

	iounmap(dev->lmmio[0]);
	iounmap(dev->lmmio[1]);

	/* Single nr release point: probe failure and remove both land here
	 * exactly once per device. */
	ida_free(&sc0710_nr_ida, dev->nr);
}

#ifdef CONFIG_PROC_FS
static int sc0710_proc_state_show(struct seq_file *m, void *v)
{
	struct sc0710_dma_channel *ch;
	struct sc0710_dev *dev;
	struct list_head *list;
	int i;

	/* For each sc0710 in the system.
	 * Hold the devlist mutex so a concurrent unbind/rmmod can't free a dev
	 * under us. */
	mutex_lock(&devlist);
	list_for_each(list, &sc0710_devlist) {
		dev = list_entry(list, struct sc0710_dev, devlist);

		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  dma status: %d\n", dma_status);

		/* Cached state only: this file is world-readable, so its read path
		 * must not run I2C or trigger a DMA reconfig; the HDMI poll
		 * thread keeps the cache fresh. signalMutex is held until after
		 * the scaler block: everything up to there reads dev->fmt or
		 * fields the HDMI thread rewrites. */
		mutex_lock(&dev->signalMutex);
		seq_printf(m, "         fmt: %p\n", dev->fmt);
	        if (dev->locked) {
			seq_printf(m, "        HDMI: %s -- %dx%d%c (%dx%d)\n",
				dev->fmt ? dev->fmt->name : "UNDEFINED",
				dev->width, dev->height,
				dev->interlaced ? 'i' : 'p',
				dev->pixelLineH, dev->pixelLineV);
			if (dev->fmt) {
				seq_printf(m, "   framesize: %d\n", dev->fmt->framesize);
			}
		} else {
			seq_printf(m, "        HDMI: no signal\n");
		}

		seq_printf(m, " colorimetry: %s\n", sc0710_colorimetry_ascii(dev->colorimetry));
		seq_printf(m, "  colorspace: %s\n", sc0710_colorspace_ascii(dev->colorspace));
		seq_printf(m, "     procamp: brightness  %d\n", dev->brightness);
		seq_printf(m, "     procamp: contrast    %d\n", dev->contrast);
		seq_printf(m, "     procamp: saturation  %d\n", dev->saturation);
		seq_printf(m, "     procamp: hue         %d\n", dev->hue);

		/* Software scaler state */
		if (sc0710_software_scaler_allowed(dev)) {
			seq_printf(m, "      scaler: %s\n",
				sc0710_scaler_mode_name(dev->scaler_mode));
			if (dev->scaler_mode != SCALER_MODE_DISABLED && dev->fmt) {
				u32 out_w, out_h;
				sc0710_scaler_get_output_size(dev,
					dev->fmt->width, dev->fmt->height,
					&out_w, &out_h);
				seq_printf(m, "  scaled out: %ux%u -> %ux%u\n",
					dev->fmt->width, dev->fmt->height,
					out_w, out_h);
			}
			seq_printf(m, " auto scaler: %s\n", dev->auto_scaler_active ? "ON (Prevented Kernel Panic)" : "OFF");
			seq_printf(m, " auto scaler cfg: %s\n", auto_scaler ? "ENABLED" : "DISABLED");
			switch (procedural_timings) {
			case TIMING_MODE_PROCEDURAL_ONLY:
				seq_printf(m, " timing calc: PROCEDURAL_ONLY\n");
				break;
			case TIMING_MODE_STATIC_ONLY:
				seq_printf(m, " timing calc: STATIC_ONLY\n");
				break;
			case TIMING_MODE_MERGE:
			default:
				seq_printf(m, " timing calc: MERGE\n");
				break;
			}
			{
				int dyn_active = (!auto_scaler &&
						  dev->scaler_mode == SCALER_MODE_DISABLED);
				seq_printf(m, " dynamic res: %s\n",
					dyn_active ? "ACTIVE" : "INACTIVE");
			}
		}
		mutex_unlock(&dev->signalMutex);

		for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
			ch = &dev->channel[i];
			if (!ch->enabled)
				continue;
			seq_printf(m, "  ch[%d]\n", i);
			seq_printf(m, "        type: %s\n",
				ch->mediatype == CHTYPE_VIDEO ? "VIDEO" : "AUDIO");
			seq_printf(m, "     dma bps: %lld (Mb/ps %lld) (MB/ps %lld)\n",
				sc0710_things_per_second_query(&ch->bitsPerSecond),
				sc0710_things_per_second_query(&ch->bitsPerSecond) / 1000000,
				sc0710_things_per_second_query(&ch->bitsPerSecond) / 1000000 / 8);
			seq_printf(m, "    descr ps: %lld\n",
				sc0710_things_per_second_query(&ch->descPerSecond));

			if (ch->mediatype == CHTYPE_AUDIO) {
				seq_printf(m, "  aud sam ps: %lld\n",
					sc0710_things_per_second_query(&ch->audioSamplesPerSecond) / 2);
			}
		}

	}
	mutex_unlock(&devlist);

	return 0;
}

static int sc0710_proc_show(struct seq_file *m, void *v)
{
	struct sc0710_dev *dev;
	struct list_head *list;
	int i;
	u32 val;

	/* For each sc0710 in the system (devlist mutex held so a concurrent
	 * unbind/rmmod can't free a dev under us) */
	mutex_lock(&devlist);
	list_for_each(list, &sc0710_devlist) {
		dev = list_entry(list, struct sc0710_dev, devlist);
		seq_printf(m, "%s = %p\n", dev->name, dev);

		if (procfs_verbosity & 0x02) {
			/* sc_read() only bounds-checks BAR1, so clamp the
			 * walk to the real BAR0 length. */
			u32 end = min_t(u64, 0x100000,
					pci_resource_len(dev->pci, 0));

			seq_printf(m, "Full PCI Register Dump:\n");
			for (i = 0; i < end; i += 4) {
				/* Reading a FIFO data register pops a byte from
				 * a possibly in-flight transfer: AXI IIC
				 * RX_FIFO 0x310C, QSPI DRR 0x206C. */
				if (i == 0x310c || i == 0x206c)
					continue;
				val = sc_read(dev, 0, i);
				if (val) {
					seq_printf(m, " 0x%04x = %08x\n", i, val);
				}
			}
		}

	}
	mutex_unlock(&devlist);

	return 0;
}

static int sc0710_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sc0710_proc_show, NULL);
}

static int sc0710_proc_state_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sc0710_proc_state_show, NULL);
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
static struct file_operations sc0710_proc_fops = {
	.open		= sc0710_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations sc0710_proc_state_fops = {
	.open		= sc0710_proc_state_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#else
static struct proc_ops sc0710_proc_fops = {
	.proc_open		= sc0710_proc_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static struct proc_ops sc0710_proc_state_fops = {
	.proc_open		= sc0710_proc_state_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};
#endif

static int sc0710_proc_create(void)
{
	struct proc_dir_entry *pe;

	/* Root-only: the raw register dump has no business being world-readable.
	 * sc0710-cli only reads sc0710-state, which stays 0444. */
	pe = proc_create("sc0710", S_IRUSR, NULL, &sc0710_proc_fops);
	if (!pe)
		return -ENOMEM;

	pe = proc_create("sc0710-state", S_IRUGO, NULL, &sc0710_proc_state_fops);
	if (!pe) {
		remove_proc_entry("sc0710", NULL);
		return -ENOMEM;
	}

	return 0;
}
#endif

static bool sc0710_dma_watchdog_check_locked(struct sc0710_dev *dev)
{
	unsigned long timeout_jiffies = msecs_to_jiffies(SC0710_DMA_WATCHDOG_TIMEOUT_MS);
	int i;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		struct sc0710_dma_channel *ch = &dev->channel[i];

		if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
			continue;
		if (ch->state != STATE_RUNNING)
			continue;
		if (atomic_read(&ch->streaming_refcount) <= 0)
			continue;

		if (!ch->dma_last_completion_jiffies) {
			ch->dma_last_completion_jiffies = jiffies;
			continue;
		}

		if (time_after(jiffies, ch->dma_last_completion_jiffies + timeout_jiffies)) {
			printk(KERN_WARNING "%s: DMA watchdog detected stalled video channel %d, scheduling resync\n",
			       dev->name, ch->nr);
			ch->dma_last_completion_jiffies = jiffies;
			return true;
		}
	}

	return false;
}

static int sc0710_thread_dma_function(void *data)
{
	struct sc0710_dev *dev = data;
	int need_dma_resync;
	int tear_requested_resync;
	int i;

	dprintk(1, "%s() Started\n", __func__);

	msleep(2000);

	set_freezable();

	while (1) {
		/* The param is root-writable at runtime; 0 would busy-loop this
		 * thread. */
		msleep_interruptible(max_t(unsigned int, thread_dma_poll_interval_ms, 1));

		if (kthread_should_stop())
			break;

		try_to_freeze();

		if (thread_dma_active == 0)
			continue;

		need_dma_resync = 0;
		tear_requested_resync = 0;
		mutex_lock(&dev->kthread_dma_lock);
		if (!dev->reconfig_in_progress) {
			sc0710_dma_channels_service(dev);
			need_dma_resync = sc0710_dma_watchdog_check_locked(dev);
			if (!need_dma_resync && dev->tear_resync_pending) {
				dev->tear_resync_pending = 0;
				need_dma_resync = 1;
				tear_requested_resync = 1;
				printk(KERN_WARNING "%s: Scheduling DMA re-resync after tear detection\n",
				       dev->name);
			}
		}
		mutex_unlock(&dev->kthread_dma_lock);

		if (need_dma_resync) {
			if (!tear_requested_resync) {
				for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
					struct sc0710_dma_channel *ch = &dev->channel[i];
					if (!ch->enabled || ch->mediatype != CHTYPE_VIDEO)
						continue;
					ch->tear_resync_retries_left = dma_resync_max_tear_retries;
				}
			}
			sc0710_reset_dma_frame_sync(dev);
		}
	}

	dprintk(1, "%s() Stopped\n", __func__);
	return 0;
}

static int sc0710_thread_hdmi_function(void *data)
{
	struct sc0710_dev *dev = data;
	unsigned int tick = 0;

	dprintk(1, "%s() Started\n", __func__);

	msleep(2000);

	set_freezable();

	while (1) {
		/* Same busy-loop guard as the DMA thread. */
		msleep_interruptible(max_t(unsigned int, thread_hdmi_poll_interval_ms, 1));

		if (kthread_should_stop())
			break;

		try_to_freeze();

		if (thread_hdmi_active == 0)
			continue;
		/* Other parts of the driver need to guarantee that
		 * various 'keep alives' aren't happening. We'll
		 * prevent race conditions by allowing the
		 * rest of the driver to dictate when
		 * this keepalives can occur.
		 * Use trylock to avoid blocking forever if I2C is stuck.
		 */
		if (!mutex_trylock(&dev->kthread_hdmi_lock)) {
			continue; /* Skip this iteration if busy */
		}

		sc0710_i2c_read_hdmi_status(dev);
		/* Keep the procamp cache fresh: /proc/sc0710-state prints it but is
		 * forbidden from running I2C itself. The values are user controls
		 * that rarely change, so every 10th tick (~2 s at the default
		 * interval) is plenty and spares the bus an MCU transaction. */
		if (tick++ % 10 == 0)
			sc0710_i2c_read_procamp(dev);
		//sc0710_i2c_read_status2(dev);
		//sc0710_i2c_read_status3(dev);

		/* Sync software scaler mode from module parameter.
		 * This allows runtime toggling via:
		 *   echo N > /sys/module/sc0710/parameters/scaler_mode
		 * where N = 0 (disabled), 1 (upscale to 4K), 2 (downscale to 1080P).
		 */
		if (sc0710_software_scaler_allowed(dev) && scaler_mode <= 2) {
			enum sc0710_scaler_mode new_mode = (enum sc0710_scaler_mode)scaler_mode;
			if (dev->scaler_mode != new_mode) {
				printk(KERN_INFO "%s: Software scaler mode changed: %s -> %s\n",
					dev->name,
					sc0710_scaler_mode_name(dev->scaler_mode),
					sc0710_scaler_mode_name(new_mode));
				dev->scaler_mode = new_mode;
			}
		}

		mutex_unlock(&dev->kthread_hdmi_lock);
	}

	dprintk(1, "%s() Stopped\n", __func__);
	return 0;
}

static int sc0710_initdev(struct pci_dev *pci_dev,
	const struct pci_device_id *pci_id)
{
	struct sc0710_dev *dev;
	int err, i;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (NULL == dev)
		return -ENOMEM;

	err = v4l2_device_register(&pci_dev->dev, &dev->v4l2_dev);
	if (err < 0) {
		printk(KERN_ERR "sc0710: v4l2_device_register failed (%d)\n", err);
		goto fail_free;
	}

	/* pci init */
	dev->pci = pci_dev;
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail_v4l2;
	}

	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER,  &dev->pci_lat);
	printk(KERN_INFO "sc0710 device found at %s, rev: %d, irq: %d, "
		"latency: %d\n",
		pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
		dev->pci_lat);
	printk(KERN_INFO "sc0710 bar[0]: 0x%llx [0x%x bytes]\n",
		(unsigned long long)pci_resource_start(pci_dev, 0),
		(unsigned int)pci_resource_len(pci_dev, 0));
	printk(KERN_INFO "sc0710 bar[1]: 0x%llx [0x%x bytes]\n",
		(unsigned long long)pci_resource_start(pci_dev, 1),
		(unsigned int)pci_resource_len(pci_dev, 1));

	pci_set_master(pci_dev);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,0,0)
	if (!pci_dma_supported(pci_dev, 0xffffffff)) {
#else
	if (dma_set_mask(&pci_dev->dev, 0xffffffff)) {
#endif
		printk("%s/0: Oops: no 32bit PCI DMA ???\n", dev->name);
		err = -EIO;
		goto fail_disable;
	}

	/* MAP the PCIe space, register i2c, program any PCIe quirks.
	 * Self-cleaning on failure: releases its own regions/mappings. */
	if (sc0710_dev_setup(dev) < 0) {
		err = -EINVAL;
		goto fail_disable;
	}

	/* The card's interrupt line is never requested: the vendor design is
	 * pure polling (the XDMA interrupt-enable masks are never armed, and
	 * no interrupt was ever observed on hardware), so a handler would
	 * only invite the spurious-IRQ detector on a shared INTx line. */

	/* Card specific tweaks with subsystems etc. On the 4K Pro this
	 * includes programming the ECP5 video-frontend FPGA; if that fails,
	 * fail the probe: binding is the driver's statement that the card can
	 * capture. */
	err = sc0710_card_setup(dev);
	if (err < 0) {
		printk(KERN_ERR "%s: card setup failed (%d), aborting probe\n",
			dev->name, err);
		goto fail_dev;
	}

	pci_set_drvdata(pci_dev, dev);

	printk(KERN_INFO "sc0710 device at %s\n", pci_name(pci_dev));
	printk(KERN_INFO "sc0710 page-size %lu bytes\n", PAGE_SIZE);

	err = sc0710_dma_channels_alloc(dev);
	if (err < 0) {
		printk(KERN_ERR "%s: DMA channel allocation failed (%d), aborting probe\n",
			dev->name, err);
		goto fail_dev;
	}

	sc0710_i2c_initialize(dev);

	/* Register the user-facing device nodes last: a registered node can be
	 * held open (udev probes every new node), so nothing in probe may fail
	 * after this point or an open fd outlives the kfree below. That is also
	 * why audio registration and the kthread starts stay non-fatal. */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		struct sc0710_dma_channel *ch = &dev->channel[i];

		if (!ch->enabled)
			continue;
		if (ch->mediatype == CHTYPE_VIDEO) {
			err = sc0710_video_register(ch);
			if (err < 0) {
				printk(KERN_ERR "%s: video registration failed (%d), aborting probe\n",
					dev->name, err);
				goto fail_dev;
			}
		} else if (ch->mediatype == CHTYPE_AUDIO) {
			/* Audio is optional: video capture works without ALSA. */
			if (sc0710_audio_register(dev) < 0)
				printk(KERN_WARNING "%s: audio registration failed, continuing without audio\n",
					dev->name);
		}
	}

	/* Put this in a global list so we can track multiple boards */
	mutex_lock(&devlist);
	list_add_tail(&dev->devlist, &sc0710_devlist);
	mutex_unlock(&devlist);

	dev->kthread_hdmi = kthread_run(sc0710_thread_hdmi_function, dev, "sc0710 hdmi");
	if (IS_ERR(dev->kthread_hdmi)) {
		printk(KERN_ERR "%s() Failed to create "
			"hdmi kernel thread (%ld)\n", __func__, PTR_ERR(dev->kthread_hdmi));
		dev->kthread_hdmi = NULL;
	} else
		dprintk(1, "%s() Created the HDMI thread\n", __func__);

	dev->kthread_dma = kthread_run(sc0710_thread_dma_function, dev, "sc0710 dma");
	if (IS_ERR(dev->kthread_dma)) {
		printk(KERN_ERR "%s() Failed to create "
			"dma kernel thread (%ld)\n", __func__, PTR_ERR(dev->kthread_dma));
		dev->kthread_dma = NULL;
	} else
		dprintk(1, "%s() Created the DMA thread\n", __func__);

	return 0;

fail_dev:
	sc0710_dev_unregister(dev);
fail_disable:
	pci_disable_device(pci_dev);
fail_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
fail_free:
	kfree(dev);
	return err;
}

static void sc0710_finidev(struct pci_dev *pci_dev)
{
	struct sc0710_dev *dev = pci_get_drvdata(pci_dev);

	if (dev->kthread_dma) {
		kthread_stop(dev->kthread_dma);
		dev->kthread_dma = NULL;
	}

	if (dev->kthread_hdmi) {
		kthread_stop(dev->kthread_hdmi);
		dev->kthread_hdmi = NULL;
	}

	sc0710_shutdown(dev);

	/* Stop the DMA engines explicitly rather than relying on
	 * pci_disable_device clearing bus-master while they still run. */
	sc0710_dma_channels_stop(dev);

	pci_disable_device(pci_dev);

	mutex_lock(&devlist);
	list_del(&dev->devlist);
	mutex_unlock(&devlist);

	sc0710_dev_unregister(dev);

	/* Free software scaler staging buffer if allocated */
	if (dev->scaler_staging_buf) {
		vfree(dev->scaler_staging_buf);
		dev->scaler_staging_buf = NULL;
	}
	if (dev->weave_staging_buf) {
		vfree(dev->weave_staging_buf);
		dev->weave_staging_buf = NULL;
	}

	v4l2_device_unregister(&dev->v4l2_dev);
	kfree(dev);
}

static struct pci_device_id sc0710_pci_tbl[] = {
	{
		.vendor       = 0x12ab,
		.device       = 0x0710,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	} , {
		.vendor       = 0x12ab,
		.device       = 0x0380,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	} , {
		/* List terminator */
	}
};
MODULE_DEVICE_TABLE(pci, sc0710_pci_tbl);

static struct pci_driver sc0710_pci_driver = {
	.name     = "sc0710",
	.id_table = sc0710_pci_tbl,
	.probe    = sc0710_initdev,
	.remove   = sc0710_finidev,
	/* TODO */
	.suspend  = NULL,
	.resume   = NULL,
};

static int __init sc0710_init(void)
{
	int ret;

	printk(KERN_INFO "sc0710 driver version %s loaded\n",
	       SC0710_DRV_VERSION);
#ifdef SNAPSHOT
	printk(KERN_INFO "sc0710: snapshot date %04d-%02d-%02d\n",
	       SNAPSHOT/10000, (SNAPSHOT/100)%100, SNAPSHOT%100);
#endif
#ifdef CONFIG_PROC_FS
	ret = sc0710_proc_create();
	if (ret < 0) {
		printk(KERN_ERR "sc0710: failed to create /proc entries (%d)\n", ret);
		return ret;
	}
#endif
	sc0710_format_initialize();
	ret = pci_register_driver(&sc0710_pci_driver);
#ifdef CONFIG_PROC_FS
	if (ret < 0) {
		remove_proc_entry("sc0710", NULL);
		remove_proc_entry("sc0710-state", NULL);
	}
#endif
	return ret;
}

static void __exit sc0710_fini(void)
{
#ifdef CONFIG_PROC_FS
	remove_proc_entry("sc0710", NULL);
	remove_proc_entry("sc0710-state", NULL);
#endif
	pci_unregister_driver(&sc0710_pci_driver);
	sc0710_video_free_status_frames();
	printk(KERN_INFO "sc0710 driver unloaded\n");
}

module_init(sc0710_init);
module_exit(sc0710_fini);

