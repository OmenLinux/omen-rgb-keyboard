// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN FourZone RGB Keyboard Driver
 *
 * A clean, lightweight Linux kernel driver for HP OMEN laptop RGB keyboard lighting.
 * Provides full control over 4-zone RGB lighting with brightness control.
 *
 * Author: alessandromrc
 * Based on reverse engineering of HP's Windows WMI interface
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/wmi.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/math.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/syscalls.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int simple_sin(int angle_degrees)
{
	angle_degrees = angle_degrees % 360;
	if (angle_degrees < 0) angle_degrees += 360;
	if (angle_degrees < 90) {
		return (angle_degrees * 100) / 90;
	} else if (angle_degrees < 180) {
		return ((180 - angle_degrees) * 100) / 90;
	} else if (angle_degrees < 270) {
		return -((angle_degrees - 180) * 100) / 90;
	} else {
		return -((360 - angle_degrees) * 100) / 90;
	}
}

MODULE_AUTHOR("alessandromrc");
MODULE_DESCRIPTION("HP OMEN FourZone RGB Keyboard Lighting Driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");

#define HPWMI_BIOS_GUID "5FB7F034-2C63-45e9-BE91-3D44E2C707E4"

enum hp_wmi_commandtype
{
	HPWMI_GET_PLATFORM_INFO = 1,
	HPWMI_FOURZONE_COLOR_GET = 2,
	HPWMI_FOURZONE_COLOR_SET = 3,
	HPWMI_STATUS = 4,
	HPWMI_SET_BRIGHTNESS = 5,
	HPWMI_SET_LIGHTBAR_COLORS = 11,
};

enum hp_wmi_command
{
	HPWMI_READ = 0x01,
	HPWMI_WRITE = 0x02,
	HPWMI_FOURZONE = 0x020009, /* Main lighting command */
	HPWMI_GAMING = 0x020008,	 /* Gaming command */
};

struct bios_args
{
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[128];
};

struct bios_return
{
	u32 sigpass;
	u32 return_code;
};

enum hp_return_value
{
	HPWMI_RET_WRONG_SIGNATURE = 0x02,
	HPWMI_RET_UNKNOWN_COMMAND = 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE = 0x04,
	HPWMI_RET_INVALID_PARAMETERS = 0x05,
};

