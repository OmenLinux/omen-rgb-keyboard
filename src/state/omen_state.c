// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - State Persistence
 *
 * State saving and loading functionality
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "omen_state.h"
#include "omen_zones.h"
#include "omen_animations.h"

void save_animation_state(void)
{
	struct file *fp;
	struct animation_state state;
	loff_t pos = 0;
	
	/* Prepare state data */
	state.mode = current_animation;
	state.speed = animation_speed;
	state.brightness = global_brightness;
	
	/* Copy current colors */
	for (int i = 0; i < ZONE_COUNT; i++) {
		state.colors[i] = original_colors[i].colors;
	}
	
	/* 
	 * Note: Directory /var/lib/omen-rgb-keyboard is created by install.sh
	 * We don't create it here to avoid kernel API compatibility issues
	 */
	
	/* Open file for writing */
	fp = filp_open(STATE_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(fp)) {
		pr_warn("Failed to save animation state: %ld\n", PTR_ERR(fp));
		return;
	}
	
	/* Write state to file */
	kernel_write(fp, &state, sizeof(state), &pos);
	
	filp_close(fp, NULL);
	
	pr_info("Animation state saved\n");
}

void load_animation_state(void)
{
	struct file *fp;
	struct animation_state state;
	loff_t pos = 0;
	ssize_t ret;
	
	/* Open file for reading */
	fp = filp_open(STATE_FILE_PATH, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		pr_info("No saved animation state found\n");
		return;
	}
	
	/* Read state from file */
	ret = kernel_read(fp, &state, sizeof(state), &pos);
	if (ret != sizeof(state)) {
		pr_warn("Failed to read animation state\n");
		filp_close(fp, NULL);
		return;
	}
	
	filp_close(fp, NULL);
	
	if (state.mode >= 0 && state.mode < ANIMATION_COUNT) {
		current_animation = state.mode;
	}
	if (state.speed >= ANIMATION_SPEED_MIN && state.speed <= ANIMATION_SPEED_MAX) {
		animation_speed = state.speed;
	}
	if (state.brightness >= 0 && state.brightness <= 100) {
		global_brightness = state.brightness;
	}
	
	/* Restore colors */
	for (int i = 0; i < ZONE_COUNT; i++) {
		original_colors[i].colors = state.colors[i];
	}
	
	pr_info("Animation state loaded: mode=%d, speed=%d, brightness=%d\n", 
		current_animation, animation_speed, global_brightness);
}

