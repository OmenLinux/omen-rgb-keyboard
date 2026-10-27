// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Fan control via HP WMI
 *
 * Default: EC automatic fans. Built-in silent/normal/performance reference
 * curves are shown in fan_curve; writing setpoints overrides the EC until the
 * user restores automatic. Custom points persist across reboot. Hysteresis
 * avoids fan chatter; under Performance, demand >= 90% can auto-engage max
 * fan (release at <= 80%). max_fan is always available. PPD drives the same
 * silent/normal/performance thermal modes via platform_profile.
 *
 * Silent and normal share the DEFAULT thermal WMI byte. PPAB stays enabled on
 * silent: clearing it makes many Omen ECs leave zero-RPM and hold a low spin
 * (power-saver louder than balanced). Omen boards (e.g. 8BB3) need v1 thermal
 * bytes (0x30/0x31) even when the fan interface is Victus-S.
 *
 * On these boards EC-automatic fans often ignore thermal mode (0 RPM even at
 * ~80C). Unless the user saved a custom curve, profile switches apply the
 * built-in silent/normal/performance curves. Curve percent 0 means true
 * zero-RPM (WMI automatic), not the BIOS manual floor.
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_THERMAL)
#include <linux/thermal.h>
#endif

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
#include <linux/platform_profile.h>
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

#ifndef ACPI_AC_CLASS
#define ACPI_AC_CLASS "ac_adapter"
#endif

#define MAX_CURVE_POINTS	8
#define FAN_CURVE_HYST_C	3
#define FAN_AUTO_MAX_ON_PCT	90
#define FAN_AUTO_MAX_OFF_PCT	80

#define FAN_STATE_PATH		"/var/lib/omen-rgb-keyboard/fan_state"
#define FAN_STATE_MAGIC		0x4e41464f /* 'OFAN' little-endian */
#define FAN_STATE_VERSION	1

/* OMEN / Victus EC / WMI profile bytes (mainline hp-wmi) */
#define OMEN_V0_DEFAULT		0x00
#define OMEN_V0_PERFORMANCE	0x01
#define OMEN_V0_COOL		0x02
#define VICTUS_QUIET		0x03
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

struct omen_fan_persist {
	u32 magic;
	u32 version;
	u8 curve_user_saved;
	u8 curve_enabled;
	u8 max_fan_manual;
	u8 num_points;
	s32 temps[MAX_CURVE_POINTS];
	s32 pcts[MAX_CURVE_POINTS];
	char thermal_profile[16];
} __packed;

static enum omen_fan_iface fan_iface = OMEN_FAN_IF_NONE;
static struct platform_device *fan_pdev;
static DEFINE_MUTEX(fan_lock);
/* Serializes snapshot+write so a stale save cannot clobber a newer one. */
static DEFINE_MUTEX(fan_persist_mutex);

static int max_fan_state;
static bool max_fan_known;
static bool max_fan_manual;
static bool max_fan_auto;
static bool fan_max_keepalive_armed;
static struct delayed_work fan_max_keepalive;

static u8 fan_tbl_min, fan_tbl_max;
static int fan_gpu_delta;
static bool fan_tbl_valid;

static int curve_temps_c[MAX_CURVE_POINTS];
static int curve_pct[MAX_CURVE_POINTS];
static int curve_num_points;
static bool curve_enabled;
static bool curve_user_saved;
static int curve_last_pct = -1;
static u8 curve_last_spd;
static bool curve_last_spd_valid;
static struct delayed_work fan_curve_work;
#if IS_ENABLED(CONFIG_THERMAL)
static struct thermal_zone_device *curve_tz;
#endif
#define FAN_TZ_NAME_SZ	64
static char curve_tz_name[FAN_TZ_NAME_SZ];

#define THERMAL_NAME_SZ 16
static char thermal_profile_name[THERMAL_NAME_SZ] = "normal";

static u8 power_pl1 = HP_POWER_LIMIT_DEFAULT;
static u8 power_pl2 = HP_POWER_LIMIT_DEFAULT;
static bool power_limits_known;
static bool power_limits_supported = true;
static bool gpu_modes_supported = true;

static struct notifier_block power_source_nb;
static bool power_source_nb_registered;
static bool fan_persist_ready;
static struct omen_fan_persist fan_persist_pending;
static bool fan_persist_pending_valid;
static struct work_struct fan_persist_work;

static void fan_preset_curve_install(const char *name);
static int fan_victus_wmi_speed_set(u8 cpu_spd);
static int fan_temp_zone_bind_default(void);
static void fan_curve_restore_ec(bool persist);
static void fan_persist_save(void);
static void fan_max_apply(bool enable, bool manual);
static void fan_curve_schedule_locked(void);
static bool fan_in_performance(void);

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
static enum platform_profile_option active_platform_profile =
	PLATFORM_PROFILE_BALANCED;
static struct device *platform_profile_dev;
static bool platform_profile_registered;
#endif

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