static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
																void *buffer, int insize, int outsize)
{
	int mid;
	struct bios_return *bios_return;
	int actual_outsize;
	union acpi_object *obj;
	struct bios_args args = {
			.signature = 0x55434553,
			.command = command,
			.commandtype = query,
			.datasize = insize,
			.data = {0},
	};
	struct acpi_buffer input = {sizeof(struct bios_args), &args};
	struct acpi_buffer output = {ACPI_ALLOCATE_BUFFER, NULL};
	int ret = 0;

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0))
		return mid;
	if (WARN_ON(insize > sizeof(args.data)))
		return -EINVAL;
	memcpy(&args.data[0], buffer, insize);

	wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	obj = output.pointer;
	if (!obj)
		return -EINVAL;
	if (obj->type != ACPI_TYPE_BUFFER)
	{
		ret = -EINVAL;
		goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;
	if (ret)
	{
		if (ret != HPWMI_RET_UNKNOWN_COMMAND &&
				ret != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("query 0x%x returned error 0x%x\n", query, ret);
		goto out_free;
	}

	if (!outsize)
		goto out_free;

	actual_outsize = min(outsize, (int)(obj->buffer.length - sizeof(*bios_return)));
	memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
	memset(buffer + actual_outsize, 0, outsize - actual_outsize);

out_free:
	kfree(obj);
	return ret;
}

#define ZONE_COUNT 4

/* Animation system constants */
#define ANIMATION_TIMER_INTERVAL_MS 50
#define ANIMATION_SPEED_MIN 1
#define ANIMATION_SPEED_MAX 10
#define ANIMATION_SPEED_DEFAULT 1

enum animation_mode
{
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

struct color_platform
{
	u8 blue;
	u8 green;
	u8 red;
} __packed;

struct platform_zone
{
	u8 offset; /* position in returned buffer */
	struct device_attribute *attr;
	struct color_platform colors;
};

/* Global brightness control */
static int global_brightness = 100;											 /* Store brightness as percentage */
static struct platform_zone original_colors[ZONE_COUNT]; /* Store original colors */

/* Animation system */
static enum animation_mode current_animation = ANIMATION_STATIC;
static int animation_speed = ANIMATION_SPEED_DEFAULT;
static struct timer_list animation_timer;
static struct work_struct animation_work;
static unsigned long animation_start_time;
static bool animation_active = false;

/* State persistence */
#define STATE_FILE_PATH "/var/lib/omen-rgb-keyboard/state"
struct animation_state {
	enum animation_mode mode;
	int speed;
	int brightness;
	struct color_platform colors[ZONE_COUNT];
};

/* Function declarations */
static void start_animation(void);
static void stop_animation(void);
static void animation_work_func(struct work_struct *work);
static void animation_timer_callback(struct timer_list *t);
static void save_animation_state(void);
static void load_animation_state(void);

static struct device_attribute *zone_dev_attrs;
static struct attribute **zone_attrs;
static struct platform_zone *zone_data;

static struct attribute_group zone_attribute_group = {
		.name = "rgb_zones",
};

static int parse_rgb(const char *buf, struct platform_zone *zone)
{
	unsigned long rgb;
	int ret;
	union color_union
	{
		struct color_platform cp;
		int package;
	} repackager;

	ret = kstrtoul(buf, 16, &rgb);
	if (ret)
		return ret;
	if (rgb > 0xFFFFFF)
		return -EINVAL;

	repackager.package = rgb;
	pr_debug("hp-wmi: r:%d g:%d b:%d\n",
					 repackager.cp.red, repackager.cp.green, repackager.cp.blue);
	zone->colors = repackager.cp;
	return 0;
}

static struct platform_zone *match_zone(struct device_attribute *attr)
{
	u8 zone;
	for (zone = 0; zone < ZONE_COUNT; zone++)
	{
		if ((struct device_attribute *)zone_data[zone].attr == attr)
			return &zone_data[zone];
	}
	return NULL;
}

static int fourzone_update_led(struct platform_zone *zone, enum hp_wmi_command rw)
{
	u8 state[128];
	int ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_GET, HPWMI_FOURZONE,
																 &state, sizeof(state), sizeof(state));
	if (ret)
	{
		pr_warn("fourzone_color_get returned error 0x%x\n", ret);
		return ret <= 0 ? ret : -EINVAL;
	}

	if (rw == HPWMI_WRITE)
	{
		state[zone->offset + 0] = zone->colors.red;
		state[zone->offset + 1] = zone->colors.green;
		state[zone->offset + 2] = zone->colors.blue;

		ret = hp_wmi_perform_query(HPWMI_FOURZONE_COLOR_SET, HPWMI_FOURZONE,
															 &state, sizeof(state), sizeof(state));
		if (ret)
			pr_warn("fourzone_color_set returned error 0x%x\n", ret);
		return ret;
	}
	else
	{
		zone->colors.red = state[zone->offset + 0];
		zone->colors.green = state[zone->offset + 1];
		zone->colors.blue = state[zone->offset + 2];
	}
	return 0;
}

static ssize_t zone_show(struct device *dev, struct device_attribute *attr,
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

static ssize_t zone_set(struct device *dev, struct device_attribute *attr,
												const char *buf, size_t count)
{
	struct platform_zone *target_zone = match_zone(attr);
	int ret;
	if (target_zone == NULL)
	{
		pr_err("hp-wmi: invalid target zone\n");
		return 1;
	}
	ret = parse_rgb(buf, target_zone);
	if (ret)
		return ret;

	int zone_idx = target_zone - zone_data;
	original_colors[zone_idx].colors.red = target_zone->colors.red;
	original_colors[zone_idx].colors.green = target_zone->colors.green;
	original_colors[zone_idx].colors.blue = target_zone->colors.blue;

	stop_animation();
	current_animation = ANIMATION_STATIC;

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

/* Brightness control - scales all zone colors */

static ssize_t brightness_show(struct device *dev, struct device_attribute *attr,
															 char *buf)
{
	return sprintf(buf, "%d\n", global_brightness);
}

static ssize_t brightness_set(struct device *dev, struct device_attribute *attr,
															const char *buf, size_t count)
{
	unsigned long level;
	int ret;

	if (kstrtoul(buf, 10, &level))
		return -EINVAL;
	if (level > 100)
		level = 100;

	global_brightness = level;

	for (int zone = 0; zone < ZONE_COUNT; zone++)
	{
		/* Read current colors from hardware */
		ret = fourzone_update_led(&zone_data[zone], HPWMI_READ);
		if (ret)
			return ret;

		/* Store the original colors */
		original_colors[zone].colors.red = zone_data[zone].colors.red;
		original_colors[zone].colors.green = zone_data[zone].colors.green;
		original_colors[zone].colors.blue = zone_data[zone].colors.blue;

		/* Scale the colors by brightness */
		zone_data[zone].colors.red = (zone_data[zone].colors.red * level) / 100;
		zone_data[zone].colors.green = (zone_data[zone].colors.green * level) / 100;
		zone_data[zone].colors.blue = (zone_data[zone].colors.blue * level) / 100;

		ret = fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
		if (ret)
			return ret;
	}

	/* Save state */
	save_animation_state();

	return count;
}

static DEVICE_ATTR(brightness, 0644, brightness_show, brightness_set);

/* Animation helper functions */
static void apply_brightness_to_color(struct color_platform *color)
{
	color->red = (color->red * global_brightness) / 100;
	color->green = (color->green * global_brightness) / 100;
	color->blue = (color->blue * global_brightness) / 100;
}

static void hsv_to_rgb(int h, int s, int v, struct color_platform *rgb)
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

static void update_all_zones_with_colors(struct color_platform colors[ZONE_COUNT])
{
	for (int zone = 0; zone < ZONE_COUNT; zone++) {
		zone_data[zone].colors = colors[zone];
		apply_brightness_to_color(&zone_data[zone].colors);
		fourzone_update_led(&zone_data[zone], HPWMI_WRITE);
	}
}

/* Animation implementations */
static void animation_breathing(void)
{
	unsigned long elapsed = jiffies - animation_start_time;
	unsigned long cycle_time = msecs_to_jiffies(2000 / animation_speed); /* 2 second cycle */
	unsigned long cycle_pos = elapsed % cycle_time;
	
	int angle = (360 * cycle_pos) / cycle_time;
	int intensity = 50 + (50 * simple_sin(angle)) / 100;
	
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
		int intensity = 30 + (70 * (100 + simple_sin(angle)) / 200);
		
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
	int intensity = 20 + (80 * (100 + simple_sin(angle)) / 200);
	
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
		int intensity = 30 + (70 * (100 + simple_sin((360 * wave_pos) / cycle_time)) / 200);
		
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

/* State persistence functions */
static void save_animation_state(void)
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
	
	{
		struct dentry *dentry;
		struct path path;
		int ret = kern_path("/var/lib/omen-rgb-keyboard", LOOKUP_FOLLOW, &path);
		if (ret) {
			dentry = kern_path_create(AT_FDCWD, "/var/lib/omen-rgb-keyboard", &path, LOOKUP_DIRECTORY);
			if (!IS_ERR(dentry)) {
				struct mnt_idmap *idmap = mnt_idmap(path.mnt);
				vfs_mkdir(idmap, d_inode(path.dentry), dentry, 0755);
				done_path_create(&path, dentry);
			}
		} else {
			path_put(&path);
		}
	}
	
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

static void load_animation_state(void)
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

static void start_animation(void)
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

static void stop_animation(void)
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

static ssize_t all_show(struct device *dev, struct device_attribute *attr,
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

static ssize_t all_set(struct device *dev, struct device_attribute *attr,
											 const char *buf, size_t count)
{
	struct platform_zone temp;
	int ret;
	u8 z;

	ret = parse_rgb(buf, &temp);
	if (ret)
		return ret;

	stop_animation();
	current_animation = ANIMATION_STATIC;

	for (z = 0; z < ZONE_COUNT; z++)
	{
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

/* Animation control sysfs attributes */
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
	
	stop_animation();
	
	current_animation = new_mode;
	
	if (new_mode != ANIMATION_STATIC) {
		start_animation();
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
		stop_animation();
		start_animation();
	}
	
	/* Save state */
	save_animation_state();
	
	return count;
}

static DEVICE_ATTR(animation_mode, 0644, animation_mode_show, animation_mode_set);
static DEVICE_ATTR(animation_speed, 0644, animation_speed_show, animation_speed_set);

static int fourzone_setup(struct platform_device *dev)
{
	u8 zone;
	char buffer[10];
	char *name;

	INIT_WORK(&animation_work, animation_work_func);
	
	/* Load saved state */
	load_animation_state();

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

	for (u8 zone = 0; zone < ZONE_COUNT; zone++)
	{
		zone_data[zone].offset = 25 + (zone * 3);
		int ret = fourzone_update_led(&zone_data[zone], HPWMI_READ);
		if (ret)
			return ret;

		/* Store original colors */
		original_colors[zone].colors.red = zone_data[zone].colors.red;
		original_colors[zone].colors.green = zone_data[zone].colors.green;
		original_colors[zone].colors.blue = zone_data[zone].colors.blue;
	}

	for (zone = 0; zone < ZONE_COUNT; zone++)
	{
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

	zone_attrs[ZONE_COUNT + 1] = &dev_attr_brightness.attr;
	zone_attrs[ZONE_COUNT + 2] = &dev_attr_animation_mode.attr;
	zone_attrs[ZONE_COUNT + 3] = &dev_attr_animation_speed.attr;
	zone_attrs[ZONE_COUNT + 4] = NULL; /* NULL terminate the array */

	zone_attribute_group.attrs = zone_attrs;
	
	if (current_animation != ANIMATION_STATIC) {
		start_animation();
	}
	
	return sysfs_create_group(&dev->dev.kobj, &zone_attribute_group);
}

static struct platform_device *hp_wmi_platform_dev;

static int __init hp_wmi_bios_setup(struct platform_device *device)
{
	return fourzone_setup(device);
}

static struct platform_driver hp_wmi_driver = {
		.driver = {
				.name = "omen-rgb-keyboard",
		},
		.remove = NULL,
};

static int __init hp_wmi_init(void)
{
	int bios_capable = wmi_has_guid(HPWMI_BIOS_GUID);
	int err;
	if (!bios_capable)
	{
		pr_err("omen-rgb-keyboard: HP WMI BIOS GUID %s not found, driver not loaded\n", HPWMI_BIOS_GUID);
		return -ENODEV;
	}

	hp_wmi_platform_dev = platform_device_register_simple("omen-rgb-keyboard", -1, NULL, 0);
	if (IS_ERR(hp_wmi_platform_dev))
	{
		pr_err("omen-rgb-keyboard: failed to register platform device\n");
		return PTR_ERR(hp_wmi_platform_dev);
	}

	err = platform_driver_probe(&hp_wmi_driver, hp_wmi_bios_setup);
	if (err)
	{
		pr_err("omen-rgb-keyboard: platform_driver_probe failed with %d\n", err);
		platform_device_unregister(hp_wmi_platform_dev);
		return err;
	}
	return 0;
}
module_init(hp_wmi_init);

static void __exit hp_wmi_exit(void)
{
	stop_animation();
	
	/* Cancel any pending work */
	cancel_work_sync(&animation_work);
	
	if (hp_wmi_platform_dev)
	{
		platform_device_unregister(hp_wmi_platform_dev);
		platform_driver_unregister(&hp_wmi_driver);
	}
}
module_exit(hp_wmi_exit);
