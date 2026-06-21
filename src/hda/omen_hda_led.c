// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - HDA LED Control
 *
 * HDA codec interface for controlling the mute button LED
 * Uses HDA verb commands to toggle the LED state
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/hda_codec.h>
#include <sound/hwdep.h>

#include "omen_hda_led.h"

/* HDA codec parameters */
#define OMEN_HDA_CODEC_NID     0x20    /* Node ID */
#define OMEN_HDA_VERB_SET_COEF 0x500   /* Set coefficient */
#define OMEN_HDA_VERB_SET_PROC 0x400   /* Set processing state */
#define OMEN_HDA_COEF_INDEX    0x0B    /* Coefficient index for LED */
#define OMEN_HDA_LED_ON_VALUE  0x7778  /* Value to turn LED on */
#define OMEN_HDA_LED_OFF_VALUE 0x7774  /* Value to turn LED off */

/* Default card and codec address (hwC1D0 = card 1, device/codec 0) */
#define DEFAULT_HDA_CARD       1
#define DEFAULT_HDA_CODEC      0

static struct hda_codec *omen_codec = NULL;
static bool led_auto_control = true; /* Enable automatic LED control based on mute */
static bool external_mute_state = false;
static bool use_external_mute = false;
static struct delayed_work codec_retry_work;
static int codec_retry_count = 0;
#define MAX_CODEC_RETRIES 20
#define CODEC_RETRY_DELAY_MS 5000  /* 5 seconds */


/**
 * find_hda_codec_by_card_number - Find HDA codec by searching through devices
 * @card_num: Sound card number
 * @codec_addr: Codec address
 *
 * Searches for codec by iterating through all sound card devices.
 * This works for both traditional HDA and SOF drivers.
 *
 * Returns: pointer to hda_codec on success, NULL on failure
 */
static struct hda_codec *find_hda_codec_by_card_number(int card_num, int codec_addr)
{
	struct snd_card *card;
	struct list_head *p;
	struct hda_codec *found_codec = NULL;
	int hwdep_count = 0;
	/* Get the sound card */
	card = snd_card_ref(card_num);
	if (!card) {
		pr_debug("Sound card %d not found or not ready yet\n", card_num);
		return NULL;
	}
	
	pr_debug("Searching for codec %d on card %d (%s)\n", 
		 codec_addr, card_num, card->shortname);
	
	/* Iterate through all devices registered with this sound card */
	list_for_each(p, &card->devices) {
		struct snd_device *dev = list_entry(p, struct snd_device, list);
		
		/* Check if this is a hwdep device */
		if (dev->type == SNDRV_DEV_HWDEP) {
			struct snd_hwdep *hwdep = dev->device_data;

			hwdep_count++;
			if (hwdep && hwdep->private_data) {
				struct hda_codec *codec = hwdep->private_data;
				
				pr_debug("Found hwdep device with codec at addr %d (vendor:0x%x)\n",
					 codec->core.addr, codec->core.vendor_id);
				
				/* Check if this is the codec we're looking for */
				if (codec && codec->core.addr == codec_addr) {
					found_codec = codec;
					pr_info("Found HDA codec on card %d, addr %d: %s\n",
						card_num, codec_addr, codec->core.chip_name);
					/* Don't unref card - we're keeping the reference */
					return found_codec;
				}
			}
		}
	}
	
	pr_debug("Codec %d not found on card %d (hwdep devices: %d)\n",
		 codec_addr, card_num, hwdep_count);
	if (card_num == DEFAULT_HDA_CARD && !hwdep_count)
		pr_info("Card %d (%s) is present but HDA codec hwdep is not registered yet\n",
			card_num, card->shortname);
	snd_card_unref(card);
	return NULL;
}

/**
 * is_realtek_or_compatible_codec - Check if codec is suitable for LED control
 * @codec: HDA codec to check
 *
 * Returns: true if codec is Realtek or other compatible audio codec, false for GPU codecs
 */
static bool is_realtek_or_compatible_codec(struct hda_codec *codec)
{
	unsigned int vendor_id;
	
	if (!codec)
		return false;
	
	vendor_id = codec->core.vendor_id >> 16;
	
	/* Realtek codecs (0x10ec) - preferred */
	if (vendor_id == 0x10ec) {
		pr_debug("Found Realtek codec: 0x%08x\n", codec->core.vendor_id);
		return true;
	}
	
	/* Conexant (0x14f1), IDT/Sigmatel (0x111d), Cirrus (0x1013) - also good */
	if (vendor_id == 0x14f1 || vendor_id == 0x111d || vendor_id == 0x1013) {
		pr_debug("Found compatible audio codec: 0x%08x\n", codec->core.vendor_id);
		return true;
	}
	
	/* Skip NVIDIA (0x10de), AMD (0x1002), Intel (0x8086) HDMI/DP codecs */
	if (vendor_id == 0x10de || vendor_id == 0x1002 || vendor_id == 0x8086) {
		pr_debug("Skipping GPU/HDMI codec: 0x%08x\n", codec->core.vendor_id);
		return false;
	}
	
	/* For unknown vendors, accept them as potential candidates */
	pr_debug("Found unknown vendor codec: 0x%08x\n", codec->core.vendor_id);
	return true;
}

