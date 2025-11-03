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
 * update_all_zones_with_colors - Update all zones with new colors
 * @colors: Array of colors for all zones
 */
void update_all_zones_with_colors(struct color_platform colors[ZONE_COUNT]);

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

