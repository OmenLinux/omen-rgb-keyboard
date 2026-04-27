// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Fan control via HP WMI
 *
 * RPM read, max fan, OMEN thermal profiles (WMI: silent / normal /
 * performance) each with a built-in fan curve when Victus WMI + fan table +
 * CONFIG_THERMAL are available; optional custom curve via fan_curve sysfs.
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_THERMAL)
#include <linux/thermal.h>
#endif

#include "omen_fan.h"
#include "omen_wmi.h"

enum omen_fan_iface {
	OMEN_FAN_IF_NONE,
	OMEN_FAN_IF_CLASSIC,
	OMEN_FAN_IF_VICTUS_S,
};

#define FAN_CPU			0
#define FAN_GPU			1
#define FAN_KEEPALIVE_JIFFIES	msecs_to_jiffies(85000)
#define FAN_CURVE_POLL_JIFFIES	msecs_to_jiffies(1500)
#define HP_FAN_SPEED_AUTOMATIC	0

#define HP_OMEN_EC_THERMAL_PROFILE_OFFSET	0x95
#define HP_VICTUS_S_EC_THERMAL_PROFILE_OFFSET	0x59

#define MAX_CURVE_POINTS	8

/* OMEN EC / WMI profile bytes (hp-wmi omen v0 / v1) */
#define OMEN_V0_DEFAULT		0x00
#define OMEN_V0_PERFORMANCE	0x01
#define OMEN_V0_COOL		0x02
#define OMEN_V1_DEFAULT		0x30
#define OMEN_V1_PERFORMANCE	0x31
#define OMEN_V1_COOL		0x50

struct victus_s_fan_table_header {
	u8 unknown;
	u8 num_entries;
} __packed;

struct victus_s_fan_table_entry {
	u8 cpu_rpm;
	u8 gpu_rpm;
	u8 unknown;
} __packed;

struct victus_s_fan_table {
	struct victus_s_fan_table_header header;
	struct victus_s_fan_table_entry entries[];
} __packed;

static enum omen_fan_iface fan_iface = OMEN_FAN_IF_NONE;
static struct platform_device *fan_pdev;
static DEFINE_MUTEX(fan_lock);

static int max_fan_state;
static bool max_fan_known;
static bool fan_max_keepalive_armed;
static struct delayed_work fan_max_keepalive;

static u8 fan_tbl_min, fan_tbl_max;
static int fan_gpu_delta;
static bool fan_tbl_valid;

static int curve_temps_c[MAX_CURVE_POINTS];
static int curve_pct[MAX_CURVE_POINTS];
static int curve_num_points;
static bool curve_enabled;
static struct delayed_work fan_curve_work;
#if IS_ENABLED(CONFIG_THERMAL)
static struct thermal_zone_device *curve_tz;
#endif
#define FAN_TZ_NAME_SZ	64
static char curve_tz_name[FAN_TZ_NAME_SZ];

static bool fan_curve_manual_off;

static int fan_victus_userdefine_trigger(void)
{
	u8 fc[4] = {};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GM_FAN_COUNT, HPWMI_GAMING, fc,
				   sizeof(u8), sizeof(fc));
	if (ret)
		return -EIO;
	return fc[0];
}

static int fan_load_victus_fan_table(void)
{
	u8 fan_data[128] = {};
	struct victus_s_fan_table *fan_table;
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GM_VICTUS_FAN_TABLE_GET, HPWMI_GAMING,
				   fan_data, 4, sizeof(fan_data));
	if (ret)
		return ret;

	fan_table = (struct victus_s_fan_table *)fan_data;
	if (fan_table->header.num_entries == 0)
		return -EINVAL;
	if (sizeof(struct victus_s_fan_table_header) +
	    sizeof(struct victus_s_fan_table_entry) * fan_table->header.num_entries >
	    sizeof(fan_data))
		return -EINVAL;

	fan_tbl_min = fan_table->entries[0].cpu_rpm;
	fan_tbl_max = fan_table->entries[fan_table->header.num_entries - 1].cpu_rpm;
	if (fan_tbl_min > fan_tbl_max) {
		u8 x = fan_tbl_min;

		fan_tbl_min = fan_tbl_max;
		fan_tbl_max = x;
	}
	fan_gpu_delta = (int)fan_table->entries[0].gpu_rpm -
			(int)fan_table->entries[0].cpu_rpm;
	fan_tbl_valid = true;
	return 0;
}

