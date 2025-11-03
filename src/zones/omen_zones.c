// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Zone Management
 *
 * RGB zone management and LED control
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/string.h>

#include "omen_rgb_keyboard.h"
#include "omen_wmi.h"
#include "omen_zones.h"
#include "omen_animations.h"
#include "omen_state.h"

struct device_attribute *zone_dev_attrs;
struct attribute **zone_attrs;
struct platform_zone *zone_data;

struct platform_zone original_colors[ZONE_COUNT];
int global_brightness = 100;

static struct attribute_group zone_attribute_group = {
	.name = "rgb_zones",
};

int parse_rgb(const char *buf, struct platform_zone *zone)
{
	unsigned long rgb;
	int ret;
	union color_union {
		struct color_platform cp;
		int package;
	} repackager;

	ret = kstrtoul(buf, 16, &rgb);
	if (ret)
		return ret;
	if (rgb > 0xFFFFFF)
		return -EINVAL;

	repackager.package = rgb;
	pr_debug("r:%d g:%d b:%d\n",
		 repackager.cp.red, repackager.cp.green, repackager.cp.blue);
	zone->colors = repackager.cp;
	return 0;
}

struct platform_zone *match_zone(struct device_attribute *attr)
{
	u8 zone;
	for (zone = 0; zone < ZONE_COUNT; zone++) {
		if ((struct device_attribute *)zone_data[zone].attr == attr)
			return &zone_data[zone];
	}
	return NULL;
}

int fourzone_update_led(struct platform_zone *zone, enum hp_wmi_command rw)
{
	u8 state[128];
	int ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET, HPWMI_FOURZONE,
				       &state, sizeof(state), sizeof(state));
	if (ret) {
		pr_warn("fourzone_color_get returned error 0x%x\n", ret);
		return ret <= 0 ? ret : -EINVAL;
	}

	if (rw == HPWMI_WRITE) {
		state[zone->offset + 0] = zone->colors.red;
		state[zone->offset + 1] = zone->colors.green;
		state[zone->offset + 2] = zone->colors.blue;

		ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_SET, HPWMI_FOURZONE,
					   &state, sizeof(state), sizeof(state));
		if (ret)
			pr_warn("fourzone_color_set returned error 0x%x\n", ret);
		return ret;
	} else {
		zone->colors.red = state[zone->offset + 0];
		zone->colors.green = state[zone->offset + 1];
		zone->colors.blue = state[zone->offset + 2];
	}
	return 0;
}

void apply_brightness_to_color(struct color_platform *color)
{
	color->red = (color->red * global_brightness) / 100;
	color->green = (color->green * global_brightness) / 100;
	color->blue = (color->blue * global_brightness) / 100;
}

void update_all_zones_with_colors(struct color_platform colors[ZONE_COUNT])
{
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].colors = colors[zone];
		apply_brightness_to_color(&zone_data[zone].colors);
		fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
	}
}

ssize_t zone_show(struct device *dev, struct device_attribute *attr,
		  char *buf)
{
	struct platform_zone *target_zone = match_zone(attr);
	int ret;
	if (target_zone == NULL)
		return sprintf(buf, "red: -1, green: -1, blue: -1\n");
	ret = fourzone_update_led(target_zone, HPWMI_READ);
	if (ret)
		return sprintf(buf, "red: -1, green: -1, blue: -1\n");
	return sprintf(buf, "#%02x%02x%02x\n",
		       target_zone->colors.red,
		       target_zone->colors.green, target_zone->colors.blue);
}

ssize_t zone_set(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	struct platform_zone *target_zone = match_zone(attr);
	int ret;
	if (target_zone == NULL) {
		pr_err("invalid target zone\n");
		return 1;
	}
	ret = parse_rgb(buf, target_zone);
	if (ret)
		return ret;

	int zone_idx = target_zone - zone_data;
	original_colors[zone_idx].colors.red = target_zone->colors.red;
	original_colors[zone_idx].colors.green = target_zone->colors.green;
	original_colors[zone_idx].colors.blue = target_zone->colors.blue;

	animation_stop();
	animation_set_mode(ANIMATION_STATIC);

	target_zone->colors.red = (target_zone->colors.red * global_brightness) / 100;
	target_zone->colors.green = (target_zone->colors.green * global_brightness) / 100;
	target_zone->colors.blue = (target_zone->colors.blue * global_brightness) / 100;

	ret = fourzone_update_led(target_zone, HPWMI_WRITE);
	if (ret)
		return ret;
	
	/* Save state */
	save_animation_state();
	
	return count;
}

ssize_t brightness_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d\n", global_brightness);
}

