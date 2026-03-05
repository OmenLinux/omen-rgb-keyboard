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
#include <linux/mutex.h>

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
struct gradient_config gradient_cfg;
DEFINE_MUTEX(gradient_cfg_mutex);

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

static void animation_gradient(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long step_time = msecs_to_jiffies(3000 / animation_speed);
	struct color_platform colors[ZONE_COUNT];
	struct gradient_config cfg_snapshot;
	int g, z;

	/* Take a snapshot of the config under the lock */
	mutex_lock(&gradient_cfg_mutex);
	cfg_snapshot = gradient_cfg;
	mutex_unlock(&gradient_cfg_mutex);

	/* Start with current zone colors as fallback for ungrouped zones */
	for (z = 0; z < ZONE_COUNT; z++)
		colors[z] = original_colors[z].colors;

	for (g = 0; g < cfg_snapshot.group_count; g++) {
		struct gradient_group *grp = &cfg_snapshot.groups[g];
		unsigned long total_cycle, cycle_pos, frac;
		int idx_a, idx_b;
		struct color_platform interpolated;

		if (grp->color_count == 0)
			continue;

		/* Total cycle = time per color transition * number of colors */
		total_cycle = step_time * grp->color_count;
		cycle_pos = elapsed % total_cycle;

		/* Which two colors are we interpolating between? */
		idx_a = cycle_pos / step_time;
		idx_b = (idx_a + 1) % grp->color_count;

		/* Fractional position between color A and B (0 to step_time) */
		frac = cycle_pos - (idx_a * step_time);

		/* Linear interpolation using integer math */
		interpolated.red   = grp->colors[idx_a].red   + (int)(grp->colors[idx_b].red   - grp->colors[idx_a].red)   * (int)frac / (int)step_time;
		interpolated.green = grp->colors[idx_a].green + (int)(grp->colors[idx_b].green - grp->colors[idx_a].green) * (int)frac / (int)step_time;
		interpolated.blue  = grp->colors[idx_a].blue  + (int)(grp->colors[idx_b].blue  - grp->colors[idx_a].blue)  * (int)frac / (int)step_time;

		for (z = 0; z < ZONE_COUNT; z++) {
			if (grp->zone_mask & (1 << z))
				colors[z] = interpolated;
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
	case ANIMATION_GRADIENT:
		animation_gradient();
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

	/* Start the timer (already initialized in animation_init) */
	mod_timer(&animation_timer, jiffies + msecs_to_jiffies(ANIMATION_TIMER_INTERVAL_MS));
}

void animation_stop(void)
{
	animation_active = false;
	timer_delete_sync(&animation_timer);
	cancel_work_sync(&animation_work);

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
		"chase", "sparkle", "candle", "aurora", "disco", "gradient"
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
	} else if (strncmp(buf, "gradient", 8) == 0) {
		new_mode = ANIMATION_GRADIENT;
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

static ssize_t gradient_config_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct gradient_config cfg_snapshot;
	int g, z, c, len = 0;

	mutex_lock(&gradient_cfg_mutex);
	cfg_snapshot = gradient_cfg;
	mutex_unlock(&gradient_cfg_mutex);

	for (g = 0; g < cfg_snapshot.group_count; g++) {
		struct gradient_group *grp = &cfg_snapshot.groups[g];
		bool first_zone = true;
		bool first_color = true;

		if (g > 0) {
			if (len >= PAGE_SIZE - 1)
				break;
			len += scnprintf(buf + len, PAGE_SIZE - len, ";");
		}

		/* Print zones */
		for (z = 0; z < ZONE_COUNT; z++) {
			if (grp->zone_mask & (1 << z)) {
				if (!first_zone)
					len += scnprintf(buf + len, PAGE_SIZE - len, ",");
				len += scnprintf(buf + len, PAGE_SIZE - len, "%d", z);
				first_zone = false;
			}
		}

		len += scnprintf(buf + len, PAGE_SIZE - len, ":");

		/* Print colors */
		for (c = 0; c < grp->color_count; c++) {
			if (!first_color)
				len += scnprintf(buf + len, PAGE_SIZE - len, ",");
			len += scnprintf(buf + len, PAGE_SIZE - len, "%02X%02X%02X",
					 grp->colors[c].red,
					 grp->colors[c].green,
					 grp->colors[c].blue);
			first_color = false;
		}
	}

	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t gradient_config_set(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct gradient_config new_cfg;
	char local_buf[512];
	char *group_str, *groups_ptr;
	int g = 0;

	if (count == 0)
		return -EINVAL;

	memset(&new_cfg, 0, sizeof(new_cfg));

	if (count >= sizeof(local_buf))
		return -EINVAL;

	strscpy(local_buf, buf, count + 1);

	/* Strip trailing newline */
	if (local_buf[count - 1] == '\n')
		local_buf[count - 1] = '\0';

	groups_ptr = local_buf;

	/* Parse groups separated by ';' */
	while ((group_str = strsep(&groups_ptr, ";")) != NULL) {
		char *zones_str, *colors_str, *token;
		char *zones_ptr, *colors_ptr;
		struct gradient_group *grp;

		if (*group_str == '\0')
			continue;

		if (g >= GRADIENT_MAX_GROUPS)
			return -EINVAL;

		grp = &new_cfg.groups[g];

		/* Split on ':' to get zones and colors */
		zones_str = strsep(&group_str, ":");
		colors_str = group_str;

		if (!zones_str || !colors_str || *colors_str == '\0')
			return -EINVAL;

		/* Parse zone numbers */
		zones_ptr = zones_str;
		grp->zone_mask = 0;
		while ((token = strsep(&zones_ptr, ",")) != NULL) {
			unsigned long zone_num;

			if (kstrtoul(token, 10, &zone_num))
				return -EINVAL;
			if (zone_num >= ZONE_COUNT)
				return -EINVAL;

			grp->zone_mask |= (1 << zone_num);
		}

		if (grp->zone_mask == 0)
			return -EINVAL;

		/* Parse hex colors */
		colors_ptr = colors_str;
		grp->color_count = 0;
		while ((token = strsep(&colors_ptr, ",")) != NULL) {
			unsigned long rgb;

			if (grp->color_count >= GRADIENT_MAX_COLORS)
				return -EINVAL;

			if (kstrtoul(token, 16, &rgb))
				return -EINVAL;
			if (rgb > 0xFFFFFF)
				return -EINVAL;

			grp->colors[grp->color_count].red = (rgb >> 16) & 0xFF;
			grp->colors[grp->color_count].green = (rgb >> 8) & 0xFF;
			grp->colors[grp->color_count].blue = rgb & 0xFF;
			grp->color_count++;
		}

		if (grp->color_count == 0)
			return -EINVAL;

		g++;
	}

	new_cfg.group_count = g;

	if (new_cfg.group_count == 0)
		return -EINVAL;

	/* Apply new config under lock */
	mutex_lock(&gradient_cfg_mutex);
	gradient_cfg = new_cfg;
	mutex_unlock(&gradient_cfg_mutex);

	/* Save state */
	save_animation_state();

	return count;
}

struct device_attribute animation_brightness_attr = __ATTR(brightness, 0644, brightness_show, brightness_set);
struct device_attribute animation_mode_attr = __ATTR(animation_mode, 0644, animation_mode_show, animation_mode_set);
struct device_attribute animation_speed_attr = __ATTR(animation_speed, 0644, animation_speed_show, animation_speed_set);
struct device_attribute gradient_config_attr = __ATTR(gradient_config, 0644, gradient_config_show, gradient_config_set);

void animation_init(void)
{
	INIT_WORK(&animation_work, animation_work_func);
	timer_setup(&animation_timer, animation_timer_callback, 0);
}

void animation_cleanup(void)
{
	animation_stop();

	/* Cancel any pending work */
	cancel_work_sync(&animation_work);
}