static int fan_rpm_classic(int fan_idx)
{
	char fan_data[4] = { fan_idx, 0, 0, 0 };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_GET, HPWMI_GAMING,
				   fan_data, sizeof(char), sizeof(fan_data));
	if (ret)
		return -EIO;

	return ((unsigned int)(u8)fan_data[2] << 8) | (u8)fan_data[3];
}

static int fan_rpm_victus(int fan_idx)
{
	u8 buf[128] = {};
	int ret;

	if (fan_idx < 0 || fan_idx >= (int)sizeof(buf))
		return -EINVAL;

	ret = hp_wmi_perform_query(HPWMI_GM_VICTUS_FAN_SPEED_GET, HPWMI_GAMING,
				   buf, sizeof(u8), sizeof(buf));
	if (ret)
		return -EIO;

	return buf[fan_idx] * 100;
}

static int fan_rpm_read(int channel)
{
	switch (fan_iface) {
	case OMEN_FAN_IF_CLASSIC:
		return fan_rpm_classic(channel);
	case OMEN_FAN_IF_VICTUS_S:
		return fan_rpm_victus(channel);
	default:
		return -ENODEV;
	}
}

static void fan_detect_iface(void)
{
	char fd[4] = { 0, 0, 0, 0 };
	u8 big[128] = {};
	int ret_classic, ret_victus;

	ret_victus = hp_wmi_perform_query(HPWMI_GM_VICTUS_FAN_SPEED_GET,
					  HPWMI_GAMING, big, sizeof(u8),
					  sizeof(big));
	if (!ret_victus) {
		fan_iface = OMEN_FAN_IF_VICTUS_S;
		return;
	}

	ret_classic = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_GET, HPWMI_GAMING,
					   fd, sizeof(char), sizeof(fd));
	if (!ret_classic) {
		fan_iface = OMEN_FAN_IF_CLASSIC;
		return;
	}

	fan_iface = OMEN_FAN_IF_NONE;
}

static int fan_wmi_performance_set(u8 mode_byte)
{
	char wbuf[2] = { (char)-1, (char)mode_byte };
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GM_SET_PERFORMANCE_MODE, HPWMI_GAMING,
				   wbuf, sizeof(wbuf), 0);
	return ret;
}

static int fan_thermal_profile_apply(const char *name)
{
	u8 v1, v0;
	int ret;

	if (sysfs_streq(name, "performance")) {
		v1 = OMEN_V1_PERFORMANCE;
		v0 = OMEN_V0_PERFORMANCE;
	} else if (sysfs_streq(name, "normal")) {
		v1 = OMEN_V1_DEFAULT;
		v0 = OMEN_V0_DEFAULT;
	} else if (sysfs_streq(name, "silent")) {
		v1 = OMEN_V1_COOL;
		v0 = OMEN_V0_COOL;
	} else {
		return -EINVAL;
	}

	ret = fan_wmi_performance_set(v1);
	if (ret == HPWMI_RET_INVALID_PARAMETERS)
		ret = fan_wmi_performance_set(v0);
	return ret;
}

static int fan_ec_profile_byte(u8 *out)
{
	int ret;

	ret = ec_read(HP_OMEN_EC_THERMAL_PROFILE_OFFSET, out);
	if (!ret)
		return 0;
	return ec_read(HP_VICTUS_S_EC_THERMAL_PROFILE_OFFSET, out);
}

