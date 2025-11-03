// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Animation System
 *
 * RGB animation effects and control
 *
 * Author: alessandromrc
 */

#ifndef OMEN_ANIMATIONS_H
#define OMEN_ANIMATIONS_H

#include <linux/device.h>
#include <linux/types.h>

/* Animation system constants */
#define ANIMATION_TIMER_INTERVAL_MS 50
#define ANIMATION_SPEED_MIN 1
#define ANIMATION_SPEED_MAX 10
#define ANIMATION_SPEED_DEFAULT 1

enum animation_mode {
	ANIMATION_STATIC = 0,
	ANIMATION_BREATHING,
	ANIMATION_RAINBOW,
	ANIMATION_WAVE,
	ANIMATION_PULSE,
	ANIMATION_CHASE,
	ANIMATION_SPARKLE,
	ANIMATION_CANDLE,
	ANIMATION_AURORA,
	ANIMATION_DISCO,
	ANIMATION_COUNT
};

/* External animation state */
extern enum animation_mode current_animation;
extern int animation_speed;
extern bool animation_active;

/* Device attributes for sysfs */
extern struct device_attribute animation_brightness_attr;
extern struct device_attribute animation_mode_attr;
extern struct device_attribute animation_speed_attr;

/**
 * animation_init - Initialize animation system
 */
void animation_init(void);

/**
 * animation_cleanup - Clean up animation resources
 */
void animation_cleanup(void);

/**
 * animation_start - Start currently selected animation
 */
void animation_start(void);

/**
 * animation_stop - Stop current animation
 */
void animation_stop(void);

/**
 * animation_set_mode - Set animation mode
 * @mode: New animation mode
 */
void animation_set_mode(enum animation_mode mode);

/**
 * animation_get_mode - Get current animation mode
 *
 * Returns: Current animation mode
 */
enum animation_mode animation_get_mode(void);

/**
 * hsv_to_rgb - Convert HSV color to RGB
 * @h: Hue (0-360)
 * @s: Saturation (0-100)
 * @v: Value (0-100)
 * @rgb: Output RGB color
 */
void hsv_to_rgb(int h, int s, int v, struct color_platform *rgb);

#endif /* OMEN_ANIMATIONS_H */

