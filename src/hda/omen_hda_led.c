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
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/hda_codec.h>
#include <sound/hwdep.h>
#include <sound/control.h>

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
static struct snd_card *omen_card = NULL;
static bool led_auto_control = true; /* Enable automatic LED control based on mute */
static struct snd_kcontrol *mute_control = NULL;
static struct timer_list mute_check_timer;
static struct work_struct mute_check_work;
static bool last_mute_state = false;

/* Check mute state every 200ms */
#define MUTE_CHECK_INTERVAL_MS 200

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
	
	/* Get the sound card */
	card = snd_card_ref(card_num);
	if (!card) {
		pr_err("Sound card %d not found\n", card_num);
		return NULL;
	}
	
	
	/* Iterate through all devices registered with this sound card */
	list_for_each(p, &card->devices) {
		struct snd_device *dev = list_entry(p, struct snd_device, list);
		
		/* Check if this is a hwdep device */
		if (dev->type == SNDRV_DEV_HWDEP) {
			struct snd_hwdep *hwdep = dev->device_data;
			
			if (hwdep && hwdep->private_data) {
				struct hda_codec *codec = hwdep->private_data;
				
				/* Check if this is the codec we're looking for */
				if (codec && codec->core.addr == codec_addr) {
					return codec;
				}
			}
		}
	}
	
	pr_warn("Codec %d not found on card %d\n", codec_addr, card_num);
	snd_card_unref(card);
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

	pr_debug("Mute LED turned %s\n", on ? "on" : "off");
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
	
	/* Disable auto control when manually setting LED */
	led_auto_control = false;
	
	ret = omen_hda_led_set_internal(on);
	if (ret == 0) {
		pr_info("Mute LED turned %s (auto-control disabled)\n", on ? "on" : "off");
	}
	
	return ret;
}

/**
 * check_mute_state - Check current mute state
 *
 * Returns: true if muted, false if unmuted
 */
static bool check_mute_state(void)
{
	struct snd_ctl_elem_value *ucontrol;
	bool is_muted = false;

	if (!mute_control)
		return false;

	ucontrol = kzalloc(sizeof(*ucontrol), GFP_KERNEL);
	if (!ucontrol)
		return false;

	/* Get current mute state */
	if (mute_control->get(mute_control, ucontrol) == 0) {
		/* value.integer.value[0] == 0 means muted for most controls */
		if (ucontrol->value.integer.value[0] == 0) {
			is_muted = true;
		}
	}

	kfree(ucontrol);
	return is_muted;
}

/**
 * mute_check_work_handler - Work queue handler for mute state checking
 * @work: Work structure
 *
 * Checks mute state and updates LED if changed
 */
static void mute_check_work_handler(struct work_struct *work)
{
	bool current_mute_state;

	if (!led_auto_control || !omen_codec || !mute_control)
		return;

	current_mute_state = check_mute_state();

	/* Update LED only if state changed */
	if (current_mute_state != last_mute_state) {
		omen_hda_led_set_internal(current_mute_state);
		last_mute_state = current_mute_state;
	}
}

/**
 * mute_check_timer_callback - Timer callback for periodic mute checking
 * @t: Timer structure
 *
 * Schedules work to check mute state
 */
static void mute_check_timer_callback(struct timer_list *t)
{
	/* Schedule work to check mute state */
	schedule_work(&mute_check_work);

	/* Re-arm timer for next check */
	if (led_auto_control) {
		mod_timer(&mute_check_timer, jiffies + msecs_to_jiffies(MUTE_CHECK_INTERVAL_MS));
	}
}

/**
 * omen_register_volume_monitor - Register volume change monitoring
 *
 * Sets up monitoring of volume/mute controls to auto-update LED
 */
static void omen_register_volume_monitor(void)
{
	struct snd_kcontrol *kctl;
	const char *control_names[] = {
		"Master Playback Switch",
		"Speaker Playback Switch",
		"Headphone Playback Switch",
		"PCM Playback Switch",
		NULL
	};
	int i;

	if (!omen_card)
		return;


	/* Try to find a mute control */
	for (i = 0; control_names[i]; i++) {
		struct snd_ctl_elem_id id = {0};
		
		id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		strncpy(id.name, control_names[i], sizeof(id.name) - 1);
		
		kctl = snd_ctl_find_id(omen_card, &id);
		if (kctl) {
			pr_info("Found control: %s - using for LED monitoring\n", control_names[i]);
			mute_control = kctl;
			break;
		}
	}
	
	if (mute_control) {
		/* Initialize work queue */
		INIT_WORK(&mute_check_work, mute_check_work_handler);
		
		/* Initialize and start timer */
		timer_setup(&mute_check_timer, mute_check_timer_callback, 0);
		mod_timer(&mute_check_timer, jiffies + msecs_to_jiffies(MUTE_CHECK_INTERVAL_MS));
		
		/* Check initial state */
		last_mute_state = check_mute_state();
		omen_hda_led_set_internal(last_mute_state);
		
	} else {
		pr_warn("No mute control found, auto LED control disabled\n");
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
	struct snd_card *card;
	
	/* Try to find the codec on card 1, device 0 (matches hwC1D0) */
	omen_codec = find_hda_codec_by_card_number(DEFAULT_HDA_CARD, DEFAULT_HDA_CODEC);
	
	if (!omen_codec) {
		pr_warn("Could not find HDA codec for LED control\n");
		return -ENODEV;
	}

	/* Keep reference to the card for volume monitoring */
	card = snd_card_ref(DEFAULT_HDA_CARD);
	if (card) {
		omen_card = card;
		/* Register volume monitoring */
		omen_register_volume_monitor();
	}

	pr_info("HDA LED control initialized successfully\n");
	return 0;
}

/**
 * omen_hda_led_cleanup - Cleanup HDA LED control
 *
 * Releases resources used by HDA LED control
 */
void omen_hda_led_cleanup(void)
{
	/* Disable auto control and stop timer */
	led_auto_control = false;
	
	if (mute_control) {
		timer_delete_sync(&mute_check_timer);
		cancel_work_sync(&mute_check_work);
		mute_control = NULL;
	}
	
	/* Turn off LED before cleanup */
	if (omen_codec) {
		omen_hda_led_set_internal(false);
	}
	
	/* Release card reference from volume monitoring */
	if (omen_card) {
		snd_card_unref(omen_card);
		omen_card = NULL;
	}
	
	if (omen_codec) {
		/* We're holding a card reference from find_hda_codec_by_card_number */
		if (omen_codec->card)
			snd_card_unref(omen_codec->card);
		omen_codec = NULL;
		pr_info("HDA LED control cleaned up\n");
	}
}