static const char *fan_ec_byte_to_name(u8 b)
{
	switch (b) {
	case OMEN_V1_PERFORMANCE:
	case OMEN_V0_PERFORMANCE:
		return "performance";
	case OMEN_V1_DEFAULT:
	case OMEN_V0_DEFAULT:
		return "normal";
	case OMEN_V1_COOL:
	case OMEN_V0_COOL:
		return "silent";
	default:
		return "unknown";
	}
}

static u8 fan_percent_to_cpu_speed(unsigned int pct)
{
	if (!fan_tbl_valid)
		return 0;
	if (pct > 100)
		pct = 100;
	if (fan_tbl_max <= fan_tbl_min)
		return fan_tbl_min;
	return (u8)(fan_tbl_min +
		    (pct * (u32)(fan_tbl_max - fan_tbl_min)) / 100u);
}

static int fan_victus_wmi_speed_set(u8 cpu_spd)
{
	u8 fan_speed[2];
	int gpu_spd, ret;

	if (fan_iface != OMEN_FAN_IF_VICTUS_S)
		return -ENODEV;

	fan_speed[0] = cpu_spd;
	fan_speed[1] = cpu_spd;
	if (cpu_spd != HP_FAN_SPEED_AUTOMATIC) {
		if (!fan_tbl_valid)
			return -ENODEV;
		gpu_spd = (int)cpu_spd + fan_gpu_delta;
		fan_speed[1] = (u8)clamp_val(gpu_spd, 0, 255);
	}

	ret = fan_victus_userdefine_trigger();
	if (ret < 0)
		return ret;

	{
		int z = 0;

		ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
					   &z, sizeof(z), 0);
	}
	if (ret)
		pr_debug("fan max clear before manual set: %d\n", ret);

	ret = hp_wmi_perform_query(HPWMI_GM_VICTUS_FAN_SPEED_SET, HPWMI_GAMING,
				   fan_speed, sizeof(fan_speed), 0);
	return ret ? -EIO : 0;
}

/* Do not call while holding fan_lock (work may be waiting on that lock). */
static void fan_curve_disable_sync(void)
{
	curve_enabled = false;
	cancel_delayed_work_sync(&fan_curve_work);

	mutex_lock(&fan_lock);
	fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
	mutex_unlock(&fan_lock);
}

static int fan_temp_zone_bind_default(void)
{
#if IS_ENABLED(CONFIG_THERMAL)
	static const char * const try_names[] = {
		"x86_pkg_temp", "acpitz", "k10temp", "pch_cannonlake"
	};
	unsigned int i;
	struct thermal_zone_device *tz;

	if (curve_tz_name[0]) {
		tz = thermal_zone_get_zone_by_name(curve_tz_name);
		if (!IS_ERR_OR_NULL(tz)) {
			curve_tz = tz;
			return 0;
		}
	}
	for (i = 0; i < ARRAY_SIZE(try_names); i++) {
		tz = thermal_zone_get_zone_by_name(try_names[i]);
		if (!IS_ERR_OR_NULL(tz)) {
			curve_tz = tz;
			strscpy(curve_tz_name, try_names[i], sizeof(curve_tz_name));
			return 0;
		}
	}
	return -ENODEV;
#else
	return -ENODEV;
#endif
}

static int fan_temp_mc(long *temp_mc)
{
#if IS_ENABLED(CONFIG_THERMAL)
	struct thermal_zone_device *tz = curve_tz;
	int t, ret;

	if (!tz)
		return -ENODEV;
	ret = thermal_zone_get_temp(tz, &t);
	if (ret)
		return ret;
	*temp_mc = t;
	return 0;
#else
	return -ENODEV;
#endif
}

