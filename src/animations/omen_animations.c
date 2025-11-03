// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Animation System
 *
 * RGB animation effects and control
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/math.h>
#include <linux/string.h>

#include "omen_rgb_keyboard.h"
#include "omen_wmi.h"
#include "omen_animations.h"
#include "omen_zones.h"
#include "omen_state.h"
#include "../utils/math/sin_lut.h"

/* Animation state */
enum animation_mode current_animation = ANIMATION_STATIC;
int animation_speed = ANIMATION_SPEED_DEFAULT;
bool animation_active = false;

static struct timer_list animation_timer;
static struct work_struct animation_work;
static unsigned long animation_start_time;

void hsv_to_rgb(int h, int s, int v, struct color_platform *rgb)
{
	int c = (v * s) / 100;
	int x = c * (60 - abs((h % 120) - 60)) / 60;
	int m = v - c;

	int r, g, b;

	if (h < 60) {
		r = c; g = x; b = 0;
	} else if (h < 120) {
		r = x; g = c; b = 0;
	} else if (h < 180) {
		r = 0; g = c; b = x;
	} else if (h < 240) {
		r = 0; g = x; b = c;
	} else if (h < 300) {
		r = x; g = 0; b = c;
	} else {
		r = c; g = 0; b = x;
	}

	rgb->red = (r + m) * 255 / 100;
	rgb->green = (g + m) * 255 / 100;
	rgb->blue = (b + m) * 255 / 100;
}

/* Animation implementations */
static void animation_breathing(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(2000 / animation_speed); /* 2 second cycle */
	unsigned long cycle_pos = elapsed % cycle_time;

	int angle = (360 * cycle_pos) / cycle_time;
	int intensity = 50 + (50 * lut_sin(angle)) / 100;

	struct color_platform colors[ZONE_COUNT];
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		colors[zone] = original_colors[zone].colors;
		colors[zone].red = (colors[zone].red * intensity) / 100;
		colors[zone].green = (colors[zone].green * intensity) / 100;
		colors[zone].blue = (colors[zone].blue * intensity) / 100;
	}

	update_all_zones_with_colors(colors);
}

static void animation_rainbow(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(3000 / animation_speed); /* 3 second cycle */
	unsigned long cycle_pos = elapsed % cycle_time;

	struct color_platform colors[ZONE_COUNT];
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		int hue = (360 * cycle_pos / cycle_time + zone * 90) % 360;
		hsv_to_rgb(hue, 100, 100, &colors[zone]);
	}

	update_all_zones_with_colors(colors);
}

static void animation_wave(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(2000 / animation_speed); /* 2 second cycle */
	unsigned long cycle_pos = elapsed % cycle_time;

	struct color_platform colors[ZONE_COUNT];
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		int wave_pos = (cycle_pos * 4 / cycle_time + zone) % 4;
		int angle = (360 * wave_pos) / 4;
		int intensity = 30 + (70 * (100 + lut_sin(angle)) / 200);

		colors[zone] = original_colors[zone].colors;
		colors[zone].red = (colors[zone].red * intensity) / 100;
		colors[zone].green = (colors[zone].green * intensity) / 100;
		colors[zone].blue = (colors[zone].blue * intensity) / 100;
	}

	update_all_zones_with_colors(colors);
}

static void animation_pulse(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(1500 / animation_speed); /* 1.5 second cycle */
	unsigned long cycle_pos = elapsed % cycle_time;

	int angle = (360 * cycle_pos) / cycle_time;
	int intensity = 20 + (80 * (100 + lut_sin(angle)) / 200);

	struct color_platform colors[ZONE_COUNT];
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		colors[zone] = original_colors[zone].colors;
		colors[zone].red = (colors[zone].red * intensity) / 100;
		colors[zone].green = (colors[zone].green * intensity) / 100;
		colors[zone].blue = (colors[zone].blue * intensity) / 100;
	}

	update_all_zones_with_colors(colors);
}

static void animation_chase(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(1200 / animation_speed); /* 1.2 second cycle */
	unsigned long cycle_pos = elapsed % cycle_time;

	struct color_platform colors[ZONE_COUNT];
	int active_zone = (cycle_pos * ZONE_COUNT) / cycle_time;

	struct color_platform base_color = original_colors[0].colors;

	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		if (zone == active_zone) {
			colors[zone] = base_color;
		} else {
			colors[zone] = base_color;
			colors[zone].red = colors[zone].red / 6;
			colors[zone].green = colors[zone].green / 6;
			colors[zone].blue = colors[zone].blue / 6;
		}
	}

	update_all_zones_with_colors(colors);
}

