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

#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "sc0710.h"

static unsigned int audio_debug = 0;
module_param(audio_debug, int, 0644);
MODULE_PARM_DESC(audio_debug, "enable debug messages [audio]");

#define SC0710_AUDIO_RATE_HZ        48000
#define SC0710_AUDIO_SILENCE_MS       10
#define SC0710_AUDIO_SILENCE_SAMPLES  ((SC0710_AUDIO_RATE_HZ * SC0710_AUDIO_SILENCE_MS) / 1000)
/* Real-delivery pause before the watchdog starts feeding silence; must sit
 * well above normal DMA completion jitter (single-digit ms). */
#define SC0710_AUDIO_GAP_MS           100

#define dprintk(level, fmt, arg...)\
	do { if (audio_debug >= level)\
		printk(KERN_DEBUG "%s/0: " fmt, dev->name, ## arg);\
	} while (0)

static void sc0710_audio_push_frames(struct sc0710_audio_dev *chip,
				     unsigned int samples, const u8 *src,
				     unsigned int stride_bytes, bool silence)
{
	struct snd_pcm_substream *substream = chip->substream;
	struct snd_pcm_runtime *runtime;
	snd_pcm_uframes_t period_size;
	unsigned int i;

	if (!substream)
		return;

	runtime = substream->runtime;
	if (!runtime || !runtime->dma_area || !runtime->buffer_size)
		return;

	period_size = runtime->period_size;
	if (!period_size)
		return;

	for (i = 0; i < samples; i++) {
		u32 *dst = (u32 *)(runtime->dma_area + chip->buffer_ptr * 4);

		if (silence)
			*dst = 0;
		else
			*dst = *(const u32 *)(src + (i * stride_bytes));

		chip->buffer_ptr++;
		if (chip->buffer_ptr >= runtime->buffer_size)
			chip->buffer_ptr = 0;
	}

	chip->period_pos += samples;
	while (chip->period_pos >= period_size) {
		chip->period_pos -= period_size;
		snd_pcm_period_elapsed(substream);
	}
}

/* Delivery-gap watchdog, armed for the whole capture session. The DMA
 * path stops delivering samples on signal loss, across a resolution
 * switch's DMA stop/restart, and while HDMI audio renegotiates after a
 * mode change; a capture stream that makes no progress for 10 s is
 * killed by the ALSA core (read returns -EIO) and takes the app's audio
 * with it. Whenever real samples pause for longer than the gap
 * threshold this feeds silence in their place, so the stream survives
 * and real audio resumes seamlessly.
 * Runs in a kworker (process context), so it may take the sleeping
 * ch->lock -- unlike a timer callback (softirq), which caused a
 * "scheduling while atomic" panic on signal loss. */
static void sc0710_audio_silence_work_fn(struct work_struct *work)
{
	struct sc0710_audio_dev *chip =
		container_of(work, struct sc0710_audio_dev, silence_work.work);
	struct sc0710_dev *dev = chip->dev;
	struct sc0710_dma_channel *ch = &dev->channel[1];
	unsigned long deadline, now, next;

	mutex_lock(&ch->lock);
	if (!chip->running || !chip->substream) {
		mutex_unlock(&ch->lock);
		return;
	}

	/* One jiffies sample for both the check and the sleep length: a
	 * re-read racing past the deadline would wrap the subtraction. */
	now = jiffies;
	deadline = chip->last_sample_jiffies + msecs_to_jiffies(SC0710_AUDIO_GAP_MS);
	if (time_after(now, deadline)) {
		if (audio_debug)
			printk_ratelimited(KERN_INFO "%s: audio delivery gap, feeding ALSA silence\n",
				dev->name);
		sc0710_audio_push_frames(chip, SC0710_AUDIO_SILENCE_SAMPLES, NULL, 0, true);
		next = msecs_to_jiffies(SC0710_AUDIO_SILENCE_MS);
	} else {
		/* Healthy delivery: sleep until the gap threshold after the
		 * most recent real samples, instead of ticking every period.
		 * Real deliveries move the deadline forward; this wake just
		 * re-checks it. */
		next = deadline - now + 1;
	}

	/* re-check: running can be cleared via the ALSA trigger without ch->lock */
	if (chip->running && chip->substream)
		mod_delayed_work(system_wq, &chip->silence_work, next);

	mutex_unlock(&ch->lock);
}

/* Sync teardown: process context only, never under ch->lock -- the work
 * takes ch->lock, so a sync cancel there would deadlock. */
static void sc0710_audio_stop_silence(struct sc0710_audio_dev *chip)
{
	if (!chip)
		return;

	cancel_delayed_work_sync(&chip->silence_work);
}

static void sc0710_audio_init_silence_work(struct sc0710_audio_dev *chip)
{
	INIT_DELAYED_WORK(&chip->silence_work, sc0710_audio_silence_work_fn);
}

int sc0710_audio_deliver_samples(struct sc0710_dev *dev, struct sc0710_dma_channel *ch,
	const u8 *buf, int bitdepth, int strideBytes, int channels, int samplesPerChannel)
{
	struct sc0710_audio_dev *chip;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;

	if (channels != 2 || bitdepth != 16 || samplesPerChannel <= 0)
		return -1;

	chip = ch->audio_dev;
	if (!chip)
		return -1;

	substream = chip->substream;
	if (!substream)
		return 0;

	runtime = substream->runtime;
	if (!runtime || !runtime->dma_area || !runtime->buffer_size)
		return -1;

	if (!runtime->period_size)
		return -1;

	sc0710_audio_push_frames(chip, samplesPerChannel, buf, strideBytes, false);
	chip->last_sample_jiffies = jiffies;

	sc0710_things_per_second_update(&ch->audioSamplesPerSecond,
					samplesPerChannel * 2);

	return 0;
}

static struct snd_pcm_hardware snd_sc0710_hw_capture =
{
	.info = SNDRV_PCM_INFO_BLOCK_TRANSFER |
	    SNDRV_PCM_INFO_MMAP |
	    SNDRV_PCM_INFO_INTERLEAVED |
	    SNDRV_PCM_INFO_MMAP_VALID |
	    SNDRV_PCM_INFO_BATCH,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = 48000,
	.rate_max         = 48000,
	.channels_min     = 2,
	.channels_max     = 2,
	.buffer_bytes_max = 131072,
	.period_bytes_min = 4096,
	.period_bytes_max = 65536,
	.periods_min      = 2,
	.periods_max      = 1024,
};

static int snd_sc0710_capture_open(struct snd_pcm_substream *substream)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sc0710_dev *dev = chip->dev;
	struct sc0710_dma_channel *ch = &dev->channel[1];

	dprintk(1, "%s()\n", __func__);

	if (!chip) {
		printk(KERN_ERR "%s() No chip\n", __func__);
		return -ENODEV;
	}
	if (!ch) {
		printk(KERN_ERR "%s() No channel\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&ch->lock);
	chip->substream = substream;
	mutex_unlock(&ch->lock);

	runtime->private_data = chip;
	runtime->hw = snd_sc0710_hw_capture;

	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);

	return 0;
}

static int snd_sc0710_pcm_close(struct snd_pcm_substream *substream)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	struct sc0710_dev *dev = chip->dev;
	struct sc0710_dma_channel *ch = &dev->channel[1];

	dprintk(1, "%s()\n", __func__);

	/* Stop the hardware */
	mutex_lock(&ch->lock);
	chip->running = false;
	chip->substream = NULL;
	mutex_unlock(&ch->lock);

	/* sync-cancel outside ch->lock (the work takes it); state cleared
	 * above, so any pending work bails. */
	sc0710_audio_stop_silence(chip);

	return 0;
}

