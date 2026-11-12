// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Zone Management
 *
 * RGB zone management and LED control
 *
 * Author: alessandromrc
 */

#ifndef OMEN_ZONES_H
#define OMEN_ZONES_H

#include <linux/device.h>
#include <linux/leds.h>
#include <linux/types.h>
#include <linux/platform_device.h>

#include "omen_wmi.h"

#define ZONE_COUNT 4

struct color_platform {
	u8 blue;
	u8 green;
	u8 red;
} __packed;

struct platform_zone {
	u8 offset; /* position in returned buffer */
	struct device_attribute *attr;
	struct color_platform colors;
};

/* Global zone data */
extern struct platform_zone *zone_data;
extern struct platform_zone original_colors[ZONE_COUNT];
extern int global_brightness;
extern struct led_classdev omen_kbd_led;

/**
 * parse_rgb - Parse hex RGB color string into platform_zone
 * @buf: Input buffer containing hex color (e.g., "FF0000")
 * @zone: Target zone to store parsed color
 *
 * Returns: 0 on success, error code otherwise
 */
int parse_rgb(const char *buf, struct platform_zone *zone);

/**
 * fourzone_update_led - Update or read LED color for a zone
 * @zone: Target zone
 * @rw: HPWMI_READ or HPWMI_WRITE
 *
 * Returns: 0 on success, error code otherwise
 */
int fourzone_update_led(struct platform_zone *zone, enum hp_wmi_command rw);

/**
 * match_zone - Find zone matching a device attribute
 * @attr: Device attribute to match
 *
 * Returns: Pointer to matching zone, NULL if not found
 */
struct platform_zone *match_zone(struct device_attribute *attr);

/**
 * apply_brightness_to_color - Apply global brightness to a color
 * @color: Color to modify
 */
void apply_brightness_to_color(struct color_platform *color);

/**
 * fourzone_apply_brightness_lock - Keep a locked backlight off zero
 *
 * While brightness_lock is set the LED class device cannot change the level,
 * so a level of zero could not be undone from the desktop. Raises it to full
 * in that case. Safe to call when the lock is clear; it does nothing then.
 */
void fourzone_apply_brightness_lock(void);

/**
 * fourzone_set_lighting - Switch the keyboard lighting power on or off
 * @on: true to switch on, false to switch off
 *
 * Returns: 0 on success, error code otherwise
 */
int fourzone_set_lighting(bool on);

/**
 * fourzone_get_lighting - Read the keyboard lighting power state
 *
 * Returns: 1 when on, 0 when off, negative error code otherwise
 */
int fourzone_get_lighting(void);

/**
 * fourzone_lighting_on_boot - Apply the lighting_on_boot module parameter
 *
 * Switches the keyboard backlight on after the saved state has been applied,
 * unless lighting_on_boot is clear or the saved brightness is zero.
 */
void fourzone_lighting_on_boot(void);

/**
 * fourzone_apply_current_colors - Write the configured colors to the hardware
 *
 * Applies original_colors scaled by the global brightness to every zone.
 */
void fourzone_apply_current_colors(void);

/**
 * update_all_zones_with_colors - Update all zones with new colors
 * @colors: Array of colors for all zones
 */
void update_all_zones_with_colors(struct color_platform colors[ZONE_COUNT]);

/**
 * omen_restore_lighting - Re-apply the stored lighting state to the hardware
 *
 * The EC drops the zone colours and the lighting power state across a
 * suspend, a hibernation or an AC plug while the machine sleeps. Called from
 * the resume path so the keyboard comes back as the user left it.
 */
void omen_restore_lighting(void);

/**
 * omen_capture_lighting - Read the lighting state back from the EC
 *
 * Fn-F4 is handled by the EC firmware, so a press never reaches the driver and
 * the only way to learn about it is to read the bit back. Called before the
 * machine sleeps so resume can restore what the user actually left behind.
 */
void omen_capture_lighting(void);

/* Sysfs attribute callbacks */
ssize_t zone_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t zone_set(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count);
ssize_t all_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t all_set(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);
ssize_t brightness_show(struct device *dev, struct device_attribute *attr,
			 char *buf);
ssize_t brightness_set(struct device *dev, struct device_attribute *attr,
	       const char *buf, size_t count);

enum led_brightness omen_kbd_brightness_get(struct led_classdev *led_cdev);
int omen_kbd_brightness_set(struct led_classdev *led_cdev,
			    enum led_brightness value);

/**
 * fourzone_setup - Initialize zone management system
 * @dev: Platform device
 *
 * Returns: 0 on success, error code otherwise
 */
int fourzone_setup(struct platform_device *dev);

/**
 * fourzone_cleanup - Clean up zone management resources
 */
void fourzone_cleanup(void);

#endif /* OMEN_ZONES_H */

