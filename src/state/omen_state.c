// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - State Persistence
 *
 * State saving and loading functionality
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "omen_state.h"
#include "omen_zones.h"
#include "omen_animations.h"

static DEFINE_MUTEX(anim_persist_mutex);
static struct animation_state anim_persist_pending;
static bool anim_persist_pending_valid;
static struct work_struct anim_persist_work;
static bool anim_persist_inited;

static void save_animation_state_work(struct work_struct *work)
{
	struct animation_state state;
	struct file *fp;
	loff_t pos = 0;

	mutex_lock(&anim_persist_mutex);
	if (!anim_persist_pending_valid) {
		mutex_unlock(&anim_persist_mutex);
		return;
	}
	state = anim_persist_pending;
	anim_persist_pending_valid = false;
	mutex_unlock(&anim_persist_mutex);

	/*
	 * Runs in kworker context (kernel creds). Sysfs callers cannot write
	 * the root-owned state file directly (EACCES).
	 */
	fp = filp_open(STATE_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(fp)) {
		pr_warn("Failed to save animation state: %ld\n", PTR_ERR(fp));
		return;
	}

	kernel_write(fp, &state, sizeof(state), &pos);
	filp_close(fp, NULL);
	pr_info("Animation state saved\n");
}

void save_animation_state(void)
{
	if (!anim_persist_inited) {
		INIT_WORK(&anim_persist_work, save_animation_state_work);
		anim_persist_inited = true;
	}

	mutex_lock(&anim_persist_mutex);
	anim_persist_pending.mode = current_animation;
	anim_persist_pending.speed = animation_speed;
	anim_persist_pending.brightness = global_brightness;

	for (int i = 0; i < ZONE_COUNT; i++)
		anim_persist_pending.colors[i] = original_colors[i].colors;

	mutex_lock(&gradient_cfg_mutex);
	anim_persist_pending.gradient = gradient_cfg;
	mutex_unlock(&gradient_cfg_mutex);

	anim_persist_pending_valid = true;
	mutex_unlock(&anim_persist_mutex);

	schedule_work(&anim_persist_work);
}

void flush_animation_state(void)
{
	if (anim_persist_inited)
		flush_work(&anim_persist_work);
}

void load_animation_state(void)
{
	struct file *fp;
	struct animation_state state;
	loff_t pos = 0;
	ssize_t ret;

	if (!anim_persist_inited) {
		INIT_WORK(&anim_persist_work, save_animation_state_work);
		anim_persist_inited = true;
	}

	fp = filp_open(STATE_FILE_PATH, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		pr_info("No saved animation state found\n");
		return;
	}

	ret = kernel_read(fp, &state, sizeof(state), &pos);
	if (ret != sizeof(state)) {
		pr_warn("Failed to read animation state\n");
		filp_close(fp, NULL);
		return;
	}

	filp_close(fp, NULL);

	if (state.mode >= 0 && state.mode < ANIMATION_COUNT)
		current_animation = state.mode;
	if (state.speed >= ANIMATION_SPEED_MIN && state.speed <= ANIMATION_SPEED_MAX)
		animation_speed = state.speed;
	if (state.brightness >= 0 && state.brightness <= 100)
		global_brightness = state.brightness;

	for (int i = 0; i < ZONE_COUNT; i++)
		original_colors[i].colors = state.colors[i];

	if (state.gradient.group_count <= GRADIENT_MAX_GROUPS) {
		bool gradient_valid = true;

		for (int i = 0; i < state.gradient.group_count; i++) {
			if (state.gradient.groups[i].color_count > GRADIENT_MAX_COLORS) {
				gradient_valid = false;
				break;
			}
		}
		if (gradient_valid) {
			mutex_lock(&gradient_cfg_mutex);
			gradient_cfg = state.gradient;
			mutex_unlock(&gradient_cfg_mutex);
		} else {
			pr_warn("Saved gradient config has invalid color_count, skipping\n");
		}
	} else {
		pr_warn("Saved gradient config has invalid group_count %d, skipping\n",
			state.gradient.group_count);
	}

	pr_info("Animation state loaded: mode=%d, speed=%d, brightness=%d\n",
		current_animation, animation_speed, global_brightness);
}
