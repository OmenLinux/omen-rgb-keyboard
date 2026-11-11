// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - HDA LED Control
 *
 * HDA codec interface for controlling the mute button LED
 *
 * Author: alessandromrc
 */

#ifndef OMEN_HDA_LED_H
#define OMEN_HDA_LED_H

#include <linux/types.h>

#ifdef CONFIG_OMEN_HDA_LED

/**
 * omen_hda_led_available - Check whether the driver-side HDA LED path is usable
 *
 * Returns true only when the code was built in, the mute_led module
 * parameter asked for it, and the kernel does not already drive the LED
 * through hda::mute (in-tree Realtek quirk).
 */
bool omen_hda_led_available(void);

/**
 * omen_hda_led_set - Set the mute button LED state
 * @on: true to turn LED on, false to turn it off
 *
 * Returns: 0 on success, negative error code on failure
 */
int omen_hda_led_set(bool on);

/**
 * omen_hda_led_init - Initialize HDA LED control
 *
 * Returns: 0 on success, negative error code on failure
 */
int omen_hda_led_init(void);

/**
 * omen_hda_led_cleanup - Cleanup HDA LED control
 */
void omen_hda_led_cleanup(void);

/**
 * omen_hda_led_set_mute_state - Set mute state from userspace (e.g., PipeWire daemon)
 * @muted: true if muted, false if unmuted
 *
 * This allows a userspace daemon to notify the kernel driver about mute state
 * changes from PipeWire/Bluetooth devices that aren't accessible via ALSA.
 *
 * Returns: 0 on success, negative error code on failure
 */
int omen_hda_led_set_mute_state(bool muted);

#else /* !CONFIG_OMEN_HDA_LED */

static inline bool omen_hda_led_available(void)
{
	return false;
}

static inline int omen_hda_led_set(bool on)
{
	return -ENODEV;
}

static inline int omen_hda_led_init(void)
{
	return 0;
}

static inline void omen_hda_led_cleanup(void)
{
}

static inline int omen_hda_led_set_mute_state(bool muted)
{
	return -ENODEV;
}

#endif /* CONFIG_OMEN_HDA_LED */

#endif /* OMEN_HDA_LED_H */
