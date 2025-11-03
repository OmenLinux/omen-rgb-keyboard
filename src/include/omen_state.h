// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - State Persistence
 *
 * State saving and loading functionality
 *
 * Author: alessandromrc
 */

#ifndef OMEN_STATE_H
#define OMEN_STATE_H

#include <linux/types.h>
#include "omen_zones.h"
#include "omen_animations.h"

#define STATE_FILE_PATH "/var/lib/omen-rgb-keyboard/state"

struct animation_state {
	enum animation_mode mode;
	int speed;
	int brightness;
	struct color_platform colors[ZONE_COUNT];
};

/**
 * save_animation_state - Save current animation state to disk
 *
 * Note: Directory /var/lib/omen-rgb-keyboard is created by install.sh
 * We don't create it here to avoid kernel API compatibility issues
 */
void save_animation_state(void);

/**
 * load_animation_state - Load saved animation state from disk
 */
void load_animation_state(void);

#endif /* OMEN_STATE_H */