ssize_t brightness_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	unsigned long level;
	int ret;

	if (kstrtoul(buf, 10, &level))
		return -EINVAL;
	if (level > 100)
		level = 100;

	global_brightness = level;

	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		/* Use the stored original colors and apply brightness */
		zone_data[zone].colors.red = (original_colors[zone].colors.red * level) / 100;
		zone_data[zone].colors.green = (original_colors[zone].colors.green * level) / 100;
		zone_data[zone].colors.blue = (original_colors[zone].colors.blue * level) / 100;

		ret = fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
		if (ret)
			return ret;
	}

	/* Save state */
	save_animation_state();

	return count;
}

ssize_t all_show(struct device *dev, struct device_attribute *attr,
		 char *buf)
{
	int ret;
	ret = fourzone_update_led(&zone_data[0], HPWMI_READ);
	if (ret)
		return sprintf(buf, "red: -1, green: -1, blue: -1\n");
	return sprintf(buf, "#%02x%02x%02x\n",
		       zone_data[0].colors.red,
		       zone_data[0].colors.green, zone_data[0].colors.blue);
}

ssize_t all_set(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct platform_zone temp;
	int ret;
	u8 z;

	ret = parse_rgb(buf, &temp);
	if (ret)
		return ret;

	animation_stop();
	animation_set_mode(ANIMATION_STATIC);

	for (z = 0; z < ZONE_COUNT; z++) {
		/* Store the new color as the original color */
		original_colors[z].colors = temp.colors;

		zone_data[z].colors.red = (temp.colors.red * global_brightness) / 100;
		zone_data[z].colors.green = (temp.colors.green * global_brightness) / 100;
		zone_data[z].colors.blue = (temp.colors.blue * global_brightness) / 100;

		ret = fourzone_update_led(&zone_data[z], HPWMI_WRITE);
		if (ret)
			return ret;
	}

	/* Save state */
	save_animation_state();

	return count;
}

int fourzone_setup(struct platform_device *dev)
{
	u8 zone;
	char buffer[10];
	char *name;

	zone_dev_attrs = kcalloc(ZONE_COUNT + 4, sizeof(struct device_attribute),
				 GFP_KERNEL);
	if (!zone_dev_attrs)
		return -ENOMEM;

	zone_attrs = kcalloc(ZONE_COUNT + 5, sizeof(struct attribute *),
			     GFP_KERNEL);
	if (!zone_attrs)
		return -ENOMEM;

	zone_data = kcalloc(ZONE_COUNT, sizeof(struct platform_zone),
			    GFP_KERNEL);
	if (!zone_data)
		return -ENOMEM;

	for (u8 zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].offset = 25 + (zone * 3);
		int ret = fourzone_update_led(&zone_data[zone], HPWMI_READ);
		if (ret)
			return ret;

		/* Store original colors */
		original_colors[zone].colors.red = zone_data[zone].colors.red;
		original_colors[zone].colors.green = zone_data[zone].colors.green;
		original_colors[zone].colors.blue = zone_data[zone].colors.blue;
	}

	for (zone = 0; zone < ZONE_COUNT; zone++) {
		sprintf(buffer, "zone%02hhX", zone);
		name = kstrdup(buffer, GFP_KERNEL);
		if (!name)
			return -ENOMEM;

		sysfs_attr_init(&zone_dev_attrs[zone].attr);
		zone_dev_attrs[zone].attr.name = name;
		zone_dev_attrs[zone].attr.mode = 0644;
		zone_dev_attrs[zone].show = zone_show;
		zone_dev_attrs[zone].store = zone_set;
		zone_data[zone].offset = 25 + (zone * 3);
		zone_attrs[zone] = &zone_dev_attrs[zone].attr;
		zone_data[zone].attr = &zone_dev_attrs[zone];
	}

	sysfs_attr_init(&zone_dev_attrs[ZONE_COUNT].attr);
	zone_dev_attrs[ZONE_COUNT].attr.name = "all";
	zone_dev_attrs[ZONE_COUNT].attr.mode = 0644;
	zone_dev_attrs[ZONE_COUNT].show = all_show;
	zone_dev_attrs[ZONE_COUNT].store = all_set;
	zone_attrs[ZONE_COUNT] = &zone_dev_attrs[ZONE_COUNT].attr;

	zone_attrs[ZONE_COUNT + 1] = &animation_brightness_attr.attr;
	zone_attrs[ZONE_COUNT + 2] = &animation_mode_attr.attr;
	zone_attrs[ZONE_COUNT + 3] = &animation_speed_attr.attr;
	zone_attrs[ZONE_COUNT + 4] = NULL; /* NULL terminate the array */

	zone_attribute_group.attrs = zone_attrs;
	
	return sysfs_create_group(&dev->dev.kobj, &zone_attribute_group);
}

void fourzone_cleanup(void)
{
	/* Free allocated zone attribute names */
	if (zone_dev_attrs) {
		for (u8 zone = 0; zone < ZONE_COUNT; zone++) {
			if (zone_dev_attrs[zone].attr.name)
				kfree(zone_dev_attrs[zone].attr.name);
		}
	}

	kfree(zone_dev_attrs);
	kfree(zone_attrs);
	kfree(zone_data);
}