static void animation_sparkle(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(3000 / animation_speed);

	struct color_platform colors[ZONE_COUNT];
	struct color_platform base_color = original_colors[0].colors;

	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		int sparkle_offset = (elapsed + zone * 800) % cycle_time;
		int sparkle_duration = cycle_time / 8; /* Short sparkle duration */

		if (sparkle_offset < sparkle_duration) {
			colors[zone].red = 255;
			colors[zone].green = 255;
			colors[zone].blue = 255;
		} else {
			colors[zone] = base_color;
			colors[zone].red = colors[zone].red / 8;
			colors[zone].green = colors[zone].green / 8;
			colors[zone].blue = colors[zone].blue / 8;
		}
	}

	update_all_zones_with_colors(colors);
}

static void animation_candle(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(100 / animation_speed); /* Fast flicker */
	unsigned long cycle_pos = elapsed % cycle_time;

	struct color_platform colors[ZONE_COUNT];

	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		/* Candle flicker - warm colors with random intensity */
		int flicker = (cycle_pos + zone * 500) % cycle_time;
		int intensity = 60 + (40 * flicker) / cycle_time;

		colors[zone].red = (255 * intensity) / 100;
		colors[zone].green = (150 * intensity) / 100;
		colors[zone].blue = (50 * intensity) / 100;
	}

	update_all_zones_with_colors(colors);
}

static void animation_aurora(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(4000 / animation_speed); /* Slow aurora */
	unsigned long cycle_pos = elapsed % cycle_time;

	struct color_platform colors[ZONE_COUNT];

	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		int wave_pos = (cycle_pos * 2 + zone * 1000) % cycle_time;
		int intensity = 30 + (70 * (100 + lut_sin((360 * wave_pos) / cycle_time)) / 200);

		/* Aurora colors - green and blue */
		colors[zone].red = (20 * intensity) / 100;
		colors[zone].green = (200 * intensity) / 100;
		colors[zone].blue = (180 * intensity) / 100;
	}

	update_all_zones_with_colors(colors);
}

static void animation_disco(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(300 / animation_speed); /* Fast strobe */
	unsigned long cycle_pos = elapsed % cycle_time;

	struct color_platform colors[ZONE_COUNT];

	/* Disco strobe - bright colors that flash */
	if (cycle_pos < cycle_time / 2) {
		/* Flash on */
		for (int zone = 0; zone < ZONE_COUNT; zone++) {
			/* Different bright colors for each zone */
			switch (zone) {
			case 0: colors[zone].red = 255; colors[zone].green = 0; colors[zone].blue = 0; break;
			case 1: colors[zone].red = 0; colors[zone].green = 255; colors[zone].blue = 0; break;
			case 2: colors[zone].red = 0; colors[zone].green = 0; colors[zone].blue = 255; break;
			case 3: colors[zone].red = 255; colors[zone].green = 0; colors[zone].blue = 255; break;
			}
		}
	} else {
		/* Flash off */
		for (int zone = 0; zone < ZONE_COUNT; zone++) {
			colors[zone].red = 0;
			colors[zone].green = 0;
			colors[zone].blue = 0;
		}
	}

	update_all_zones_with_colors(colors);
}

/* Animation work function - runs in work queue context */
static void animation_work_func(struct work_struct *work)
{
	if (!animation_active || current_animation == ANIMATION_STATIC)
		return;

	switch (current_animation) {
	case ANIMATION_BREATHING:
		animation_breathing();
		break;
	case ANIMATION_RAINBOW:
		animation_rainbow();
		break;
	case ANIMATION_WAVE:
		animation_wave();
		break;
	case ANIMATION_PULSE:
		animation_pulse();
		break;
	case ANIMATION_CHASE:
		animation_chase();
		break;
	case ANIMATION_SPARKLE:
		animation_sparkle();
		break;
	case ANIMATION_CANDLE:
		animation_candle();
		break;
	case ANIMATION_AURORA:
		animation_aurora();
		break;
	case ANIMATION_DISCO:
		animation_disco();
		break;
	default:
		break;
	}
}