static int curve_percent_for_temp_c(int temp_c)
{
	int i, lo_t, hi_t, lo_p, hi_p;

	if (curve_num_points < 2)
		return -EINVAL;

	if (temp_c <= curve_temps_c[0])
		return curve_pct[0];
	if (temp_c >= curve_temps_c[curve_num_points - 1])
		return curve_pct[curve_num_points - 1];

	for (i = 0; i < curve_num_points - 1; i++) {
		lo_t = curve_temps_c[i];
		hi_t = curve_temps_c[i + 1];
		if (temp_c >= hi_t)
			continue;
		lo_p = curve_pct[i];
		hi_p = curve_pct[i + 1];
		if (hi_t <= lo_t)
			return lo_p;
		return lo_p + (int)(temp_c - lo_t) * (hi_p - lo_p) / (hi_t - lo_t);
	}
	return curve_pct[curve_num_points - 1];
}

static void fan_curve_sort_points(void)
{
	int i, j, tmp;

	for (i = 0; i < curve_num_points - 1; i++) {
		for (j = i + 1; j < curve_num_points; j++) {
			if (curve_temps_c[j] < curve_temps_c[i]) {
				tmp = curve_temps_c[i];
				curve_temps_c[i] = curve_temps_c[j];
				curve_temps_c[j] = tmp;
				tmp = curve_pct[i];
				curve_pct[i] = curve_pct[j];
				curve_pct[j] = tmp;
			}
		}
	}
}

/*
 * Default temp:% fan curves per preset (piecewise-linear vs thermal zone).
 * Silent stays low until high load; normal is balanced; performance ramps early.
 */
static void fan_preset_curve_install(const char *name)
{
	static const int silent_t[] = { 35, 50, 65, 80, 95 };
	static const int silent_p[] = { 12, 20, 34, 52, 72 };
	static const int normal_t[] = { 35, 52, 68, 82, 98 };
	static const int normal_p[] = { 22, 36, 55, 78, 95 };
	static const int performance_t[] = { 30, 48, 62, 76, 92 };
	static const int performance_p[] = { 35, 50, 68, 88, 100 };
	const int *t, *p;
	int i, n;

	if (sysfs_streq(name, "silent")) {
		t = silent_t;
		p = silent_p;
		n = ARRAY_SIZE(silent_t);
	} else if (sysfs_streq(name, "normal")) {
		t = normal_t;
		p = normal_p;
		n = ARRAY_SIZE(normal_t);
	} else if (sysfs_streq(name, "performance")) {
		t = performance_t;
		p = performance_p;
		n = ARRAY_SIZE(performance_t);
	} else {
		return;
	}

	if (n > MAX_CURVE_POINTS)
		n = MAX_CURVE_POINTS;
	for (i = 0; i < n; i++) {
		curve_temps_c[i] = t[i];
		curve_pct[i] = p[i];
	}
	curve_num_points = n;
	fan_curve_sort_points();
}

static void fan_curve_work_fn(struct work_struct *work)
{
	long temp_mc = 0;
	int temp_c, pct, ret, run_again;
	u8 spd;

	mutex_lock(&fan_lock);
	if (!curve_enabled) {
		mutex_unlock(&fan_lock);
		return;
	}
	if (max_fan_state) {
		curve_enabled = false;
		mutex_unlock(&fan_lock);
		return;
	}

	ret = fan_temp_mc(&temp_mc);
	if (ret)
		temp_c = 50;
	else
		temp_c = (int)(temp_mc / 1000);

	pct = curve_percent_for_temp_c(temp_c);
	if (pct < 0)
		pct = 50;
	spd = fan_percent_to_cpu_speed((unsigned int)pct);
	ret = fan_victus_wmi_speed_set(spd);
	if (ret)
		pr_debug("fan curve apply failed: %d\n", ret);

	mutex_unlock(&fan_lock);

	mutex_lock(&fan_lock);
	run_again = curve_enabled ? 1 : 0;
	mutex_unlock(&fan_lock);
	if (run_again)
		schedule_delayed_work(&fan_curve_work, FAN_CURVE_POLL_JIFFIES);
}