/**
 * release_hda_codec_ref - Drop a card reference acquired during codec lookup
 * @codec: HDA codec whose card reference should be released
 */
static void release_hda_codec_ref(struct hda_codec *codec)
{
	if (codec && codec->card)
		snd_card_unref(codec->card);
}

/**
 * find_suitable_hda_codec - Find a Realtek/compatible HDA codec for LED control
 *
 * Scans sound cards looking for an audio codec suitable for mute LED verbs.
 * GPU HDMI/DP codecs are ignored. The default card (usually hwC1D0) is tried
 * first so laptop Realtek codecs win over earlier-probing GPU codecs.
 *
 * Returns: pointer to hda_codec on success, NULL on failure
 */
static struct hda_codec *find_suitable_hda_codec(void)
{
	struct hda_codec *codec;
	int card_order[8];
	int i, card_idx;
	bool saw_gpu_codec = false;

	/* Prefer the laptop audio card before scanning GPU HDMI cards */
	card_order[0] = DEFAULT_HDA_CARD;
	card_idx = 1;
	for (i = 0; i < 8; i++) {
		if (i != DEFAULT_HDA_CARD)
			card_order[card_idx++] = i;
	}

	for (i = 0; i < 8; i++) {
		int card_num = card_order[i];
		int codec_addr;

		for (codec_addr = 0; codec_addr < 4; codec_addr++) {
			codec = find_hda_codec_by_card_number(card_num, codec_addr);
			if (!codec)
				continue;

			if (is_realtek_or_compatible_codec(codec)) {
				pr_info("Selected audio codec on card %d, addr %d (vendor: 0x%04x)\n",
					card_num, codec_addr, codec->core.vendor_id >> 16);
				return codec;
			}

			saw_gpu_codec = true;
			pr_debug("Ignoring unsuitable codec on card %d, addr %d: %s\n",
				 card_num, codec_addr, codec->core.chip_name);
			release_hda_codec_ref(codec);
		}
	}

	if (saw_gpu_codec)
		pr_info("Found GPU HDMI codec(s) but no suitable audio codec yet; will keep retrying\n");

	return NULL;
}

/**
 * omen_hda_led_set_internal - Internal function to set LED state
 * @on: true to turn LED on, false to turn it off
 *
 * Returns: 0 on success, negative error code on failure
 */
static int omen_hda_led_set_internal(bool on)
{
	int ret;
	unsigned int led_value;

	if (!omen_codec) {
		pr_err("HDA codec not initialized\n");
		return -ENODEV;
	}

	/* First command: Set coefficient index */
	ret = snd_hda_codec_write(omen_codec, OMEN_HDA_CODEC_NID, 0,
				  OMEN_HDA_VERB_SET_COEF, OMEN_HDA_COEF_INDEX);
	if (ret < 0) {
		pr_err("Failed to set coefficient index: %d\n", ret);
		return ret;
	}

	/* Second command: Set LED state */
	led_value = on ? OMEN_HDA_LED_ON_VALUE : OMEN_HDA_LED_OFF_VALUE;
	ret = snd_hda_codec_write(omen_codec, OMEN_HDA_CODEC_NID, 0,
				  OMEN_HDA_VERB_SET_PROC, led_value);
	if (ret < 0) {
		pr_err("Failed to set LED state: %d\n", ret);
		return ret;
	}

	pr_info("Mute LED turned %s\n", on ? "ON" : "OFF");
	return 0;
}

/**
 * omen_hda_led_set - Set the mute button LED state
 * @on: true to turn LED on, false to turn it off
 *
 * Sends HDA verb commands to control the mute button LED:
 * 1. Set coefficient index (0x500, 0x0B)
 * 2. Set LED state (0x400, 0x7778 for on or 0x7774 for off)
 *
 * Returns: 0 on success, negative error code on failure
 */
int omen_hda_led_set(bool on)
{
	int ret;
		
	ret = omen_hda_led_set_internal(on);
	if (ret == 0) {
		pr_debug("Mute LED turned %s (manual override)\n", on ? "on" : "off");
	}
	
	return ret;
}


/**
 * codec_retry_work_handler - Delayed work handler for retrying codec discovery
 * @work: Delayed work structure
 *
 * Retries finding the HDA codec asynchronously without blocking driver init
 */