/* Animation timer callback */
static void animation_timer_callback(struct timer_list *t)
{
	if (animation_active && current_animation != ANIMATION_STATIC) {
		schedule_work(&animation_work);
		mod_timer(&animation_timer, jiffies + msecs_to_jiffies(ANIMATION_TIMER_INTERVAL_MS));
	}
}

void animation_start(void)
{
	if (current_animation == ANIMATION_STATIC) {
		animation_active = false;
		return;
	}

	animation_start_time = jiffies;
	animation_active = true;

	/* Start the timer */
	timer_setup(&animation_timer, animation_timer_callback, 0);
	mod_timer(&animation_timer, jiffies + msecs_to_jiffies(ANIMATION_TIMER_INTERVAL_MS));
}

void animation_stop(void)
{
	animation_active = false;
	timer_delete(&animation_timer);

	/* Restore original colors */
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].colors = original_colors[zone].colors;
		apply_brightness_to_color(&zone_data[zone].colors);
		fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
	}
}

void animation_set_mode(enum animation_mode mode)
{
	current_animation = mode;
}

enum animation_mode animation_get_mode(void)
{
	return current_animation;
}

/* Sysfs attribute callbacks */
static ssize_t animation_mode_show(struct device *dev, struct device_attribute *attr,
				   char *buf)
{
	const char *mode_names[] = {
		"static", "breathing", "rainbow", "wave", "pulse",
		"chase", "sparkle", "candle", "aurora", "disco"
	};

	if (current_animation >= ANIMATION_COUNT)
		return sprintf(buf, "unknown\n");

	return sprintf(buf, "%s\n", mode_names[current_animation]);
}

static ssize_t animation_mode_set(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	enum animation_mode new_mode = ANIMATION_STATIC;

	if (strncmp(buf, "static", 6) == 0) {
		new_mode = ANIMATION_STATIC;
	} else if (strncmp(buf, "breathing", 9) == 0) {
		new_mode = ANIMATION_BREATHING;
	} else if (strncmp(buf, "rainbow", 7) == 0) {
		new_mode = ANIMATION_RAINBOW;
	} else if (strncmp(buf, "wave", 4) == 0) {
		new_mode = ANIMATION_WAVE;
	} else if (strncmp(buf, "pulse", 5) == 0) {
		new_mode = ANIMATION_PULSE;
	} else if (strncmp(buf, "chase", 5) == 0) {
		new_mode = ANIMATION_CHASE;
	} else if (strncmp(buf, "sparkle", 7) == 0) {
		new_mode = ANIMATION_SPARKLE;
	} else if (strncmp(buf, "candle", 6) == 0) {
		new_mode = ANIMATION_CANDLE;
	} else if (strncmp(buf, "aurora", 6) == 0) {
		new_mode = ANIMATION_AURORA;
	} else if (strncmp(buf, "disco", 5) == 0) {
		new_mode = ANIMATION_DISCO;
	} else {
		return -EINVAL;
	}

	animation_stop();

	current_animation = new_mode;

	if (new_mode != ANIMATION_STATIC) {
		animation_start();
	}

	/* Save state */
	save_animation_state();

	return count;
}

static ssize_t animation_speed_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	return sprintf(buf, "%d\n", animation_speed);
}

static ssize_t animation_speed_set(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long speed;
	int ret;

	ret = kstrtoul(buf, 10, &speed);
	if (ret)
		return ret;

	if (speed < ANIMATION_SPEED_MIN || speed > ANIMATION_SPEED_MAX)
		return -EINVAL;

	animation_speed = speed;

	if (animation_active && current_animation != ANIMATION_STATIC) {
		animation_stop();
		animation_start();
	}

	/* Save state */
	save_animation_state();

	return count;
}

DEVICE_ATTR(brightness, 0644, brightness_show, brightness_set);
DEVICE_ATTR(animation_mode, 0644, animation_mode_show, animation_mode_set);
DEVICE_ATTR(animation_speed, 0644, animation_speed_show, animation_speed_set);

struct device_attribute animation_brightness_attr = __ATTR(brightness, 0644, brightness_show, brightness_set);
struct device_attribute animation_mode_attr = __ATTR(animation_mode, 0644, animation_mode_show, animation_mode_set);
struct device_attribute animation_speed_attr = __ATTR(animation_speed, 0644, animation_speed_show, animation_speed_set);

void animation_init(void)
{
	INIT_WORK(&animation_work, animation_work_func);
}

void animation_cleanup(void)
{
	animation_stop();

	/* Cancel any pending work */
	cancel_work_sync(&animation_work);
}