static void fan_max_keepalive_fn(struct work_struct *work)
{
	int en = 1;
	int ret;

	if (!fan_max_keepalive_armed || !max_fan_state)
		return;

	ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
				   &en, sizeof(en), 0);
	if (ret)
		pr_debug("fan max keepalive failed: %d\n", ret);

	if (fan_max_keepalive_armed && max_fan_state)
		schedule_delayed_work(&fan_max_keepalive, FAN_KEEPALIVE_JIFFIES);
}

static ssize_t cpu_fan_rpm_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int rpm;

	mutex_lock(&fan_lock);
	rpm = fan_rpm_read(FAN_CPU);
	mutex_unlock(&fan_lock);

	if (rpm < 0)
		return sysfs_emit(buf, "n/a\n");
	return sysfs_emit(buf, "%d\n", rpm);
}

static ssize_t gpu_fan_rpm_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int rpm;

	mutex_lock(&fan_lock);
	rpm = fan_rpm_read(FAN_GPU);
	mutex_unlock(&fan_lock);

	if (rpm < 0)
		return sysfs_emit(buf, "n/a\n");
	return sysfs_emit(buf, "%d\n", rpm);
}

static ssize_t max_fan_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int val = 0;
	int ret;

	mutex_lock(&fan_lock);
	ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_GET, HPWMI_GAMING,
				   &val, sizeof(val), sizeof(val));
	if (!ret) {
		int hw = val ? 1 : 0;

		if (max_fan_known && hw != max_fan_state) {
			int m = max_fan_state;

			mutex_unlock(&fan_lock);
			return sysfs_emit(buf, "%d\n", m);
		}
		max_fan_state = hw;
		max_fan_known = true;
		mutex_unlock(&fan_lock);
		return sysfs_emit(buf, "%d\n", max_fan_state);
	}

	if (max_fan_known) {
		int m = max_fan_state;

		mutex_unlock(&fan_lock);
		return sysfs_emit(buf, "%d\n", m);
	}
	mutex_unlock(&fan_lock);
	return sysfs_emit(buf, "0\n");
}

static ssize_t max_fan_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned long v;
	int enabled;
	int ret;

	if (kstrtoul(buf, 10, &v))
		return -EINVAL;
	if (v > 1)
		return -EINVAL;

	if (v)
		fan_curve_disable_sync();

	mutex_lock(&fan_lock);

	if (!v) {
		fan_max_keepalive_armed = false;
		cancel_delayed_work_sync(&fan_max_keepalive);
	}

	enabled = v ? 1 : 0;

	if (fan_iface == OMEN_FAN_IF_VICTUS_S && v)
		fan_victus_userdefine_trigger();

	ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
				   &enabled, sizeof(enabled), 0);
	if (ret) {
		mutex_unlock(&fan_lock);
		return -EIO;
	}

	max_fan_state = v ? 1 : 0;
	max_fan_known = true;

	if (v) {
		fan_max_keepalive_armed = true;
		schedule_delayed_work(&fan_max_keepalive, FAN_KEEPALIVE_JIFFIES);
	}
	mutex_unlock(&fan_lock);
	return count;
}

static ssize_t thermal_profile_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	u8 ec = 0;
	int ret;

	mutex_lock(&fan_lock);
	ret = fan_ec_profile_byte(&ec);
	mutex_unlock(&fan_lock);

	if (ret)
		return sysfs_emit(buf, "unknown (ec_read error %d)\n", ret);
	return sysfs_emit(buf, "%s\n", fan_ec_byte_to_name(ec));
}

static ssize_t thermal_profile_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char line[32];
	int ret;

	if (count >= sizeof(line))
		return -EINVAL;
	memcpy(line, buf, count);
	line[count] = '\0';
	strim(line);

	ret = fan_thermal_profile_apply(line);
	if (ret == -EINVAL)
		return -EINVAL;
	if (ret)
		return -EIO;

	mutex_lock(&fan_lock);
	fan_preset_curve_install(line);

	if (curve_enabled && !max_fan_state && fan_iface == OMEN_FAN_IF_VICTUS_S &&
	    fan_tbl_valid)
		schedule_delayed_work(&fan_curve_work, 1);
	mutex_unlock(&fan_lock);
	return count;
}

