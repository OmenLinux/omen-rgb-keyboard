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
#include <linux/leds.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "omen_rgb_keyboard.h"
#include "omen_wmi.h"
#include "omen_zones.h"
#include "omen_animations.h"
#include "omen_state.h"
#include "omen_hda_led.h"

struct device_attribute *zone_dev_attrs;
struct attribute **zone_attrs;
struct platform_zone *zone_data;

static struct platform_device *zone_platform_dev;

struct platform_zone original_colors[ZONE_COUNT];
int global_brightness = 100;

static bool brightness_lock;
module_param(brightness_lock, bool, 0644);
MODULE_PARM_DESC(brightness_lock,
		 "Ignore keyboard backlight brightness changes requested through the LED class device, so a desktop cannot dim the keyboard (default 0)");

static bool lighting_on_boot = true;
module_param(lighting_on_boot, bool, 0644);
MODULE_PARM_DESC(lighting_on_boot,
		 "Switch the keyboard backlight on when the driver loads, unless the saved brightness is zero (default 1)");

/*
 * Last lighting power state requested from this driver. The EC drops it, and
 * the zone colours with it, across a suspend, a hibernation or an AC plug
 * while the machine sleeps. Keeping the request here lets resume and the
 * Fn-F4 toggle put the keyboard back exactly as the user left it.
 */
static bool lighting_enabled = true;

struct led_classdev omen_kbd_led;

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
	int ret;

	if (!zone || zone->offset > sizeof(state) - 3)
		return -EINVAL;

	ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET, HPWMI_FOURZONE,
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

int fourzone_set_lighting(bool on)
{
	u8 state[4] = { on ? HPWMI_FOURZONE_LIGHTING_ON :
			      HPWMI_FOURZONE_LIGHTING_OFF, 0, 0, 0 };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FOURZONE_BRIGHT_SET, HPWMI_FOURZONE,
				   state, sizeof(state), 0);
	if (!ret)
		lighting_enabled = on;

	return ret;
}

int fourzone_get_lighting(void)
{
	u8 state[4] = {0};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_FOURZONE_BRIGHT_GET, HPWMI_FOURZONE,
				   state, sizeof(state), sizeof(state));
	if (ret)
		return ret <= 0 ? ret : -EINVAL;

	return state[0] == HPWMI_FOURZONE_LIGHTING_ON;
}

void fourzone_lighting_on_boot(void)
{
	bool on = lighting_on_boot && global_brightness > 0;
	int ret;

	/* Write the state either way, so the intent is not left half-applied */
	ret = fourzone_set_lighting(on);
	if (ret)
		pr_warn("Failed to switch the keyboard backlight on: %d\n", ret);
}

void fourzone_apply_current_colors(void)
{
	for (u8 zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].colors = original_colors[zone].colors;
		apply_brightness_to_color(&zone_data[zone].colors);
		fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
	}
}

void update_all_zones_with_colors(struct color_platform colors[ZONE_COUNT])
{
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].colors = colors[zone];
		apply_brightness_to_color(&zone_data[zone].colors);
		fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
	}
}

void omen_restore_lighting(void)
{
	int ret;

	/*
	 * Nothing else repaints a static configuration, and the EC may have
	 * dropped the zone colours while the machine slept, so the colours go
	 * back first. A running animation rewrites every zone on its next tick
	 * and would be painted over by the static colours here.
	 */
	if (!animation_active)
		fourzone_apply_current_colors();

	ret = fourzone_set_lighting(lighting_enabled);
	if (ret)
		pr_warn("Failed to restore keyboard lighting after resume: %d\n",
			ret);
}

void omen_capture_lighting(void)
{
	int state = fourzone_get_lighting();

	/*
	 * Fn-F4 never reaches the driver: the EC firmware toggles the lighting
	 * bit itself and the press is not forwarded to the OS, so reading the
	 * bit back is the only way to learn what the user left behind. It has
	 * to happen before the machine sleeps, because the EC drops the bit
	 * during sleep and a resume-time read would always report "off".
	 */
	if (state >= 0)
		lighting_enabled = state > 0;
}

static int omen_apply_brightness(unsigned long level)
{
	int ret;

	if (level > 100)
		level = 100;

	global_brightness = level;

	/*
	 * While an animation is running it owns the zone colours and rewrites
	 * all of them on its next tick, which is at most
	 * ANIMATION_TIMER_INTERVAL_MS away. Writing the static colours here
	 * would put them on the keyboard until that tick lands, so a desktop
	 * that fades the backlight over a second paints the static colour over
	 * the animation for the whole fade. Record the level and let the
	 * animation apply it; apply_brightness_to_color() already scales every
	 * frame by global_brightness.
	 */
	if (animation_active) {
		save_animation_state();
		return 0;
	}

	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].colors.red = (original_colors[zone].colors.red * level) / 100;
		zone_data[zone].colors.green = (original_colors[zone].colors.green * level) / 100;
		zone_data[zone].colors.blue = (original_colors[zone].colors.blue * level) / 100;

		ret = fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
		if (ret)
			return ret;
	}

	save_animation_state();
	return 0;
}