static int snd_sc0710_hw_capture_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *hw_params)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sc0710_dev *dev = chip->dev;
	int size;

	size = params_buffer_bytes(hw_params);
	dprintk(1, "%s() buffer_bytes %d\n", __func__, size);

	if (runtime->dma_area) {
		if (runtime->dma_bytes > size)
			return 0;
		vfree(runtime->dma_area);
	}
	runtime->dma_area = vzalloc(size);
	if (!runtime->dma_area)
		return -ENOMEM;
	else
		runtime->dma_bytes = size;

	return 0; /* Success */
}

static int snd_sc0710_hw_capture_free(struct snd_pcm_substream *substream)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	struct sc0710_dev *dev = chip->dev;
	//struct sc0710_dma_channel *ch = &dev->channel[1];

	dprintk(1, "%s() rate = %d\n", __func__, substream->runtime->rate);

	/* Stop the stream */

	return 0;
}

static int snd_sc0710_prepare(struct snd_pcm_substream *substream)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	struct sc0710_dev *dev = chip->dev;
	//struct sc0710_dma_channel *ch = &dev->channel[1];

	dprintk(1, "%s() requested rate = %d\n", __func__, substream->runtime->rate);

	chip->buffer_ptr = 0;
	chip->period_pos = 0;

	if (substream->runtime->rate != 48000) {
		dprintk(1, "%s() audio rate mismatch (%u vs %u)\n", __func__, substream->runtime->rate, 48000);
		return -EINVAL;
	}

	/* Configure the h/w for out audio requirements */

	return 0;
}