static void codec_retry_work_handler(struct work_struct *work)
{
	codec_retry_count++;

	pr_debug("Retry attempt %d/%d: searching for HDA codec\n",
		 codec_retry_count, MAX_CODEC_RETRIES);

	/* Try to find a suitable audio codec (Realtek, etc.) */
	omen_codec = find_suitable_hda_codec();

	if (omen_codec) {
		pr_info("HDA codec found on retry attempt %d\n", codec_retry_count);
		pr_info("HDA LED control initialized successfully (after retry)\n");
		pr_info("Mute LED will be controlled via userspace daemon (PipeWire/Bluetooth)\n");
		
		/* Sync LED: honor mute_state already written before codec was ready */
		if (led_auto_control && omen_codec) {
			if (use_external_mute)
				omen_hda_led_set_internal(external_mute_state);
			else
				omen_hda_led_set_internal(false);
		}
		
		codec_retry_count = 0; /* Reset counter */
		return;
	}

	/* If we haven't exhausted retries, schedule another attempt */
	if (codec_retry_count < MAX_CODEC_RETRIES) {
		pr_debug("Codec not found, scheduling retry %d/%d in %d seconds\n",
			 codec_retry_count + 1, MAX_CODEC_RETRIES,
			 CODEC_RETRY_DELAY_MS / 1000);
		schedule_delayed_work(&codec_retry_work,
				      msecs_to_jiffies(CODEC_RETRY_DELAY_MS));
	} else {
		pr_warn("Failed to find HDA codec after all %d retry attempts\n",
			MAX_CODEC_RETRIES);
		codec_retry_count = 0; /* Reset counter */
	}
}


/**
 * omen_hda_led_init - Initialize HDA LED control
 *
 * Finds the HDA codec and initializes LED control
 *
 * Returns: 0 on success, negative error code on failure
 */
int omen_hda_led_init(void)
{
	pr_debug("Initializing HDA LED control\n");

	omen_codec = find_suitable_hda_codec();

	if (!omen_codec) {
		pr_info("Codec not found on default card %d yet, will scan again asynchronously\n",
			DEFAULT_HDA_CARD);
		pr_warn("Could not find HDA codec for LED control on any card\n");
		pr_warn("Mute LED functionality will not be available until codec is ready\n");
		pr_info("Will retry codec discovery asynchronously (up to %d attempts, every %d seconds)\n",
			MAX_CODEC_RETRIES, CODEC_RETRY_DELAY_MS / 1000);

		INIT_DELAYED_WORK(&codec_retry_work, codec_retry_work_handler);

		codec_retry_count = 0;

		schedule_delayed_work(&codec_retry_work, msecs_to_jiffies(CODEC_RETRY_DELAY_MS));

		return 0;
	}

	pr_info("HDA LED control initialized successfully\n");
	pr_info("Mute LED will be controlled via userspace daemon (PipeWire/Bluetooth)\n");
	
	if (led_auto_control && omen_codec) {
		if (use_external_mute)
			omen_hda_led_set_internal(external_mute_state);
		else
			omen_hda_led_set_internal(false);
	}
	
	return 0;
}

/**
 * omen_hda_led_cleanup - Cleanup HDA LED control
 *
 * Releases resources used by HDA LED control
 */
void omen_hda_led_cleanup(void)
{
	/* Disable auto control */
	led_auto_control = false;
	
	cancel_delayed_work_sync(&codec_retry_work);
	codec_retry_count = 0;
	
	/* Turn off LED before cleanup */
	if (omen_codec) {
		omen_hda_led_set_internal(false);
	}
	
	if (omen_codec) {
		/* We're holding a card reference from find_hda_codec_by_card_number */
		if (omen_codec->card)
			snd_card_unref(omen_codec->card);
		omen_codec = NULL;
		pr_info("HDA LED control cleaned up\n");
	}
}

/**
 * omen_hda_led_set_mute_state - Set mute state from userspace
 * @muted: true if muted, false if unmuted
 *
 * Allows userspace daemon (e.g., PipeWire monitor) to notify kernel
 * about mute state changes. When this is used, ALSA control checking
 * is bypassed in favor of the external state.
 *
 * Returns: 0 on success
 */
int omen_hda_led_set_mute_state(bool muted)
{
	int ret;

	external_mute_state = muted;
	use_external_mute = true;

	if (!led_auto_control)
		return -EIO;
	if (!omen_codec) {
		/* State is kept for when codec appears (retry worker or next boot). */
		pr_debug("Mute LED: %s queued (HDA codec not ready yet)\n",
			 muted ? "ON" : "OFF");
		return 0;
	}

	pr_info("Setting mute LED to %s (from userspace)\n", muted ? "ON" : "OFF");
	ret = omen_hda_led_set_internal(muted);
	if (ret < 0)
		pr_warn("HDA mute LED update failed: %d\n", ret);
	return ret;
}