enum led_brightness omen_kbd_brightness_get(struct led_classdev *led_cdev)
{
	return global_brightness;
}

int omen_kbd_brightness_set(struct led_classdev *led_cdev,
			    enum led_brightness value)
{
	/*
	 * This is the path desktops drive, and it is the one an idle timer
	 * fades down. Report success without touching the hardware when
	 * locked: returning an error here only makes callers retry or log,
	 * and rgb_zones/brightness still sets the level directly.
	 */
	if (brightness_lock)
		return 0;

	return omen_apply_brightness(value);
}

ssize_t zone_show(struct device *dev, struct device_attribute *attr,
		  char *buf)
{
	struct platform_zone *target_zone = match_zone(attr);
	int ret;
	if (target_zone == NULL)
		return sysfs_emit(buf, "red: -1, green: -1, blue: -1\n");
	ret = fourzone_update_led(target_zone, HPWMI_READ);
	if (ret)
		return sysfs_emit(buf, "red: -1, green: -1, blue: -1\n");
	return sysfs_emit(buf, "#%02x%02x%02x\n",
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
		return -EINVAL;
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
	return sysfs_emit(buf, "%d\n", global_brightness);
}

ssize_t brightness_set(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	unsigned long level;
	int ret;

	if (kstrtoul(buf, 10, &level))
		return -EINVAL;

	ret = omen_apply_brightness(level);
	if (ret)
		return ret;

	return count;
}

static ssize_t lighting_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int ret = fourzone_get_lighting();

	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", ret);
}

static ssize_t lighting_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	ret = fourzone_set_lighting(on);
	if (ret)
		return ret <= 0 ? ret : -EINVAL;

	return count;
}

static DEVICE_ATTR(lighting, 0664, lighting_show, lighting_store);

void fourzone_apply_brightness_lock(void)
{
	/*
	 * Holding the backlight at zero is a trap: the lock is precisely what
	 * stops the desktop writing to the backlight, so there is nothing left
	 * to raise it again, and a keyboard that stays dark is never what the
	 * option is for. Come up to full instead and let rgb_zones/brightness
	 * take it down deliberately if that is really wanted.
	 */
	if (brightness_lock && global_brightness == 0)
		omen_apply_brightness(100);
}

static ssize_t brightness_lock_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", brightness_lock);
}

static ssize_t brightness_lock_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	bool lock;
	int ret;

	ret = kstrtobool(buf, &lock);
	if (ret)
		return ret;

	brightness_lock = lock;
	fourzone_apply_brightness_lock();

	return count;
}

static struct device_attribute brightness_lock_attr =
	__ATTR(brightness_lock, 0664, brightness_lock_show, brightness_lock_store);