static int omen_set_cpu_pl1_pl2(u8 pl1, u8 pl2)
{
	struct omen_power_limits power_limits;
	int ret;

	if (pl1 == HP_POWER_LIMIT_NO_CHANGE || pl2 == HP_POWER_LIMIT_NO_CHANGE)
		return -EINVAL;
	if (pl2 < pl1)
		return -EINVAL;

	power_limits.pl1 = pl1;
	power_limits.pl2 = pl2;
	power_limits.pl4 = HP_POWER_LIMIT_NO_CHANGE;
	power_limits.cpu_gpu_concurrent_limit = HP_POWER_LIMIT_NO_CHANGE;

	ret = hp_wmi_perform_query(HPWMI_GM_SET_POWER_LIMITS, HPWMI_GAMING,
				   &power_limits, sizeof(power_limits), 0);
	if (ret) {
		if (ret == HPWMI_RET_UNKNOWN_CMDTYPE ||
		    ret == HPWMI_RET_UNKNOWN_COMMAND)
			power_limits_supported = false;
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int omen_gpu_thermal_get(bool *ctgp_enable, bool *ppab_enable,
				u8 *dstate, u8 *gpu_slowdown_temp)
{
	struct omen_gpu_power_modes modes = {};
	int ret;

	ret = hp_wmi_perform_query(HPWMI_GM_GET_GPU_THERMAL_MODES, HPWMI_GAMING,
				   &modes, sizeof(modes), sizeof(modes));
	if (ret) {
		if (ret == HPWMI_RET_UNKNOWN_CMDTYPE ||
		    ret == HPWMI_RET_UNKNOWN_COMMAND)
			gpu_modes_supported = false;
		return ret < 0 ? ret : -EIO;
	}

	*ctgp_enable = modes.ctgp_enable ? true : false;
	*ppab_enable = modes.ppab_enable ? true : false;
	*dstate = modes.dstate;
	*gpu_slowdown_temp = modes.gpu_slowdown_temp;
	return 0;
}

static int omen_gpu_thermal_set(bool ctgp_enable, bool ppab_enable, u8 dstate)
{
	struct omen_gpu_power_modes modes;
	bool cur_ctgp, cur_ppab;
	u8 cur_dstate, cur_slowdown;
	int ret;

	if (!gpu_modes_supported)
		return -ENODEV;

	ret = omen_gpu_thermal_get(&cur_ctgp, &cur_ppab, &cur_dstate,
				   &cur_slowdown);
	if (ret) {
		pr_debug("GPU modes not updated, unable to get slowdown temp\n");
		return ret;
	}

	modes.ctgp_enable = ctgp_enable ? 0x01 : 0x00;
	modes.ppab_enable = ppab_enable ? 0x01 : 0x00;
	modes.dstate = dstate;
	modes.gpu_slowdown_temp = cur_slowdown;

	ret = hp_wmi_perform_query(HPWMI_GM_SET_GPU_THERMAL_MODES, HPWMI_GAMING,
				   &modes, sizeof(modes), 0);
	if (ret) {
		if (ret == HPWMI_RET_UNKNOWN_CMDTYPE ||
		    ret == HPWMI_RET_UNKNOWN_COMMAND)
			gpu_modes_supported = false;
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int omen_gpu_modes_for_profile(const char *name)
{
	bool ctgp, ppab;

	if (sysfs_streq(name, "performance")) {
		ctgp = true;
		ppab = true;
	} else if (sysfs_streq(name, "normal")) {
		ctgp = false;
		ppab = true;
	} else if (sysfs_streq(name, "silent")) {
		/*
		 * Keep PPAB on. PPAB-off disables zero-RPM on boards like
		 * Omen Transcend 16 (8BB3), so power-saver becomes louder
		 * than balanced. Silent then matches balanced for GPU boost
		 * flags; the DEFAULT thermal byte is already shared.
		 */
		ctgp = false;
		ppab = true;
	} else {
		return -EINVAL;
	}

	return omen_gpu_thermal_set(ctgp, ppab, 1);
}

/*
 * Mainline hp-wmi: several Omen boards use Victus-S fan WMI but Omen v1
 * thermal profile bytes (0x30/0x31). Prefer those when DMI says Omen.
 */
static bool fan_thermal_use_omen_v1(void)
{
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);
	const char *product = dmi_get_system_info(DMI_PRODUCT_NAME);

	if (board && !strcmp(board, "8BB3"))
		return true;
	if (product && strstr(product, "OMEN"))
		return true;
	return false;
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

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
static const char *omen_pp_to_name(enum platform_profile_option profile)
{
	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		return "performance";
	case PLATFORM_PROFILE_BALANCED:
		return "normal";
	case PLATFORM_PROFILE_LOW_POWER:
		return "silent";
	default:
		return NULL;
	}
}

static enum platform_profile_option omen_name_to_pp(const char *name)
{
	if (sysfs_streq(name, "performance"))
		return PLATFORM_PROFILE_PERFORMANCE;
	if (sysfs_streq(name, "normal"))
		return PLATFORM_PROFILE_BALANCED;
	if (sysfs_streq(name, "silent"))
		return PLATFORM_PROFILE_LOW_POWER;
	return PLATFORM_PROFILE_LAST;
}
#endif

static bool fan_in_performance(void)
{
#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	if (active_platform_profile == PLATFORM_PROFILE_PERFORMANCE)
		return true;
#endif
	return sysfs_streq(thermal_profile_name, "performance");
}

static bool fan_curve_can_override(void)
{
	return fan_iface == OMEN_FAN_IF_VICTUS_S && fan_tbl_valid &&
	       curve_num_points >= 2;
}

static bool fan_worker_needed_locked(void)
{
	if (max_fan_manual)
		return false;
	if (curve_enabled && fan_curve_can_override())
		return true;
	if (fan_in_performance())
		return true;
	if (max_fan_auto)
		return true;
	return false;
}

static void fan_curve_schedule_locked(void)
{
	if (fan_worker_needed_locked())
		schedule_delayed_work(&fan_curve_work, 1);
}

/*
 * After a thermal preset change: refresh the reference table when the user
 * has no saved custom points, and drive that preset via WMI (EC-automatic
 * alone does not follow thermal mode on several Omen boards).
 *
 * Only re-send fan-speed automatic when dropping an override — not on every
 * profile switch (that can fight the EC curve).
 */
static void fan_curve_after_profile(const char *name)
{
	bool keep_override;
	bool had_override;
	bool release_to_ec;

	mutex_lock(&fan_lock);
	had_override = curve_enabled;

	if (!curve_user_saved) {
		fan_preset_curve_install(name);
		curve_enabled = true;
	}

	keep_override = curve_enabled && !max_fan_manual &&
			fan_curve_can_override();
#if IS_ENABLED(CONFIG_THERMAL)
	if (keep_override && fan_temp_zone_bind_default()) {
		pr_debug("thermal zone unavailable; keeping EC automatic\n");
		keep_override = false;
		curve_enabled = false;
	}
#else
	if (keep_override) {
		keep_override = false;
		curve_enabled = false;
	}
#endif

	release_to_ec = had_override && !keep_override && !max_fan_state &&
			fan_iface == OMEN_FAN_IF_VICTUS_S;

	if (!max_fan_manual && !fan_in_performance() && !max_fan_auto)
		curve_last_pct = -1;

	if (keep_override) {
		curve_last_pct = -1;
		curve_last_spd_valid = false;
	}
	mutex_unlock(&fan_lock);

	if (release_to_ec) {
		cancel_delayed_work_sync(&fan_curve_work);
		mutex_lock(&fan_lock);
		if (!max_fan_state && fan_iface == OMEN_FAN_IF_VICTUS_S)
			fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
		fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
	} else {
		mutex_lock(&fan_lock);
		fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
	}

	fan_persist_save();
}

static int fan_thermal_profile_apply(const char *name, bool from_pp)
{
	u8 primary, fallback;
	int ret;
#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	enum platform_profile_option pp;
#endif

	if (sysfs_streq(name, "performance")) {
		if (fan_thermal_use_omen_v1() ||
		    fan_iface != OMEN_FAN_IF_VICTUS_S) {
			primary = OMEN_V1_PERFORMANCE;
			fallback = OMEN_V0_PERFORMANCE;
		} else {
			primary = OMEN_V0_PERFORMANCE;
			fallback = OMEN_V1_PERFORMANCE;
		}
	} else if (sysfs_streq(name, "normal") || sysfs_streq(name, "silent")) {
		if (fan_thermal_use_omen_v1() ||
		    fan_iface != OMEN_FAN_IF_VICTUS_S) {
			primary = OMEN_V1_DEFAULT;
			fallback = OMEN_V0_DEFAULT;
		} else {
			primary = OMEN_V0_DEFAULT;
			fallback = OMEN_V1_DEFAULT;
		}
	} else {
		return -EINVAL;
	}

	fan_victus_userdefine_trigger();

	ret = fan_wmi_performance_set(primary);
	if (ret == HPWMI_RET_INVALID_PARAMETERS)
		ret = fan_wmi_performance_set(fallback);
	if (ret)
		return ret;

	ret = omen_gpu_modes_for_profile(name);
	if (ret && ret != -ENODEV)
		pr_debug("GPU thermal modes for %s: %d\n", name, ret);

	if (power_limits_supported) {
		ret = omen_set_cpu_pl1_pl2(HP_POWER_LIMIT_DEFAULT,
					  HP_POWER_LIMIT_DEFAULT);
		if (ret && ret != -ENODEV)
			pr_debug("power limit actualize after %s: %d\n",
				 name, ret);
		else if (!ret) {
			mutex_lock(&fan_lock);
			power_pl1 = HP_POWER_LIMIT_DEFAULT;
			power_pl2 = HP_POWER_LIMIT_DEFAULT;
			power_limits_known = true;
			mutex_unlock(&fan_lock);
		}
	}

	mutex_lock(&fan_lock);
	strscpy(thermal_profile_name, name, sizeof(thermal_profile_name));
	mutex_unlock(&fan_lock);

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	pp = omen_name_to_pp(name);
	if (pp != PLATFORM_PROFILE_LAST)
		active_platform_profile = pp;

	if (!from_pp && platform_profile_registered && platform_profile_dev)
		platform_profile_notify(platform_profile_dev);
#endif

	fan_curve_after_profile(name);
	return 0;
}

static bool fan_ec_byte_known(u8 b)
{
	switch (b) {
	case OMEN_V1_PERFORMANCE:
	case OMEN_V0_PERFORMANCE:
	case OMEN_V1_DEFAULT:
	case OMEN_V0_DEFAULT:
	case OMEN_V1_COOL:
	case OMEN_V0_COOL:
	case VICTUS_QUIET:
		return true;
	default:
		return false;
	}
}

static int fan_ec_profile_byte(u8 *out)
{
	u8 a = 0, b = 0;
	int ret_a, ret_b;
	const u8 first = (fan_iface == OMEN_FAN_IF_VICTUS_S) ?
			 HP_VICTUS_S_EC_THERMAL_PROFILE_OFFSET :
			 HP_OMEN_EC_THERMAL_PROFILE_OFFSET;
	const u8 second = (first == HP_VICTUS_S_EC_THERMAL_PROFILE_OFFSET) ?
			  HP_OMEN_EC_THERMAL_PROFILE_OFFSET :
			  HP_VICTUS_S_EC_THERMAL_PROFILE_OFFSET;

	ret_a = ec_read(first, &a);
	ret_b = ec_read(second, &b);

	if (!ret_a && fan_ec_byte_known(a)) {
		*out = a;
		return 0;
	}
	if (!ret_b && fan_ec_byte_known(b)) {
		*out = b;
		return 0;
	}
	if (!ret_a) {
		*out = a;
		return 0;
	}
	if (!ret_b) {
		*out = b;
		return 0;
	}
	return ret_a ? ret_a : ret_b;
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
	case VICTUS_QUIET:
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

/*
 * Return control to the EC without discarding saved custom setpoints.
 * Do not call while holding fan_lock (cancels work that may take the lock).
 */
static void fan_curve_restore_ec(bool persist)
{
	curve_enabled = false;
	curve_last_pct = -1;
	curve_last_spd_valid = false;
	cancel_delayed_work_sync(&fan_curve_work);

	mutex_lock(&fan_lock);
	if (!max_fan_state && fan_iface == OMEN_FAN_IF_VICTUS_S)
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
	if (!curve_user_saved)
		fan_preset_curve_install(thermal_profile_name);
	fan_curve_schedule_locked();
	mutex_unlock(&fan_lock);

	if (persist)
		fan_persist_save();
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

/*
 * Schmitt-style band: ramp up follows the curve immediately; ramp down only
 * when the curve at (temp + HYST) is also below the held percent.
 */
static int curve_percent_with_hysteresis(int temp_c)
{
	int pct_now, pct_cool;

	pct_now = curve_percent_for_temp_c(temp_c);
	if (pct_now < 0)
		return pct_now;

	if (curve_last_pct < 0) {
		curve_last_pct = pct_now;
		return pct_now;
	}

	if (pct_now > curve_last_pct) {
		curve_last_pct = pct_now;
		return curve_last_pct;
	}

	pct_cool = curve_percent_for_temp_c(temp_c + FAN_CURVE_HYST_C);
	if (pct_cool < 0)
		pct_cool = pct_now;
	if (pct_cool < curve_last_pct)
		curve_last_pct = pct_cool;

	return curve_last_pct;
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

static void fan_preset_curve_install(const char *name)
{
	/* 0% => WMI automatic (true zero-RPM). Higher presets spin earlier. */
	static const int silent_t[] = { 50, 60, 70, 85, 95 };
	static const int silent_p[] = { 0, 20, 35, 50, 65 };
	static const int normal_t[] = { 45, 55, 68, 82, 98 };
	static const int normal_p[] = { 0, 28, 55, 78, 95 };
	static const int performance_t[] = { 40, 50, 62, 76, 92 };
	static const int performance_p[] = { 0, 45, 68, 88, 100 };
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

static void fan_max_keepalive_arm_locked(void)
{
	fan_max_keepalive_armed = true;
	schedule_delayed_work(&fan_max_keepalive, FAN_KEEPALIVE_JIFFIES);
}

static void fan_max_keepalive_disarm(void)
{
	fan_max_keepalive_armed = false;
	cancel_delayed_work_sync(&fan_max_keepalive);
}

/*
 * Engage or clear max fan. manual=true is a sticky user latch; manual=false
 * is the Performance auto path (must not clear a manual latch).
 * Caller must not hold fan_lock across the keepalive cancel.
 */
static void fan_max_apply(bool enable, bool manual)
{
	int enabled;
	int ret;

	if (enable) {
		if (manual) {
			max_fan_manual = true;
			max_fan_auto = false;
		} else {
			if (max_fan_manual)
				return;
			max_fan_auto = true;
		}

		mutex_lock(&fan_lock);
		if (fan_iface == OMEN_FAN_IF_VICTUS_S)
			fan_victus_userdefine_trigger();
		enabled = 1;
		ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET,
					   HPWMI_GAMING, &enabled,
					   sizeof(enabled), 0);
		if (!ret) {
			max_fan_state = 1;
			max_fan_known = true;
			curve_last_spd_valid = false;
			fan_max_keepalive_arm_locked();
		} else if (manual) {
			max_fan_manual = false;
		} else {
			max_fan_auto = false;
		}
		mutex_unlock(&fan_lock);
		return;
	}

	/* disable */
	if (manual) {
		max_fan_manual = false;
		max_fan_auto = false;
	} else {
		if (max_fan_manual)
			return;
		max_fan_auto = false;
	}

	fan_max_keepalive_disarm();

	mutex_lock(&fan_lock);
	enabled = 0;
	ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
				   &enabled, sizeof(enabled), 0);
	if (!ret) {
		max_fan_state = 0;
		max_fan_known = true;
	}

	if (curve_enabled && fan_curve_can_override()) {
#if IS_ENABLED(CONFIG_THERMAL)
		if (!fan_temp_zone_bind_default()) {
			curve_last_pct = -1;
			fan_curve_schedule_locked();
		}
#endif
	} else if (fan_iface == OMEN_FAN_IF_VICTUS_S) {
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
		fan_curve_schedule_locked();
	}
	mutex_unlock(&fan_lock);
}

static void fan_auto_max_update_locked(int demand_pct)
{
	bool want_on = false;

	if (max_fan_manual)
		return;

	if (fan_in_performance() && demand_pct >= FAN_AUTO_MAX_ON_PCT)
		want_on = true;
	else if (max_fan_auto &&
		 (!fan_in_performance() || demand_pct <= FAN_AUTO_MAX_OFF_PCT))
		want_on = false;
	else if (max_fan_auto)
		want_on = true;
	else
		return;

	if (want_on && !max_fan_auto) {
		mutex_unlock(&fan_lock);
		fan_max_apply(true, false);
		mutex_lock(&fan_lock);
	} else if (!want_on && max_fan_auto) {
		mutex_unlock(&fan_lock);
		fan_max_apply(false, false);
		mutex_lock(&fan_lock);
	}
}

static void fan_curve_work_fn(struct work_struct *work)
{
	long temp_mc = 0;
	int temp_c, pct, ret, run_again;
	u8 spd;
	bool apply_override;

	mutex_lock(&fan_lock);

	if (max_fan_manual) {
		mutex_unlock(&fan_lock);
		return;
	}

	if (!fan_worker_needed_locked()) {
		mutex_unlock(&fan_lock);
		return;
	}

#if IS_ENABLED(CONFIG_THERMAL)
	if (!curve_tz)
		fan_temp_zone_bind_default();
#endif

	ret = fan_temp_mc(&temp_mc);
	if (ret)
		temp_c = 50;
	else
		temp_c = (int)(temp_mc / 1000);

	pct = curve_percent_with_hysteresis(temp_c);
	if (pct < 0)
		pct = 50;

	fan_auto_max_update_locked(pct);

	apply_override = curve_enabled && fan_curve_can_override() &&
			 !max_fan_state;

	if (apply_override) {
		/*
		 * Manual Victus speeds cannot reach true 0 RPM (BIOS floor).
		 * Percent 0 hands control back to EC automatic so zero-RPM
		 * works; any positive percent uses the fan table.
		 */
		if (pct <= 0)
			spd = HP_FAN_SPEED_AUTOMATIC;
		else
			spd = fan_percent_to_cpu_speed((unsigned int)pct);
		if (!curve_last_spd_valid || spd != curve_last_spd) {
			ret = fan_victus_wmi_speed_set(spd);
			if (ret)
				pr_debug("fan curve apply failed: %d\n", ret);
			else {
				curve_last_spd = spd;
				curve_last_spd_valid = true;
			}
		}
	}

	run_again = fan_worker_needed_locked() ? 1 : 0;
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

/*
 * Snapshot under locks, write from a kworker. Sysfs store paths run as the
 * calling user and cannot write root-owned /var/lib files (EACCES); kworkers
 * use kernel credentials and succeed.
 */
static void fan_persist_work_fn(struct work_struct *work)
{
	struct omen_fan_persist st;
	struct file *fp;
	loff_t pos = 0;

	mutex_lock(&fan_persist_mutex);
	if (!fan_persist_pending_valid) {
		mutex_unlock(&fan_persist_mutex);
		return;
	}
	st = fan_persist_pending;
	fan_persist_pending_valid = false;
	mutex_unlock(&fan_persist_mutex);

	fp = filp_open(FAN_STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(fp)) {
		pr_warn("fan state save failed: %ld\n", PTR_ERR(fp));
		return;
	}
	kernel_write(fp, &st, sizeof(st), &pos);
	filp_close(fp, NULL);
}

static void fan_persist_save(void)
{
	int i;

	if (!fan_persist_ready)
		return;

	mutex_lock(&fan_persist_mutex);
	mutex_lock(&fan_lock);
	fan_persist_pending.magic = FAN_STATE_MAGIC;
	fan_persist_pending.version = FAN_STATE_VERSION;
	fan_persist_pending.curve_user_saved = curve_user_saved ? 1 : 0;
	fan_persist_pending.curve_enabled = curve_enabled ? 1 : 0;
	fan_persist_pending.max_fan_manual = max_fan_manual ? 1 : 0;
	fan_persist_pending.num_points =
		(u8)clamp_t(int, curve_num_points, 0, MAX_CURVE_POINTS);
	memset(fan_persist_pending.temps, 0, sizeof(fan_persist_pending.temps));
	memset(fan_persist_pending.pcts, 0, sizeof(fan_persist_pending.pcts));
	for (i = 0; i < fan_persist_pending.num_points; i++) {
		fan_persist_pending.temps[i] = curve_temps_c[i];
		fan_persist_pending.pcts[i] = curve_pct[i];
	}
	strscpy(fan_persist_pending.thermal_profile, thermal_profile_name,
		sizeof(fan_persist_pending.thermal_profile));
	mutex_unlock(&fan_lock);
	fan_persist_pending_valid = true;
	mutex_unlock(&fan_persist_mutex);

	schedule_work(&fan_persist_work);
}

static int fan_persist_load(struct omen_fan_persist *out)
{
	struct file *fp;
	loff_t pos = 0;
	ssize_t ret;

	fp = filp_open(FAN_STATE_PATH, O_RDONLY, 0);
	if (IS_ERR(fp))
		return -ENOENT;

	ret = kernel_read(fp, out, sizeof(*out), &pos);
	filp_close(fp, NULL);
	if (ret != sizeof(*out))
		return -EINVAL;
	if (out->magic != FAN_STATE_MAGIC || out->version != FAN_STATE_VERSION)
		return -EINVAL;
	if (out->num_points > MAX_CURVE_POINTS)
		return -EINVAL;
	return 0;
}

static void fan_persist_apply(const struct omen_fan_persist *st)
{
	int i;
	bool want_override;
	bool want_max;

	mutex_lock(&fan_lock);
	if (st->curve_user_saved && st->num_points >= 2) {
		curve_user_saved = true;
		curve_num_points = st->num_points;
		for (i = 0; i < curve_num_points; i++) {
			curve_temps_c[i] = st->temps[i];
			curve_pct[i] = st->pcts[i];
		}
		fan_curve_sort_points();
		want_override = st->curve_enabled ? true : false;
	} else {
		curve_user_saved = false;
		want_override = false;
		if (st->thermal_profile[0])
			fan_preset_curve_install(st->thermal_profile);
	}
	curve_enabled = false;
	want_max = st->max_fan_manual ? true : false;
	mutex_unlock(&fan_lock);

	if (st->thermal_profile[0] &&
	    (sysfs_streq(st->thermal_profile, "silent") ||
	     sysfs_streq(st->thermal_profile, "normal") ||
	     sysfs_streq(st->thermal_profile, "performance")))
		fan_thermal_profile_apply(st->thermal_profile, true);

	if (want_max) {
		fan_max_apply(true, true);
	} else if (want_override && fan_curve_can_override()) {
#if IS_ENABLED(CONFIG_THERMAL)
		mutex_lock(&fan_lock);
		if (!fan_temp_zone_bind_default()) {
			curve_enabled = true;
			curve_last_pct = -1;
			curve_last_spd_valid = false;
			fan_curve_schedule_locked();
		}
		mutex_unlock(&fan_lock);
#endif
	}

	fan_persist_save();
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

	if (kstrtoul(buf, 10, &v))
		return -EINVAL;
	if (v > 1)
		return -EINVAL;

	fan_max_apply(v ? true : false, true);
	fan_persist_save();
	return count;
}

static ssize_t thermal_profile_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	char name[THERMAL_NAME_SZ];

	mutex_lock(&fan_lock);
	strscpy(name, thermal_profile_name, sizeof(name));
	mutex_unlock(&fan_lock);

	return sysfs_emit(buf, "%s\n", name);
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

	ret = fan_thermal_profile_apply(line, false);
	if (ret == -EINVAL)
		return -EINVAL;
	if (ret)
		return -EIO;
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
	char line[16];
	size_t len;

	while (*pbuf == ' ' || *pbuf == '\t' || *pbuf == '\n' || *pbuf == '\r')
		pbuf++;

	len = 0;
	while (pbuf[len] && pbuf[len] != ' ' && pbuf[len] != '\t' &&
	       pbuf[len] != '\n' && pbuf[len] != '\r' && len < sizeof(line) - 1)
		len++;
	memcpy(line, pbuf, len);
	line[len] = '\0';

	if (sysfs_streq(line, "default") || sysfs_streq(line, "ec")) {
		fan_curve_restore_ec(true);
		return count;
	}

#if !IS_ENABLED(CONFIG_THERMAL)
	return -ENODEV;
#endif

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
	curve_user_saved = true;
	curve_enabled = true;
	curve_last_pct = -1;
	curve_last_spd_valid = false;

	if (max_fan_manual || !fan_curve_can_override()) {
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}

#if IS_ENABLED(CONFIG_THERMAL)
	if (fan_temp_zone_bind_default()) {
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}
#endif

	fan_curve_schedule_locked();
	mutex_unlock(&fan_lock);
	fan_persist_save();
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

	if (!v) {
		fan_curve_restore_ec(true);
		return count;
	}

#if !IS_ENABLED(CONFIG_THERMAL)
	return -ENODEV;
#endif

	mutex_lock(&fan_lock);
	if (!curve_user_saved || curve_num_points < 2) {
		mutex_unlock(&fan_lock);
		return -EINVAL;
	}
	if (!fan_curve_can_override()) {
		mutex_unlock(&fan_lock);
		return -EINVAL;
	}
	if (max_fan_manual) {
		curve_enabled = true;
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}

#if IS_ENABLED(CONFIG_THERMAL)
	ret = fan_temp_zone_bind_default();
	if (ret) {
		mutex_unlock(&fan_lock);
		return ret;
	}
#endif

	curve_enabled = true;
	curve_last_pct = -1;
	curve_last_spd_valid = false;
	fan_curve_schedule_locked();
	mutex_unlock(&fan_lock);
	fan_persist_save();
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

static ssize_t power_limits_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	if (!power_limits_supported)
		return sysfs_emit(buf, "unsupported\n");
	if (!power_limits_known)
		return sysfs_emit(buf, "default\n");
	if (power_pl1 == HP_POWER_LIMIT_DEFAULT &&
	    power_pl2 == HP_POWER_LIMIT_DEFAULT)
		return sysfs_emit(buf, "default\n");
	return sysfs_emit(buf, "%u %u\n", power_pl1, power_pl2);
}

static ssize_t power_limits_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	char line[32];
	unsigned int pl1, pl2;
	int ret;

	if (!power_limits_supported)
		return -ENODEV;

	if (count >= sizeof(line))
		return -EINVAL;
	memcpy(line, buf, count);
	line[count] = '\0';
	strim(line);

	if (sysfs_streq(line, "default") || sysfs_streq(line, "0 0")) {
		pl1 = HP_POWER_LIMIT_DEFAULT;
		pl2 = HP_POWER_LIMIT_DEFAULT;
	} else if (sscanf(line, "%u %u", &pl1, &pl2) != 2) {
		return -EINVAL;
	}

	if (pl1 > 254 || pl2 > 254)
		return -EINVAL;
	if (pl2 < pl1)
		return -EINVAL;

	mutex_lock(&fan_lock);
	fan_victus_userdefine_trigger();
	ret = omen_set_cpu_pl1_pl2((u8)pl1, (u8)pl2);
	if (!ret) {
		power_pl1 = (u8)pl1;
		power_pl2 = (u8)pl2;
		power_limits_known = true;
	}
	mutex_unlock(&fan_lock);

	if (ret)
		return ret;
	return count;
}

static int omen_powersource_event(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct acpi_bus_event *event_entry = data;
	bool in_performance;
	int err;

	if (!event_entry ||
	    strcmp(event_entry->device_class, ACPI_AC_CLASS) != 0)
		return NOTIFY_DONE;

	mutex_lock(&fan_lock);
#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	in_performance =
		(active_platform_profile == PLATFORM_PROFILE_PERFORMANCE);
#else
	in_performance = sysfs_streq(thermal_profile_name, "performance");
#endif
	if (!in_performance || !power_limits_supported) {
		mutex_unlock(&fan_lock);
		return NOTIFY_DONE;
	}

	pr_debug("AC event: actualizing CPU PL1/PL2 for performance\n");
	fan_victus_userdefine_trigger();
	err = omen_set_cpu_pl1_pl2(HP_POWER_LIMIT_DEFAULT,
				  HP_POWER_LIMIT_DEFAULT);
	if (!err) {
		power_pl1 = HP_POWER_LIMIT_DEFAULT;
		power_pl2 = HP_POWER_LIMIT_DEFAULT;
		power_limits_known = true;
	} else {
		pr_warn("Failed to actualize power limits: %d\n", err);
	}
	mutex_unlock(&fan_lock);
	return NOTIFY_DONE;
}

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
static int omen_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int omen_platform_profile_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	*profile = active_platform_profile;
	return 0;
}

static int omen_platform_profile_set(struct device *dev,
				     enum platform_profile_option profile)
{
	const char *name = omen_pp_to_name(profile);

	if (!name)
		return -EOPNOTSUPP;

	return fan_thermal_profile_apply(name, true);
}

static const struct platform_profile_ops omen_platform_profile_ops = {
	.probe = omen_platform_profile_probe,
	.profile_get = omen_platform_profile_get,
	.profile_set = omen_platform_profile_set,
};

static void omen_platform_profile_init_from_ec(void)
{
	u8 ec = 0;
	const char *name;
	bool ctgp = false, ppab = false;
	u8 dstate = 0, slowdown = 0;

	if (fan_ec_profile_byte(&ec))
		return;

	name = fan_ec_byte_to_name(ec);
	if (sysfs_streq(name, "unknown"))
		return;

	/* Legacy: older builds cleared PPAB for silent. */
	if (sysfs_streq(name, "normal") &&
	    !omen_gpu_thermal_get(&ctgp, &ppab, &dstate, &slowdown) &&
	    !ctgp && !ppab)
		name = "silent";

	active_platform_profile = omen_name_to_pp(name);
	if (active_platform_profile == PLATFORM_PROFILE_LAST)
		active_platform_profile = PLATFORM_PROFILE_BALANCED;
	strscpy(thermal_profile_name, name, sizeof(thermal_profile_name));
}
#endif

static DEVICE_ATTR_RO(cpu_fan_rpm);
static DEVICE_ATTR_RO(gpu_fan_rpm);
static DEVICE_ATTR_RW(max_fan);
static DEVICE_ATTR_RW(thermal_profile);
static DEVICE_ATTR_RW(fan_curve);
static DEVICE_ATTR_RW(fan_curve_enable);
static DEVICE_ATTR_RW(fan_temp_zone);
static DEVICE_ATTR_RW(power_limits);

static struct attribute *fan_attrs[] = {
	&dev_attr_cpu_fan_rpm.attr,
	&dev_attr_gpu_fan_rpm.attr,
	&dev_attr_max_fan.attr,
	&dev_attr_thermal_profile.attr,
	&dev_attr_fan_curve.attr,
	&dev_attr_fan_curve_enable.attr,
	&dev_attr_fan_temp_zone.attr,
	&dev_attr_power_limits.attr,
	NULL,
};

static struct attribute_group fan_attr_group = {
	.name = "fan",
	.attrs = fan_attrs,
};

int omen_fan_setup(struct platform_device *pdev)
{
	int ret;
	struct omen_fan_persist loaded = {};
	bool have_persist = false;

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
	INIT_WORK(&fan_persist_work, fan_persist_work_fn);
	fan_pdev = pdev;

	ret = sysfs_create_group(&pdev->dev.kobj, &fan_attr_group);
	if (ret) {
		pr_warn("failed to create fan sysfs group: %d\n", ret);
		fan_pdev = NULL;
		return ret;
	}

	power_source_nb.notifier_call = omen_powersource_event;
	ret = register_acpi_notifier(&power_source_nb);
	if (ret)
		pr_warn("Failed to install ACPI power source notifier: %d\n",
			ret);
	else
		power_source_nb_registered = true;

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	omen_platform_profile_init_from_ec();
	platform_profile_dev = platform_profile_register(&pdev->dev,
							 "omen-rgb-keyboard",
							 NULL,
							 &omen_platform_profile_ops);
	if (IS_ERR(platform_profile_dev)) {
		pr_warn("platform_profile register failed: %ld (PPD integration disabled)\n",
			PTR_ERR(platform_profile_dev));
		platform_profile_dev = NULL;
	} else {
		platform_profile_registered = true;
		pr_info("platform_profile registered (PPD: low-power/balanced/performance)\n");
	}
#endif

	if (fan_iface == OMEN_FAN_IF_VICTUS_S)
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);

	if (!fan_persist_load(&loaded))
		have_persist = true;

	fan_persist_ready = true;

	if (have_persist) {
		pr_info("restoring saved fan state (override=%u custom=%u max=%u profile=%s)\n",
			loaded.curve_enabled, loaded.curve_user_saved,
			loaded.max_fan_manual, loaded.thermal_profile);
		fan_persist_apply(&loaded);
	} else {
		/* No saved state: apply built-in curve for the active profile. */
		fan_curve_after_profile(thermal_profile_name);
	}

	if (fan_iface == OMEN_FAN_IF_CLASSIC)
		pr_info("fan interface: classic WMI (RPM read, max fan)\n");
	else if (fan_iface == OMEN_FAN_IF_VICTUS_S)
		pr_info("fan interface: Victus-S WMI (RPM read, max fan, EC default, optional curve override)\n");
	else
		pr_info("fan interface: RPM queries unsupported; max_fan may still work\n");

	return 0;
}

void omen_fan_cleanup(void)
{
	int z = 0;

	fan_persist_save();
	fan_persist_ready = false;
	flush_work(&fan_persist_work);
	cancel_work_sync(&fan_persist_work);

#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	if (platform_profile_registered) {
		platform_profile_remove(platform_profile_dev);
		platform_profile_dev = NULL;
		platform_profile_registered = false;
	}
#endif

	if (power_source_nb_registered) {
		unregister_acpi_notifier(&power_source_nb);
		power_source_nb_registered = false;
	}

	mutex_lock(&fan_lock);
	fan_max_keepalive_armed = false;
	max_fan_manual = false;
	max_fan_auto = false;
	curve_enabled = false;
	mutex_unlock(&fan_lock);
	cancel_delayed_work_sync(&fan_max_keepalive);
	cancel_delayed_work_sync(&fan_curve_work);

	mutex_lock(&fan_lock);
	if (fan_iface == OMEN_FAN_IF_VICTUS_S)
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
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
	curve_user_saved = false;
	curve_last_pct = -1;
	curve_last_spd_valid = false;
	curve_tz_name[0] = '\0';
#if IS_ENABLED(CONFIG_THERMAL)
	curve_tz = NULL;
#endif
	power_limits_known = false;
	power_pl1 = HP_POWER_LIMIT_DEFAULT;
	power_pl2 = HP_POWER_LIMIT_DEFAULT;
	strscpy(thermal_profile_name, "normal", sizeof(thermal_profile_name));
#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	active_platform_profile = PLATFORM_PROFILE_BALANCED;
#endif
}