static ssize_t fan_curve_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int i, n = 0;

	mutex_lock(&fan_lock);
	for (i = 0; i < curve_num_points; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, "%d:%d%s",
			       curve_temps_c[i], curve_pct[i],
			       (i < curve_num_points - 1) ? " " : "");
	mutex_unlock(&fan_lock);

	if (!n)
		return sysfs_emit(buf, "(unset)\n");
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	return n;
}

static ssize_t fan_curve_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int t, p, npt = 0;
	const char *pbuf = buf;

	mutex_lock(&fan_lock);
	while (npt < MAX_CURVE_POINTS) {
		int c;

		while (*pbuf == ' ' || *pbuf == '\t' || *pbuf == '\n' || *pbuf == '\r')
			pbuf++;
		if (*pbuf == '\0')
			break;
		c = sscanf(pbuf, "%d:%d", &t, &p);
		if (c != 2) {
			mutex_unlock(&fan_lock);
			return -EINVAL;
		}
		if (t < 0 || t > 120 || p < 0 || p > 100) {
			mutex_unlock(&fan_lock);
			return -EINVAL;
		}
		curve_temps_c[npt] = t;
		curve_pct[npt] = p;
		npt++;
		while (*pbuf && *pbuf != ' ' && *pbuf != '\t' && *pbuf != '\n')
			pbuf++;
	}
	if (npt < 2) {
		mutex_unlock(&fan_lock);
		return -EINVAL;
	}

	curve_num_points = npt;
	fan_curve_sort_points();
	mutex_unlock(&fan_lock);
	return count;
}

static ssize_t fan_curve_enable_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int en;

	mutex_lock(&fan_lock);
	en = curve_enabled ? 1 : 0;
	mutex_unlock(&fan_lock);
	return sysfs_emit(buf, "%d\n", en);
}

static ssize_t fan_curve_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned long v;
	int ret;

	if (kstrtoul(buf, 10, &v))
		return -EINVAL;
	if (v > 1)
		return -EINVAL;

#if !IS_ENABLED(CONFIG_THERMAL)
	if (v)
		return -ENODEV;
#endif

	mutex_lock(&fan_lock);
	if (v && max_fan_state) {
		mutex_unlock(&fan_lock);
		return -EBUSY;
	}
	if (!v) {
		mutex_unlock(&fan_lock);
		fan_curve_disable_sync();
		mutex_lock(&fan_lock);
		fan_curve_manual_off = true;
		mutex_unlock(&fan_lock);
		return count;
	}

	if (curve_num_points < 2 || !fan_tbl_valid ||
	    fan_iface != OMEN_FAN_IF_VICTUS_S) {
		mutex_unlock(&fan_lock);
		return -EINVAL;
	}

#if IS_ENABLED(CONFIG_THERMAL)
	ret = fan_temp_zone_bind_default();
	if (ret) {
		mutex_unlock(&fan_lock);
		return ret;
	}
#else
	mutex_unlock(&fan_lock);
	return -ENODEV;
#endif

	curve_enabled = true;
	fan_curve_manual_off = false;
	schedule_delayed_work(&fan_curve_work, 1);
	mutex_unlock(&fan_lock);
	return count;
}

static ssize_t fan_temp_zone_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	mutex_lock(&fan_lock);
	if (!curve_tz_name[0]) {
		mutex_unlock(&fan_lock);
		return sysfs_emit(buf, "(auto)\n");
	}
	mutex_unlock(&fan_lock);
	return sysfs_emit(buf, "%s\n", curve_tz_name);
}

static ssize_t fan_temp_zone_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char line[FAN_TZ_NAME_SZ];
	size_t len = min(count, sizeof(line) - 1);

	memcpy(line, buf, len);
	line[len] = '\0';
	strim(line);