ssize_t all_show(struct device *dev, struct device_attribute *attr,
		 char *buf)
{
	int ret;
	ret = fourzone_update_led(&zone_data[0], HPWMI_READ);
	if (ret)
		return sysfs_emit(buf, "red: -1, green: -1, blue: -1\n");
	return sysfs_emit(buf, "#%02x%02x%02x\n",
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

static ssize_t mute_led_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	/* We don't have a way to read the LED state, so just return help text */
	return sysfs_emit(buf, "Write '1' to turn on, '0' to turn off\n");
}

static ssize_t mute_led_set(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long value;
	int ret;

	ret = kstrtoul(buf, 10, &value);
	if (ret)
		return ret;

	/* Set mute button LED state based on value */
	ret = omen_hda_led_set(value ? true : false);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR(mute_led, 0664, mute_led_show, mute_led_set);

static ssize_t mute_state_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	return sysfs_emit(buf, "Write '1' for muted, '0' for unmuted (from userspace daemon)\n");
}

static ssize_t mute_state_set(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long value;
	int ret;

	ret = kstrtoul(buf, 10, &value);
	if (ret)
		return ret;

	/* Set mute state from userspace (e.g., PipeWire daemon) */
	ret = omen_hda_led_set_mute_state(value ? true : false);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR(mute_state, 0664, mute_state_show, mute_state_set);

int fourzone_setup(struct platform_device *dev)
{
	u8 zone;
	char buffer[10];
	char *name;
	int ret;

	zone_dev_attrs = kcalloc(ZONE_COUNT + 6, sizeof(struct device_attribute),
				 GFP_KERNEL);
	if (!zone_dev_attrs)
		return -ENOMEM;

	zone_attrs = kcalloc(ZONE_COUNT + 10, sizeof(struct attribute *),
			     GFP_KERNEL);
	if (!zone_attrs) {
		ret = -ENOMEM;
		goto err_free_dev_attrs;
	}

	zone_data = kcalloc(ZONE_COUNT, sizeof(struct platform_zone),
			    GFP_KERNEL);
	if (!zone_data) {
		ret = -ENOMEM;
		goto err_free_zone_attrs;
	}

	for (zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].offset = 25 + (zone * 3);
		ret = fourzone_update_led(&zone_data[zone], HPWMI_READ);
		if (ret)
			goto err_free_zone_data;

		/* Store original colors */
		original_colors[zone].colors.red = zone_data[zone].colors.red;
		original_colors[zone].colors.green = zone_data[zone].colors.green;
		original_colors[zone].colors.blue = zone_data[zone].colors.blue;
	}

	for (zone = 0; zone < ZONE_COUNT; zone++) {
		if (snprintf(buffer, sizeof(buffer), "zone%02X", zone) >=
		    (int)sizeof(buffer)) {
			ret = -EINVAL;
			goto err_free_names;
		}
		name = kstrdup(buffer, GFP_KERNEL);
		if (!name) {
			ret = -ENOMEM;
			goto err_free_names;
		}

		sysfs_attr_init(&zone_dev_attrs[zone].attr);
		zone_dev_attrs[zone].attr.name = name;
		zone_dev_attrs[zone].attr.mode = 0664;
		zone_dev_attrs[zone].show = zone_show;
		zone_dev_attrs[zone].store = zone_set;
		zone_data[zone].offset = 25 + (zone * 3);
		zone_attrs[zone] = &zone_dev_attrs[zone].attr;
		zone_data[zone].attr = &zone_dev_attrs[zone];
	}

	sysfs_attr_init(&zone_dev_attrs[ZONE_COUNT].attr);
	zone_dev_attrs[ZONE_COUNT].attr.name = "all";
	zone_dev_attrs[ZONE_COUNT].attr.mode = 0664;
	zone_dev_attrs[ZONE_COUNT].show = all_show;
	zone_dev_attrs[ZONE_COUNT].store = all_set;
	zone_attrs[ZONE_COUNT] = &zone_dev_attrs[ZONE_COUNT].attr;

	zone_attrs[ZONE_COUNT + 1] = &animation_brightness_attr.attr;
	zone_attrs[ZONE_COUNT + 2] = &animation_mode_attr.attr;
	zone_attrs[ZONE_COUNT + 3] = &animation_speed_attr.attr;
	zone_attrs[ZONE_COUNT + 4] = &gradient_config_attr.attr;
	zone_attrs[ZONE_COUNT + 5] = &dev_attr_mute_led.attr;
	zone_attrs[ZONE_COUNT + 6] = &dev_attr_mute_state.attr;
	zone_attrs[ZONE_COUNT + 7] = &brightness_lock_attr.attr;
	zone_attrs[ZONE_COUNT + 8] = &dev_attr_lighting.attr;
	zone_attrs[ZONE_COUNT + 9] = NULL; /* NULL terminate the array */

	zone_attribute_group.attrs = zone_attrs;

	ret = sysfs_create_group(&dev->dev.kobj, &zone_attribute_group);
	if (ret)
		goto err_free_names;

	zone_platform_dev = dev;
	omen_kbd_led.name = "omen::kbd_backlight";
	omen_kbd_led.max_brightness = 100;
	omen_kbd_led.brightness_set_blocking = omen_kbd_brightness_set;
	omen_kbd_led.brightness_get = omen_kbd_brightness_get;
	/*
	 * The LED is devm-registered, so it is unregistered by
	 * platform_device_unregister() in hp_wmi_exit(), after
	 * fourzone_cleanup() has freed zone_data. Without this flag,
	 * led_classdev_unregister() sets the LED to 0, which writes black to
	 * the EC through the freed zone_data and queues a state save with
	 * brightness 0 after flush_animation_state() has already run.
	 */
	omen_kbd_led.flags |= LED_RETAIN_AT_SHUTDOWN;
	return 0;

err_free_names:
	for (u8 z = 0; z < ZONE_COUNT; z++) {
		if (zone_dev_attrs[z].attr.name)
			kfree(zone_dev_attrs[z].attr.name);
	}
err_free_zone_data:
	kfree(zone_data);
	zone_data = NULL;
err_free_zone_attrs:
	kfree(zone_attrs);
	zone_attrs = NULL;
err_free_dev_attrs:
	kfree(zone_dev_attrs);
	zone_dev_attrs = NULL;
	return ret;
}

void fourzone_cleanup(void)
{
	/* Remove sysfs group before freeing backing memory */
	if (zone_platform_dev)
		sysfs_remove_group(&zone_platform_dev->dev.kobj, &zone_attribute_group);

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
	zone_dev_attrs = NULL;
	zone_attrs = NULL;
	zone_data = NULL;
}

