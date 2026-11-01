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
	ssize_t written;
	int err;

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

	written = kernel_write(fp, &state, sizeof(state), &pos);
	err = filp_close(fp, NULL);
	if (written != sizeof(state)) {
		pr_warn("Failed to save animation state: write returned %zd of %zu bytes\n",
			written, sizeof(state));
		return;
	}
	if (err) {
		pr_warn("Failed to save animation state: close returned %d\n", err);
		return;
	}
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

struct omen_read_work {
	struct work_struct work;
	const char *path;
	void *buf;
	size_t len;
	ssize_t ret;
};

static void omen_read_file_work(struct work_struct *work)
{
	struct omen_read_work *rw = container_of(work, struct omen_read_work, work);
	struct file *fp;
	loff_t pos = 0;

	fp = filp_open(rw->path, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		rw->ret = PTR_ERR(fp);
		return;
	}
	rw->ret = kernel_read(fp, rw->buf, rw->len, &pos);
	filp_close(fp, NULL);
}

ssize_t omen_read_state_file(const char *path, void *buf, size_t len)
{
	struct omen_read_work rw = { .path = path, .buf = buf, .len = len };

	/*
	 * Read in kworker context (kernel creds), like the save path. Module
	 * init runs with the credentials of whoever loads the module; at boot
	 * that is systemd-modules-load, which SELinux does not allow to read
	 * /var/lib (the denial is dontaudit'ed, so nothing shows in the log).
	 */
	INIT_WORK_ONSTACK(&rw.work, omen_read_file_work);
	schedule_work(&rw.work);
	flush_work(&rw.work);
	destroy_work_on_stack(&rw.work);

	return rw.ret;
}

void load_animation_state(void)
{
	struct animation_state state;
	ssize_t ret;

	if (!anim_persist_inited) {
		INIT_WORK(&anim_persist_work, save_animation_state_work);
		anim_persist_inited = true;
	}

	ret = omen_read_state_file(STATE_FILE_PATH, &state, sizeof(state));
	if (ret == -ENOENT) {
		pr_info("No saved animation state found\n");
		return;
	}
	if (ret < 0) {
		pr_warn("Failed to read animation state from %s: %zd\n",
			STATE_FILE_PATH, ret);
		return;
	}
	if (ret != sizeof(state)) {
		pr_warn("Failed to read animation state: got %zd of %zu bytes\n",
			ret, sizeof(state));
		return;
	}

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