static int snd_sc0710_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	struct sc0710_dev *dev = chip->dev;

	dprintk(1, "%s() cmd %d\n", __func__, cmd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* Baseline for the gap watchdog: with no signal at START the
		 * first silence fill lands one gap threshold later. Trigger
		 * runs under snd_pcm_stream_lock (atomic); mod_delayed_work
		 * is safe there. */
		chip->last_sample_jiffies = jiffies;
		chip->running = true;
		mod_delayed_work(system_wq, &chip->silence_work,
				 msecs_to_jiffies(SC0710_AUDIO_GAP_MS));
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		/* atomic context: async cancel only; pcm_close() does the sync teardown */
		chip->running = false;
		cancel_delayed_work(&chip->silence_work);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t snd_sc0710_capture_pointer(struct snd_pcm_substream
						    *substream)
{
	struct sc0710_audio_dev *chip = snd_pcm_substream_chip(substream);
	//printk("%s()\n", __func__);
	return chip->buffer_ptr;
}

static struct page *snd_pcm_pd_get_page(struct snd_pcm_substream *subs,
					unsigned long offset)
{
	void *pageptr = subs->runtime->dma_area + offset;
	/* printk("%s()\n", __func__); */
	return vmalloc_to_page(pageptr);
}

static struct snd_pcm_ops pcm_capture_ops =
{
	.open      = snd_sc0710_capture_open,
	.close     = snd_sc0710_pcm_close,
	.ioctl     = snd_pcm_lib_ioctl,
	.hw_params = snd_sc0710_hw_capture_params,
	.hw_free   = snd_sc0710_hw_capture_free,
	.prepare   = snd_sc0710_prepare,
	.trigger   = snd_sc0710_capture_trigger,
	.pointer   = snd_sc0710_capture_pointer,
	.page      = snd_pcm_pd_get_page,
};

/* Final card free (last handle closed, or immediately when none were open):
 * drop the device reference the card held. */
static void sc0710_audio_private_free(struct snd_card *card)
{
	struct sc0710_audio_dev *chip = (struct sc0710_audio_dev *)card->private_data;

	v4l2_device_put(&chip->dev->v4l2_dev);
}

void sc0710_audio_unregister(struct sc0710_dev *dev)
{
	struct sc0710_dma_channel *channel = &dev->channel[1];
	struct sc0710_audio_dev *chip = channel->audio_dev;

	dprintk(1, "%s()\n", __func__);
	dprintk(0, "Unregistered ALSA audio device %p\n", chip);

	/* Normal when audio registration failed or never ran. */
	if (!chip)
		return;

	sc0710_audio_stop_silence(chip);
	/* Disconnects immediately (open PCM/ctl handles start erroring) and
	 * defers the card free to the last close, so a handle held open across
	 * remove - PipeWire keeps one persistently - neither blocks remove nor
	 * outlives the card. */
	snd_card_free_when_closed(chip->card);
}

/*
 * create a PCM device
 */
static int snd_sc0710_pcm(struct sc0710_audio_dev *chip, int device, char *name)
{
	int err;
	struct snd_pcm *pcm;

	err = snd_pcm_new(chip->card, name, device, 0, 1, &pcm);
	if (err < 0)
		return err;

	pcm->private_data = chip;

	strcpy(pcm->name, name);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &pcm_capture_ops);

	return 0;
}

int sc0710_audio_register(struct sc0710_dev *dev)
{
	struct snd_card *card;
	struct sc0710_audio_dev *chip;

	/* We register the audio device using DMA channel #2 but we
	 * switch the DMA channel when the user selects a different
	 * video input.
	 */
	struct sc0710_dma_channel *channel = &dev->channel[1];
	int err;

	err = snd_card_new(&dev->pci->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			      THIS_MODULE, sizeof(struct sc0710_audio_dev),
			      &card);
	if (err < 0) {
		printk(KERN_ERR "%s(): snd_card_new failed (%d)\n", __func__, err);
		return err;
	}

	chip = (struct sc0710_audio_dev *)card->private_data;
	chip->card = card;
	chip->dev = dev;
	chip->buffer_ptr = 0;
	chip->running = false;
	sc0710_audio_init_silence_work(chip);

	/* The PCM/ctl callbacks reach into dev; pin it until the card is truly
	 * freed. private_free fires exactly once on every card-free path,
	 * including this function's own error unwind. */
	v4l2_device_get(&dev->v4l2_dev);
	card->private_free = sc0710_audio_private_free;

	err = snd_sc0710_pcm(chip, 0, "sc0710 HDMI");
	if (err < 0)
		goto error;

	strcpy(card->driver, "sc0710");
	sprintf(card->shortname, "Elgato (Yuan sc0710)");
	sprintf(card->longname, "%s at %s", card->shortname, dev->name);
	strcpy(card->mixername, "sc0710");

	err = snd_card_register(card);
	if (err < 0)
		goto error;

	channel->audio_dev = chip;
	dev->channel[1].audio_dev = chip;

	snd_card_set_dev(card, &dev->pci->dev);

	dprintk(1, "Registered ALSA audio device %p card %p\n",
		channel->audio_dev, channel->audio_dev->card);
	return 0;

error:
	snd_card_free(card);
	printk(KERN_ERR "%s(): Failed to register analog "
	       "audio adapter (%d)\n", __func__, err);

	return err;
}