#if IS_ENABLED(CONFIG_THERMAL)
	{
		struct thermal_zone_device *tz;

		tz = thermal_zone_get_zone_by_name(line);
		if (IS_ERR_OR_NULL(tz))
			return -EINVAL;
		mutex_lock(&fan_lock);
		strscpy(curve_tz_name, line, sizeof(curve_tz_name));
		curve_tz = tz;
		mutex_unlock(&fan_lock);
	}
	return count;
#else
	return -ENODEV;
#endif
}

static DEVICE_ATTR_RO(cpu_fan_rpm);
static DEVICE_ATTR_RO(gpu_fan_rpm);
static DEVICE_ATTR_RW(max_fan);
static DEVICE_ATTR_RW(thermal_profile);
static DEVICE_ATTR_RW(fan_curve);
static DEVICE_ATTR_RW(fan_curve_enable);
static DEVICE_ATTR_RW(fan_temp_zone);

static struct attribute *fan_attrs[] = {
	&dev_attr_cpu_fan_rpm.attr,
	&dev_attr_gpu_fan_rpm.attr,
	&dev_attr_max_fan.attr,
	&dev_attr_thermal_profile.attr,
	&dev_attr_fan_curve.attr,
	&dev_attr_fan_curve_enable.attr,
	&dev_attr_fan_temp_zone.attr,
	NULL,
};

static struct attribute_group fan_attr_group = {
	.name = "fan",
	.attrs = fan_attrs,
};

int omen_fan_setup(struct platform_device *pdev)
{
	int ret;

	fan_detect_iface();
	if (fan_iface == OMEN_FAN_IF_VICTUS_S) {
		ret = fan_load_victus_fan_table();
		if (ret)
			pr_info("Victus fan table WMI (0x2f) unavailable (%d); manual curve disabled\n",
				ret);
		else
			pr_info("Victus fan table loaded (cpu u8 range %u..%u, gpu delta %+d)\n",
				fan_tbl_min, fan_tbl_max, fan_gpu_delta);
	}

	INIT_DELAYED_WORK(&fan_max_keepalive, fan_max_keepalive_fn);
	INIT_DELAYED_WORK(&fan_curve_work, fan_curve_work_fn);
	fan_pdev = pdev;

	ret = sysfs_create_group(&pdev->dev.kobj, &fan_attr_group);
	if (ret) {
		pr_warn("failed to create fan sysfs group: %d\n", ret);
		fan_pdev = NULL;
		return ret;
	}

	if (fan_iface == OMEN_FAN_IF_CLASSIC)
		pr_info("fan interface: classic WMI (RPM read, max fan)\n");
	else if (fan_iface == OMEN_FAN_IF_VICTUS_S)
		pr_info("fan interface: Victus-S WMI (RPM read, max fan, curve)\n");
	else
		pr_info("fan interface: RPM queries unsupported; max_fan may still work\n");

	return 0;
}

void omen_fan_cleanup(void)
{
	int z = 0;

	mutex_lock(&fan_lock);
	fan_max_keepalive_armed = false;
	mutex_unlock(&fan_lock);
	cancel_delayed_work_sync(&fan_max_keepalive);
	fan_curve_disable_sync();

	mutex_lock(&fan_lock);
	hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
			     &z, sizeof(z), 0);
	max_fan_state = 0;
	mutex_unlock(&fan_lock);

	if (fan_pdev) {
		sysfs_remove_group(&fan_pdev->dev.kobj, &fan_attr_group);
		fan_pdev = NULL;
	}

	fan_iface = OMEN_FAN_IF_NONE;
	max_fan_known = false;
	max_fan_state = 0;
	fan_tbl_valid = false;
	curve_num_points = 0;
	curve_tz_name[0] = '\0';
#if IS_ENABLED(CONFIG_THERMAL)
	curve_tz = NULL;
#endif
	fan_curve_manual_off = false;
}
