// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Fan control via HP WMI
 *
 * Default: EC automatic fans. Built-in silent/normal/performance reference
 * curves are shown in fan_curve; writing setpoints overrides the EC until the
 * user restores automatic. Custom points persist across reboot. A 3°C band
 * keeps the fans from chattering. fan_curve drives the CPU fan and, until a
 * gpu_fan_curve is written, the GPU fan as well. The custom-speed switch
 * releases both. Percent scales linearly up to the measured hardware max
 * RPM for that fan. The driver measures those once, stores them next to the
 * animation state, and exposes them as fan/max_rpm. Later loads only read
 * the stored value. 100 on a curve means that stored max. max_fan is always
 * available. PPD drives the same silent/normal/performance thermal modes
 * via platform_profile.
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
#include <linux/delay.h>
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
#include "omen_state.h"
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

#define FAN_STATE_PATH		"/var/lib/omen-rgb-keyboard/fan_state"
#define FAN_MAX_RPM_PATH	"/var/lib/omen-rgb-keyboard/max_rpm"
#define FAN_STATE_MAGIC		0x4e41464f /* 'OFAN' little-endian */
#define FAN_STATE_VERSION	1
#define FAN_STATE_VERSION_RPM	2
#define FAN_STATE_VERSION_GPU	3
#define FAN_TZ_NAME_SZ		64

/* OMEN / Victus EC / WMI profile bytes (mainline hp-wmi) */
#define OMEN_V0_DEFAULT		0x00
#define OMEN_V0_PERFORMANCE	0x01
#define OMEN_V0_COOL		0x02
#define VICTUS_QUIET		0x03
#define OMEN_V1_DEFAULT		0x30
#define OMEN_V1_PERFORMANCE	0x31
#define OMEN_V1_COOL		0x50

/* Same layout as mainline hp-wmi. num_entries is not in the header; rows
 * run until an all-zero entry. cpu_rpm/gpu_rpm are RPM/100.
 */
struct victus_s_fan_table_header {
	u8 num_fans;
	u8 unknown;
} __packed;

struct victus_s_fan_table_entry {
	u8 cpu_rpm;
	u8 gpu_rpm;
	u8 noise_db;
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
	/* Historical tail. v1 files end before this. v2 added the learned
	 * RPM pair; those values now live in max_rpm and new saves write 0.
	 */
	u32 learned_cpu_max_rpm;
	u32 learned_gpu_max_rpm;
	/* v3. A short file leaves these zero, so the GPU fan follows the CPU curve. */
	u8 gpu_user_saved;
	u8 gpu_num_points;
	u8 pad[2];
	s32 gpu_temps[MAX_CURVE_POINTS];
	s32 gpu_pcts[MAX_CURVE_POINTS];
	char gpu_tz_name[FAN_TZ_NAME_SZ];
} __packed;

#define FAN_STATE_V1_SIZE	offsetof(struct omen_fan_persist, learned_cpu_max_rpm)
#define FAN_STATE_V2_SIZE	offsetof(struct omen_fan_persist, gpu_user_saved)

struct omen_curve {
	int temps[MAX_CURVE_POINTS];
	int pcts[MAX_CURVE_POINTS];
	int npoints;
	int last_pct;
	bool user_saved;
};

/* Same idea as struct animation_state: a small file the driver owns. */
struct omen_max_rpm_state {
	u32 cpu_rpm;
	u32 gpu_rpm;
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
static u8 fan_gpu_tbl_min, fan_gpu_tbl_max;
static int fan_gpu_delta;
static bool fan_tbl_valid;

static struct omen_curve cpu_curve = { .last_pct = -1 };
static struct omen_curve gpu_curve = { .last_pct = -1 };
static bool curve_enabled;
static int fan_learned_cpu_max;
static int fan_learned_gpu_max;
static bool fan_rpm_learning;
static struct work_struct fan_max_rpm_work;
static struct delayed_work fan_max_rpm_probe_work;
static int fan_max_rpm_probe_tries;
static DEFINE_MUTEX(fan_max_rpm_mutex);
static u32 fan_max_rpm_save_cpu;
static u32 fan_max_rpm_save_gpu;
static bool fan_max_rpm_save_pending;
static u8 curve_last_cpu_spd, curve_last_gpu_spd;
static bool curve_last_spd_valid;
static struct delayed_work fan_curve_work;
#if IS_ENABLED(CONFIG_THERMAL)
static struct thermal_zone_device *curve_tz;
static struct thermal_zone_device *gpu_tz;
#endif
static char curve_tz_name[FAN_TZ_NAME_SZ];
static char gpu_tz_name[FAN_TZ_NAME_SZ];

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
	int i, nmax, ret;
	u8 cpu, gpu, noise;
	u8 min_rpm = U8_MAX, max_rpm = 0;
	u8 gpu_min = U8_MAX, gpu_max = 0;
	bool have = false, have_gpu = false;

	ret = hp_wmi_perform_query(HPWMI_GM_VICTUS_FAN_TABLE_GET, HPWMI_GAMING,
				   fan_data, 4, sizeof(fan_data));
	if (ret)
		return ret;

	fan_table = (struct victus_s_fan_table *)fan_data;
	/* num_fans is informative. Rows are scanned until an all-zero entry
	 * so a quirky header cannot disable the curve.
	 */

	nmax = (int)((sizeof(fan_data) - sizeof(struct victus_s_fan_table_header)) /
		     sizeof(struct victus_s_fan_table_entry));
	for (i = 0; i < nmax; i++) {
		cpu = fan_table->entries[i].cpu_rpm;
		gpu = fan_table->entries[i].gpu_rpm;
		noise = fan_table->entries[i].noise_db;
		/* Table is truncated with an all-zero row (mainline hp-wmi). */
		if (!cpu && !gpu && !noise)
			break;
		if (!cpu)
			continue;
		if (!have) {
			fan_gpu_delta = (int)gpu - (int)cpu;
			have = true;
		}
		if (cpu < min_rpm)
			min_rpm = cpu;
		if (cpu > max_rpm)
			max_rpm = cpu;
		if (gpu) {
			have_gpu = true;
			if (gpu < gpu_min)
				gpu_min = gpu;
			if (gpu > gpu_max)
				gpu_max = gpu;
		}
	}
	if (!have || !max_rpm || min_rpm == U8_MAX)
		return -EINVAL;

	fan_tbl_min = min_rpm;
	fan_tbl_max = max_rpm;
	if (have_gpu && gpu_max) {
		fan_gpu_tbl_min = gpu_min;
		fan_gpu_tbl_max = gpu_max;
	} else {
		fan_gpu_tbl_min = min_rpm;
		fan_gpu_tbl_max = max_rpm;
	}
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
	       cpu_curve.npoints >= 2;
}

/* No saved GPU curve: the GPU fan uses the CPU curve's speed byte. */
static bool gpu_curve_linked(void)
{
	return !gpu_curve.user_saved || gpu_curve.npoints < 2;
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

	if (!cpu_curve.user_saved) {
		fan_preset_curve_install(name);
		curve_enabled = true;
	}

	keep_override = curve_enabled && !max_fan_manual &&
			fan_curve_can_override();
#if IS_ENABLED(CONFIG_THERMAL)
	/* The zone may not be registered yet during early boot. Leave the
	 * saved switch on; the worker binds once the sensor shows up.
	 */
	if (keep_override)
		fan_temp_zone_bind_default();
#else
	if (keep_override) {
		keep_override = false;
		curve_enabled = false;
	}
#endif

	release_to_ec = had_override && !keep_override && !max_fan_state &&
			fan_iface == OMEN_FAN_IF_VICTUS_S;

	if (!max_fan_manual && !fan_in_performance() && !max_fan_auto) {
		cpu_curve.last_pct = -1;
		gpu_curve.last_pct = -1;
	}

	if (keep_override) {
		cpu_curve.last_pct = -1;
		gpu_curve.last_pct = -1;
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
	return ret_a;
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

/*
 * 0 is EC automatic (true zero-RPM). 1–99 scale linearly in RPM up to the
 * tach reading taken at hardware max. Callers treat 100 separately: both
 * fans at 100 use the max-fan command, and one fan at 100 uses its top
 * table byte.
 */
static u8 fan_percent_to_speed(unsigned int pct, int learned, u8 tmin, u8 tmax)
{
	int target, byte;

	if (!fan_tbl_valid)
		return 0;
	if (pct == 0)
		return HP_FAN_SPEED_AUTOMATIC;
	if (pct > 100)
		pct = 100;

	if (learned >= 1000 && learned <= 20000) {
		if (pct >= 100)
			pct = 99;
		target = (int)(((s64)learned * pct) / 100);
		byte = (target + 50) / 100;
	} else if (tmax <= tmin) {
		return tmin;
	} else {
		byte = (int)tmin +
		       (int)(((u32)pct * (u32)(tmax - tmin)) / 100u);
	}

	if (byte < tmin)
		byte = tmin;
	if (byte > tmax)
		byte = tmax;
	return (u8)byte;
}

/* 0 stays automatic for that fan. 100 is the top manual table byte.
 * Hardware max for both fans is the max-fan command, used when every
 * active curve asks for it.
 */
static u8 fan_spd_for_pct(int pct, int learned, u8 tmin, u8 tmax)
{
	if (pct <= 0)
		return HP_FAN_SPEED_AUTOMATIC;
	if (pct >= 100)
		return tmax;
	return fan_percent_to_speed((unsigned int)pct, learned, tmin, tmax);
}

static int fan_victus_wmi_speed_set_pair(u8 cpu_spd, u8 gpu_spd)
{
	u8 fan_speed[2];
	int ret;

	if (fan_iface != OMEN_FAN_IF_VICTUS_S)
		return -ENODEV;

	fan_speed[0] = cpu_spd;
	fan_speed[1] = gpu_spd;

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

/* Same speed byte on both fans, plus the BIOS table's GPU offset. */
static int fan_victus_wmi_speed_set(u8 cpu_spd)
{
	u8 gpu_spd = cpu_spd;
	int gpu;

	if (cpu_spd != HP_FAN_SPEED_AUTOMATIC) {
		if (!fan_tbl_valid)
			return -ENODEV;
		gpu = (int)cpu_spd + fan_gpu_delta;
		gpu_spd = (u8)clamp_val(gpu, 0, 255);
	}
	return fan_victus_wmi_speed_set_pair(cpu_spd, gpu_spd);
}

/*
 * Return control to the EC without discarding saved custom setpoints.
 * Do not call while holding fan_lock (cancels work that may take the lock).
 */
static void fan_curve_restore_ec(bool persist)
{
	curve_enabled = false;
	cpu_curve.last_pct = -1;
	gpu_curve.last_pct = -1;
	curve_last_spd_valid = false;
	cancel_delayed_work_sync(&fan_curve_work);

	mutex_lock(&fan_lock);
	if (!max_fan_state && fan_iface == OMEN_FAN_IF_VICTUS_S)
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
	if (!cpu_curve.user_saved)
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

static int fan_curve_npoints(const struct omen_curve *c)
{
	if (c->npoints < 0)
		return 0;
	if (c->npoints > MAX_CURVE_POINTS)
		return MAX_CURVE_POINTS;
	return c->npoints;
}

static int curve_percent_for_temp_c(struct omen_curve *c, int temp_c)
{
	int i, lo_t, hi_t, lo_p, hi_p, n;

	n = fan_curve_npoints(c);
	if (n < 2)
		return -EINVAL;

	if (temp_c <= c->temps[0])
		return clamp_t(int, c->pcts[0], 0, 100);
	if (temp_c >= c->temps[n - 1])
		return clamp_t(int, c->pcts[n - 1], 0, 100);

	for (i = 0; i < n - 1; i++) {
		s64 span_t, span_p, den;

		lo_t = c->temps[i];
		hi_t = c->temps[i + 1];
		if (temp_c >= hi_t)
			continue;
		lo_p = c->pcts[i];
		hi_p = c->pcts[i + 1];
		span_t = (s64)temp_c - (s64)lo_t;
		span_p = (s64)hi_p - (s64)lo_p;
		den = (s64)hi_t - (s64)lo_t;
		if (den <= 0)
			return clamp_t(int, lo_p, 0, 100);
		{
			s64 pct = (s64)lo_p + span_t * span_p / den;

			if (pct < 0)
				return 0;
			if (pct > 100)
				return 100;
			return (int)pct;
		}
	}
	return clamp_t(int, c->pcts[n - 1], 0, 100);
}

/*
 * Schmitt-style band: ramp up follows the curve immediately; ramp down only
 * when the curve at (temp + HYST) is also below the held percent.
 */
static int curve_percent_with_hysteresis(struct omen_curve *c, int temp_c)
{
	int pct_now, pct_cool;

	pct_now = curve_percent_for_temp_c(c, temp_c);
	if (pct_now < 0)
		return pct_now;

	if (c->last_pct < 0) {
		c->last_pct = pct_now;
		return pct_now;
	}

	if (pct_now > c->last_pct) {
		c->last_pct = pct_now;
		return c->last_pct;
	}

	pct_cool = curve_percent_for_temp_c(c, temp_c + FAN_CURVE_HYST_C);
	if (pct_cool < 0)
		pct_cool = pct_now;
	if (pct_cool < c->last_pct)
		c->last_pct = pct_cool;

	return c->last_pct;
}

static void fan_curve_sort_points(struct omen_curve *c)
{
	int i, j, tmp, n;

	n = fan_curve_npoints(c);
	if (n < 2)
		return;

	for (i = 0; i < n - 1; i++) {
		for (j = i + 1; j < n; j++) {
			if (c->temps[j] < c->temps[i]) {
				tmp = c->temps[i];
				c->temps[i] = c->temps[j];
				c->temps[j] = tmp;
				tmp = c->pcts[i];
				c->pcts[i] = c->pcts[j];
				c->pcts[j] = tmp;
			}
		}
	}
}

/* 0% => zero-RPM. 100% => measured hardware max. Same points as before. */
static const int fan_preset_silent_t[] = { 50, 60, 70, 85, 95 };
static const int fan_preset_silent_p[] = { 0, 20, 35, 50, 65 };
static const int fan_preset_normal_t[] = { 45, 55, 68, 82, 98 };
static const int fan_preset_normal_p[] = { 0, 28, 55, 78, 95 };
static const int fan_preset_performance_t[] = { 40, 50, 62, 76, 92 };
static const int fan_preset_performance_p[] = { 0, 45, 68, 88, 100 };

static void fan_preset_curve_install(const char *name)
{
	const int *t, *p;
	int i, n;

	if (sysfs_streq(name, "silent")) {
		t = fan_preset_silent_t;
		p = fan_preset_silent_p;
		n = ARRAY_SIZE(fan_preset_silent_t);
	} else if (sysfs_streq(name, "normal")) {
		t = fan_preset_normal_t;
		p = fan_preset_normal_p;
		n = ARRAY_SIZE(fan_preset_normal_t);
	} else if (sysfs_streq(name, "performance")) {
		t = fan_preset_performance_t;
		p = fan_preset_performance_p;
		n = ARRAY_SIZE(fan_preset_performance_t);
	} else {
		return;
	}

	if (n > MAX_CURVE_POINTS)
		n = MAX_CURVE_POINTS;
	for (i = 0; i < n; i++) {
		cpu_curve.temps[i] = t[i];
		cpu_curve.pcts[i] = p[i];
	}
	cpu_curve.npoints = n;
	fan_curve_sort_points(&cpu_curve);
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
		fan_temp_zone_bind_default();
#endif
		cpu_curve.last_pct = -1;
		gpu_curve.last_pct = -1;
		fan_curve_schedule_locked();
	} else if (fan_iface == OMEN_FAN_IF_VICTUS_S) {
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);
		fan_curve_schedule_locked();
	}
	mutex_unlock(&fan_lock);
}

/* 100% on the active curve is hardware max, on any profile. A manual
 * max_fan latch is left alone. Caller holds fan_lock; this may drop it.
 */
static void fan_auto_max_update_locked(int demand_pct)
{
	bool driving, want_on;

	if (max_fan_manual)
		return;

	driving = curve_enabled && fan_curve_can_override();
	want_on = driving && demand_pct >= 100;

	if (want_on == max_fan_auto)
		return;

	mutex_unlock(&fan_lock);
	fan_max_apply(want_on, false);
	mutex_lock(&fan_lock);
}

static void fan_curve_work_fn(struct work_struct *work)
{
	long temp_mc = 0;
	int cpu_temp, gpu_temp, cpu_pct, gpu_pct, demand, ret, run_again;
	int gpu_learned;
	u8 cpu_spd, gpu_spd;
	bool own_gpu, drive_cpu, drive_gpu;

	mutex_lock(&fan_lock);

	if (fan_rpm_learning || max_fan_manual) {
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
	if (gpu_tz_name[0] && !gpu_tz) {
		struct thermal_zone_device *tz;

		tz = thermal_zone_get_zone_by_name(gpu_tz_name);
		if (!IS_ERR_OR_NULL(tz))
			gpu_tz = tz;
	}
#endif

	ret = fan_temp_mc(&temp_mc);
	cpu_temp = ret ? 50 : (int)(temp_mc / 1000);
	gpu_temp = cpu_temp;
#if IS_ENABLED(CONFIG_THERMAL)
	if (gpu_tz) {
		int t;

		if (!thermal_zone_get_temp(gpu_tz, &t))
			gpu_temp = t / 1000;
	}
#endif

	drive_cpu = curve_enabled && fan_curve_can_override();
	/* The custom-speed switch gates both curves. A saved GPU curve
	 * stays on disk, but it must not keep driving after that switch is off.
	 */
	own_gpu = drive_cpu && !gpu_curve_linked();
	cpu_pct = 0;
	gpu_pct = 0;
	if (drive_cpu) {
		cpu_pct = curve_percent_with_hysteresis(&cpu_curve, cpu_temp);
		if (cpu_pct < 0)
			cpu_pct = 0;
	}
	if (own_gpu) {
		gpu_pct = curve_percent_with_hysteresis(&gpu_curve, gpu_temp);
		if (gpu_pct < 0)
			gpu_pct = 0;
	} else {
		gpu_pct = cpu_pct;
	}

	/*
	 * The max-fan command spins both fans. Use it only when every fan
	 * this worker is driving asks for hardware max. One fan at 100%
	 * gets the top manual table byte instead.
	 */
	if (!curve_enabled)
		demand = 0;
	else if (!own_gpu)
		demand = cpu_pct;
	else
		demand = min_t(int, cpu_pct, gpu_pct);
	fan_auto_max_update_locked(demand);

	drive_cpu = drive_cpu && !max_fan_state;
	drive_gpu = own_gpu && !max_fan_state;

	if (drive_cpu || drive_gpu) {
		if (drive_cpu)
			cpu_spd = fan_spd_for_pct(cpu_pct, fan_learned_cpu_max,
						  fan_tbl_min, fan_tbl_max);
		else
			cpu_spd = HP_FAN_SPEED_AUTOMATIC;

		if (own_gpu) {
			gpu_learned = fan_learned_gpu_max;
			if (gpu_learned < 1000 || gpu_learned > 20000)
				gpu_learned = fan_learned_cpu_max;
			gpu_spd = fan_spd_for_pct(gpu_pct, gpu_learned,
						  fan_gpu_tbl_min, fan_gpu_tbl_max);
			if (!curve_last_spd_valid ||
			    cpu_spd != curve_last_cpu_spd ||
			    gpu_spd != curve_last_gpu_spd) {
				ret = fan_victus_wmi_speed_set_pair(cpu_spd, gpu_spd);
				if (ret)
					pr_debug("fan curve apply failed: %d\n", ret);
				else {
					curve_last_cpu_spd = cpu_spd;
					curve_last_gpu_spd = gpu_spd;
					curve_last_spd_valid = true;
				}
			}
		} else if (!curve_last_spd_valid ||
			   cpu_spd != curve_last_cpu_spd) {
			ret = fan_victus_wmi_speed_set(cpu_spd);
			if (ret)
				pr_debug("fan curve apply failed: %d\n", ret);
			else {
				curve_last_cpu_spd = cpu_spd;
				curve_last_gpu_spd = cpu_spd;
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
	ssize_t written;
	int err;

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
	written = kernel_write(fp, &st, sizeof(st), &pos);
	err = filp_close(fp, NULL);
	if (written != (ssize_t)sizeof(st))
		pr_warn("fan state save failed: write returned %zd of %zu bytes\n",
			written, sizeof(st));
	else if (err)
		pr_warn("fan state save failed: close returned %d\n", err);
}

static void fan_persist_save(void)
{
	int i;

	if (!fan_persist_ready)
		return;

	mutex_lock(&fan_persist_mutex);
	mutex_lock(&fan_lock);
	memset(&fan_persist_pending, 0, sizeof(fan_persist_pending));
	fan_persist_pending.magic = FAN_STATE_MAGIC;
	fan_persist_pending.version = FAN_STATE_VERSION_GPU;
	fan_persist_pending.curve_user_saved = cpu_curve.user_saved ? 1 : 0;
	fan_persist_pending.curve_enabled = curve_enabled ? 1 : 0;
	fan_persist_pending.max_fan_manual = max_fan_manual ? 1 : 0;
	fan_persist_pending.num_points =
		(u8)clamp_t(int, cpu_curve.npoints, 0, MAX_CURVE_POINTS);
	for (i = 0; i < fan_persist_pending.num_points; i++) {
		fan_persist_pending.temps[i] = cpu_curve.temps[i];
		fan_persist_pending.pcts[i] = cpu_curve.pcts[i];
	}
	strscpy(fan_persist_pending.thermal_profile, thermal_profile_name,
		sizeof(fan_persist_pending.thermal_profile));
	fan_persist_pending.gpu_user_saved = gpu_curve.user_saved ? 1 : 0;
	fan_persist_pending.gpu_num_points =
		(u8)clamp_t(int, gpu_curve.npoints, 0, MAX_CURVE_POINTS);
	for (i = 0; i < fan_persist_pending.gpu_num_points; i++) {
		fan_persist_pending.gpu_temps[i] = gpu_curve.temps[i];
		fan_persist_pending.gpu_pcts[i] = gpu_curve.pcts[i];
	}
	strscpy(fan_persist_pending.gpu_tz_name, gpu_tz_name,
		sizeof(fan_persist_pending.gpu_tz_name));
	mutex_unlock(&fan_lock);
	fan_persist_pending_valid = true;
	mutex_unlock(&fan_persist_mutex);

	schedule_work(&fan_persist_work);
}

static int fan_persist_load(struct omen_fan_persist *out)
{
	ssize_t ret;

	ret = omen_read_state_file(FAN_STATE_PATH, out, sizeof(*out));
	if (ret == -ENOENT)
		return -ENOENT;
	if (ret < 0) {
		pr_warn("fan state load from %s failed: %zd\n", FAN_STATE_PATH, ret);
		return ret;
	}
	if (ret != (ssize_t)sizeof(*out) && ret != FAN_STATE_V1_SIZE &&
	    ret != FAN_STATE_V2_SIZE)
		return -EINVAL;
	if (out->magic != FAN_STATE_MAGIC)
		return -EINVAL;
	if (ret == FAN_STATE_V1_SIZE) {
		if (out->version != FAN_STATE_VERSION)
			return -EINVAL;
	} else if (ret == FAN_STATE_V2_SIZE) {
		if (out->version != FAN_STATE_VERSION &&
		    out->version != FAN_STATE_VERSION_RPM)
			return -EINVAL;
	} else if (out->version != FAN_STATE_VERSION_GPU) {
		return -EINVAL;
	}
	if (out->num_points > MAX_CURVE_POINTS)
		return -EINVAL;
	if (out->gpu_num_points > MAX_CURVE_POINTS)
		return -EINVAL;
	/* Fixed-size names may lack a NUL. Never scan past them. */
	out->thermal_profile[sizeof(out->thermal_profile) - 1] = '\0';
	out->gpu_tz_name[sizeof(out->gpu_tz_name) - 1] = '\0';
	return 0;
}

static bool fan_persist_points_ok(const s32 *temps, const s32 *pcts, u8 n)
{
	int i;

	if (n < 2 || n > MAX_CURVE_POINTS)
		return false;
	for (i = 0; i < n; i++) {
		if (temps[i] < 0 || temps[i] > 120)
			return false;
		if (pcts[i] < 0 || pcts[i] > 100)
			return false;
	}
	return true;
}

static bool fan_persist_points_sane(const struct omen_fan_persist *st)
{
	return fan_persist_points_ok(st->temps, st->pcts, st->num_points);
}

static bool fan_tz_name_ok(const char *name)
{
	size_t i, n = strnlen(name, FAN_TZ_NAME_SZ);

	if (n >= FAN_TZ_NAME_SZ)
		return false;
	for (i = 0; i < n; i++) {
		unsigned char c = name[i];

		if (c <= ' ' || c >= 0x7f || c == '/')
			return false;
	}
	return true;
}

static void fan_persist_apply(const struct omen_fan_persist *st)
{
	int i;
	bool want_override;
	bool want_max;

	mutex_lock(&fan_lock);
	if (st->curve_user_saved && fan_persist_points_sane(st)) {
		cpu_curve.user_saved = true;
		cpu_curve.npoints = st->num_points;
		for (i = 0; i < cpu_curve.npoints; i++) {
			cpu_curve.temps[i] = st->temps[i];
			cpu_curve.pcts[i] = st->pcts[i];
		}
		fan_curve_sort_points(&cpu_curve);
		want_override = st->curve_enabled ? true : false;
	} else {
		cpu_curve.user_saved = false;
		want_override = false;
		if (st->thermal_profile[0])
			fan_preset_curve_install(st->thermal_profile);
	}
	if (st->gpu_user_saved == 1 &&
	    fan_persist_points_ok(st->gpu_temps, st->gpu_pcts, st->gpu_num_points)) {
		gpu_curve.user_saved = true;
		gpu_curve.npoints = st->gpu_num_points;
		for (i = 0; i < gpu_curve.npoints; i++) {
			gpu_curve.temps[i] = st->gpu_temps[i];
			gpu_curve.pcts[i] = st->gpu_pcts[i];
		}
		fan_curve_sort_points(&gpu_curve);
	} else {
		gpu_curve.user_saved = false;
		gpu_curve.npoints = 0;
	}
	if (fan_tz_name_ok(st->gpu_tz_name))
		strscpy(gpu_tz_name, st->gpu_tz_name, sizeof(gpu_tz_name));
	else
		gpu_tz_name[0] = '\0';
	/* Set this before the profile apply below. That path saves state, and
	 * it must not persist the switch as off while the saved file said on.
	 */
	curve_enabled = want_override && fan_curve_can_override();
	want_max = st->max_fan_manual ? true : false;
	mutex_unlock(&fan_lock);

	if (st->thermal_profile[0] &&
	    (sysfs_streq(st->thermal_profile, "silent") ||
	     sysfs_streq(st->thermal_profile, "normal") ||
	     sysfs_streq(st->thermal_profile, "performance")))
		fan_thermal_profile_apply(st->thermal_profile, true);

	if (want_max) {
		fan_max_apply(true, true);
	} else if (curve_enabled) {
		mutex_lock(&fan_lock);
#if IS_ENABLED(CONFIG_THERMAL)
		fan_temp_zone_bind_default();
#endif
		cpu_curve.last_pct = -1;
		gpu_curve.last_pct = -1;
		curve_last_spd_valid = false;
		fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
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
	if (fan_rpm_learning)
		return -EBUSY;

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

	if (strscpy(line, buf, sizeof(line)) < 0)
		return -EINVAL;
	strim(line);

	ret = fan_thermal_profile_apply(line, false);
	if (ret == -EINVAL)
		return -EINVAL;
	if (ret)
		return -EIO;
	return count;
}

static int fan_curve_emit(char *buf, const struct omen_curve *c)
{
	int i, n = 0, npts = fan_curve_npoints(c);

	for (i = 0; i < npts && n < PAGE_SIZE; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, "%d:%d%s",
			       c->temps[i], c->pcts[i],
			       (i < npts - 1) ? " " : "");
	if (!n)
		return sysfs_emit(buf, "(unset)\n");
	if (n < PAGE_SIZE)
		n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	return n;
}

static ssize_t fan_curve_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int n;

	mutex_lock(&fan_lock);
	n = fan_curve_emit(buf, &cpu_curve);
	mutex_unlock(&fan_lock);
	return n;
}

/* Word match that never reads past end. */
static bool fan_curve_span_is(const char *s, const char *end, const char *word)
{
	size_t n = strlen(word);

	if (s > end || (size_t)(end - s) < n)
		return false;
	if (memcmp(s, word, n))
		return false;
	if ((size_t)(end - s) == n)
		return true;
	return s[n] == '\n' || s[n] == '\r' || s[n] == ' ' || s[n] == '\t';
}

static const char *fan_skip_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

/* Decimal in [0, max]. Stops before the running total can wrap. */
static int fan_parse_dec(const char **pp, const char *end, int max, int *out)
{
	const char *p = *pp;
	unsigned int v = 0;
	bool any = false;

	if (max < 0)
		return -EINVAL;
	while (p < end && *p >= '0' && *p <= '9') {
		unsigned int d = *p - '0';

		if (v > (unsigned int)max / 10 ||
		    (v == (unsigned int)max / 10 && d > (unsigned int)max % 10))
			return -EINVAL;
		v = v * 10u + d;
		any = true;
		p++;
	}
	if (!any)
		return -EINVAL;
	*out = (int)v;
	*pp = p;
	return 0;
}

/*
 * 0–100 is a percent (100 and "max" are the measured hardware max).
 * A number above 100 is an RPM, stored as the matching percent.
 * Only bytes in [s, end) are examined.
 */
static int fan_curve_parse_speed(const char **pp, const char *end,
				 int learned, int *pct)
{
	const char *p = *pp;
	int v;

	if (fan_curve_span_is(p, end, "max")) {
		*pct = 100;
		*pp = p + 3;
		return 0;
	}

	if (fan_parse_dec(&p, end, 20000, &v))
		return -EINVAL;
	if (v <= 100) {
		*pct = v;
		*pp = p;
		return 0;
	}
	if (learned < 1000 || learned > 20000)
		return -EINVAL;
	/* Round so the stored percent maps back onto the requested RPM. */
	v = (int)(((s64)v * 100 + learned / 2) / learned);
	if (v < 1)
		v = 1;
	if (v > 100)
		v = 100;
	*pct = v;
	*pp = p;
	return 0;
}

static int fan_curve_learned_for(bool gpu)
{
	int learned = gpu ? fan_learned_gpu_max : fan_learned_cpu_max;

	if (learned < 1000 || learned > 20000)
		learned = fan_learned_cpu_max;
	return learned;
}

/* Fills temps/pcts. Does not touch the live curves. */
static int fan_curve_parse_points(const char *buf, size_t count, int learned,
				  int *temps, int *pcts, int *out_n)
{
	const char *end, *p;
	int npt = 0;

	if (!buf || count > PAGE_SIZE)
		return -EINVAL;
	end = buf + count;
	p = fan_skip_ws(buf, end);
	while (npt < MAX_CURVE_POINTS) {
		int t, pct;

		p = fan_skip_ws(p, end);
		if (p >= end)
			break;
		if (fan_parse_dec(&p, end, 120, &t) || p >= end || *p != ':')
			return -EINVAL;
		p++;
		if (fan_curve_parse_speed(&p, end, learned, &pct))
			return -EINVAL;
		temps[npt] = t;
		pcts[npt] = pct;
		npt++;
	}
	p = fan_skip_ws(p, end);
	if (p < end || npt < 2)
		return -EINVAL;
	*out_n = npt;
	return 0;
}

static void fan_curve_commit(struct omen_curve *c, const int *temps,
			     const int *pcts, int npt)
{
	int i;

	if (npt < 2)
		return;
	if (npt > MAX_CURVE_POINTS)
		npt = MAX_CURVE_POINTS;
	for (i = 0; i < npt; i++) {
		c->temps[i] = clamp_t(int, temps[i], 0, 120);
		c->pcts[i] = clamp_t(int, pcts[i], 0, 100);
	}
	c->npoints = npt;
	c->user_saved = true;
	c->last_pct = -1;
	fan_curve_sort_points(c);
	curve_last_spd_valid = false;
}

static ssize_t fan_curve_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int temps[MAX_CURVE_POINTS];
	int pcts[MAX_CURVE_POINTS];
	int npt = 0;
	const char *end, *p;

	if (!buf || count > PAGE_SIZE)
		return -EINVAL;

	end = buf + count;
	p = fan_skip_ws(buf, end);

	if (fan_curve_span_is(p, end, "default") || fan_curve_span_is(p, end, "ec")) {
		fan_curve_restore_ec(true);
		return count;
	}

#if !IS_ENABLED(CONFIG_THERMAL)
	return -ENODEV;
#endif

	if (fan_curve_parse_points(buf, count, fan_curve_learned_for(false),
				   temps, pcts, &npt))
		return -EINVAL;

	mutex_lock(&fan_lock);
	fan_curve_commit(&cpu_curve, temps, pcts, npt);
	curve_enabled = true;

	if (max_fan_manual || !fan_curve_can_override()) {
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}

#if IS_ENABLED(CONFIG_THERMAL)
	fan_temp_zone_bind_default();
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
	if (!cpu_curve.user_saved || cpu_curve.npoints < 2) {
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
	fan_temp_zone_bind_default();
#endif

	curve_enabled = true;
	cpu_curve.last_pct = -1;
	gpu_curve.last_pct = -1;
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

	if (strscpy(line, buf, sizeof(line)) < 0)
		return -EINVAL;
	strim(line);
	if (!line[0] || !fan_tz_name_ok(line))
		return -EINVAL;

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

static ssize_t gpu_fan_curve_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int n;
	bool linked;

	mutex_lock(&fan_lock);
	linked = gpu_curve_linked();
	n = linked ? 0 : fan_curve_emit(buf, &gpu_curve);
	mutex_unlock(&fan_lock);
	if (linked)
		return sysfs_emit(buf, "(cpu)\n");
	return n;
}

static ssize_t gpu_fan_curve_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int temps[MAX_CURVE_POINTS];
	int pcts[MAX_CURVE_POINTS];
	int npt = 0;
	const char *end, *p;

	if (!buf || count > PAGE_SIZE)
		return -EINVAL;

	end = buf + count;
	p = fan_skip_ws(buf, end);

	/* Drop the GPU curve. The GPU fan follows the CPU curve again. */
	if (fan_curve_span_is(p, end, "cpu") || fan_curve_span_is(p, end, "default")) {
		mutex_lock(&fan_lock);
		gpu_curve.user_saved = false;
		gpu_curve.npoints = 0;
		gpu_curve.last_pct = -1;
		curve_last_spd_valid = false;
		if (fan_worker_needed_locked())
			fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}

#if !IS_ENABLED(CONFIG_THERMAL)
	return -ENODEV;
#endif

	if (fan_curve_parse_points(buf, count, fan_curve_learned_for(true),
				   temps, pcts, &npt))
		return -EINVAL;

	mutex_lock(&fan_lock);
	fan_curve_commit(&gpu_curve, temps, pcts, npt);
	if (max_fan_manual || fan_iface != OMEN_FAN_IF_VICTUS_S || !fan_tbl_valid) {
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}
	fan_curve_schedule_locked();
	mutex_unlock(&fan_lock);
	fan_persist_save();
	return count;
}

static ssize_t gpu_fan_temp_zone_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	mutex_lock(&fan_lock);
	if (!gpu_tz_name[0]) {
		mutex_unlock(&fan_lock);
		return sysfs_emit(buf, "(cpu)\n");
	}
	mutex_unlock(&fan_lock);
	return sysfs_emit(buf, "%s\n", gpu_tz_name);
}

static ssize_t gpu_fan_temp_zone_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	char line[FAN_TZ_NAME_SZ];

	if (strscpy(line, buf, sizeof(line)) < 0)
		return -EINVAL;
	strim(line);

	if (!line[0] || sysfs_streq(line, "cpu") || sysfs_streq(line, "default")) {
		mutex_lock(&fan_lock);
		gpu_tz_name[0] = '\0';
#if IS_ENABLED(CONFIG_THERMAL)
		gpu_tz = NULL;
#endif
		gpu_curve.last_pct = -1;
		curve_last_spd_valid = false;
		if (fan_worker_needed_locked())
			fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
		fan_persist_save();
		return count;
	}

	if (!fan_tz_name_ok(line))
		return -EINVAL;

#if IS_ENABLED(CONFIG_THERMAL)
	{
		struct thermal_zone_device *tz;

		tz = thermal_zone_get_zone_by_name(line);
		if (IS_ERR_OR_NULL(tz))
			return -EINVAL;
		mutex_lock(&fan_lock);
		strscpy(gpu_tz_name, line, sizeof(gpu_tz_name));
		gpu_tz = tz;
		gpu_curve.last_pct = -1;
		curve_last_spd_valid = false;
		if (fan_worker_needed_locked())
			fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
	}
	fan_persist_save();
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
	char extra;
	unsigned int pl1, pl2;
	int ret;

	if (!power_limits_supported)
		return -ENODEV;

	if (strscpy(line, buf, sizeof(line)) < 0)
		return -EINVAL;
	strim(line);

	if (sysfs_streq(line, "default") || sysfs_streq(line, "0 0")) {
		pl1 = HP_POWER_LIMIT_DEFAULT;
		pl2 = HP_POWER_LIMIT_DEFAULT;
	} else if (sscanf(line, "%3u %3u %c", &pl1, &pl2, &extra) != 2) {
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

static ssize_t max_rpm_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int cpu, gpu;

	mutex_lock(&fan_lock);
	cpu = fan_learned_cpu_max;
	gpu = fan_learned_gpu_max;
	mutex_unlock(&fan_lock);

	if (cpu < 1000 || cpu > 20000 || gpu < 0 || gpu > 20000)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%d %d\n", cpu, gpu);
}

static DEVICE_ATTR_RO(cpu_fan_rpm);
static DEVICE_ATTR_RO(gpu_fan_rpm);
static DEVICE_ATTR_RO(max_rpm);
static DEVICE_ATTR_RW(max_fan);
static DEVICE_ATTR_RW(thermal_profile);
static DEVICE_ATTR_RW(fan_curve);
static DEVICE_ATTR_RW(gpu_fan_curve);
static DEVICE_ATTR_RW(fan_curve_enable);
static DEVICE_ATTR_RW(fan_temp_zone);
static DEVICE_ATTR_RW(gpu_fan_temp_zone);
static DEVICE_ATTR_RW(power_limits);

static struct attribute *fan_attrs[] = {
	&dev_attr_cpu_fan_rpm.attr,
	&dev_attr_gpu_fan_rpm.attr,
	&dev_attr_max_rpm.attr,
	&dev_attr_max_fan.attr,
	&dev_attr_thermal_profile.attr,
	&dev_attr_fan_curve.attr,
	&dev_attr_gpu_fan_curve.attr,
	&dev_attr_fan_curve_enable.attr,
	&dev_attr_fan_temp_zone.attr,
	&dev_attr_gpu_fan_temp_zone.attr,
	&dev_attr_power_limits.attr,
	NULL,
};

static struct attribute_group fan_attr_group = {
	.name = "fan",
	.attrs = fan_attrs,
};

static void fan_max_rpm_save_work(struct work_struct *work)
{
	struct omen_max_rpm_state st;
	struct file *fp;
	loff_t pos = 0;
	ssize_t written;
	int err;

	mutex_lock(&fan_max_rpm_mutex);
	if (!fan_max_rpm_save_pending) {
		mutex_unlock(&fan_max_rpm_mutex);
		return;
	}
	st.cpu_rpm = fan_max_rpm_save_cpu;
	st.gpu_rpm = fan_max_rpm_save_gpu;
	fan_max_rpm_save_pending = false;
	mutex_unlock(&fan_max_rpm_mutex);

	fp = filp_open(FAN_MAX_RPM_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(fp)) {
		pr_warn("max rpm save failed: %ld\n", PTR_ERR(fp));
		return;
	}
	written = kernel_write(fp, &st, sizeof(st), &pos);
	err = filp_close(fp, NULL);
	if (written != sizeof(st))
		pr_warn("max rpm save failed: write returned %zd of %zu bytes\n",
			written, sizeof(st));
	else if (err)
		pr_warn("max rpm save failed: close returned %d\n", err);
	else
		pr_info("max fan RPM stored: cpu %u gpu %u\n",
			st.cpu_rpm, st.gpu_rpm);
}

static void fan_max_rpm_store(void)
{
	if (fan_learned_cpu_max < 1000 || fan_learned_cpu_max > 20000)
		return;

	mutex_lock(&fan_max_rpm_mutex);
	fan_max_rpm_save_cpu = fan_learned_cpu_max;
	fan_max_rpm_save_gpu = (fan_learned_gpu_max > 0 &&
				fan_learned_gpu_max <= 20000) ?
				fan_learned_gpu_max : 0;
	fan_max_rpm_save_pending = true;
	mutex_unlock(&fan_max_rpm_mutex);
	schedule_work(&fan_max_rpm_work);
}

/* 0 if a stored measurement was loaded. -ENOENT if this is the first run. */
static int fan_max_rpm_load(void)
{
	struct omen_max_rpm_state st = {};
	ssize_t ret;

	ret = omen_read_state_file(FAN_MAX_RPM_PATH, &st, sizeof(st));
	if (ret == -ENOENT)
		return -ENOENT;
	if (ret < 0) {
		pr_warn("max rpm load failed: %zd\n", ret);
		return ret;
	}
	if (ret != sizeof(st) || st.cpu_rpm < 1000 || st.cpu_rpm > 20000 ||
	    st.gpu_rpm > 20000)
		return -EINVAL;

	fan_learned_cpu_max = st.cpu_rpm;
	fan_learned_gpu_max = st.gpu_rpm;
	return 0;
}

static void fan_rpm_note(int cpu, int gpu, int *best_cpu, int *best_gpu,
			 int *quiet)
{
	if (cpu > *best_cpu) {
		*best_cpu = cpu;
		*quiet = 0;
	} else if (*best_cpu >= 2500) {
		(*quiet)++;
	}
	if (gpu > *best_gpu)
		*best_gpu = gpu;
}

/*
 * Spin the fans at hardware max and record the tach. Only used when
 * max_rpm has never been stored. Some boards report a real tach during
 * the max command. Others keep that read at 0 until the command is
 * cleared, then report the speed the fans are still turning.
 */
static int fan_learn_max_rpm(void)
{
	int en = 1;
	int i, cpu, gpu, best_cpu = 0, best_gpu = 0;
	int quiet = 0;
	int ret;

	if (fan_iface == OMEN_FAN_IF_NONE)
		return -ENODEV;

	ret = hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
				   &en, sizeof(en), 0);
	if (ret)
		return ret < 0 ? ret : -EIO;

	/*
	 * These fans take well over 10s to reach hardware max from a stop.
	 * Keep the high-water mark and stop once it has not risen for 2s,
	 * or at 20s.
	 */
	for (i = 0; i < 40; i++) {
		msleep(500);
		cpu = fan_rpm_read(FAN_CPU);
		gpu = fan_rpm_read(FAN_GPU);
		fan_rpm_note(cpu, gpu, &best_cpu, &best_gpu, &quiet);
		if (quiet >= 4)
			break;
	}

	en = 0;
	hp_wmi_perform_query(HPWMI_GM_FAN_SPEED_MAX_SET, HPWMI_GAMING,
			     &en, sizeof(en), 0);

	if (best_cpu < 1000) {
		pr_info("max fan tach read cpu %d gpu %d while boost was on; sampling after release\n",
			best_cpu, best_gpu);
		quiet = 0;
		for (i = 0; i < 16; i++) {
			msleep(250);
			cpu = fan_rpm_read(FAN_CPU);
			gpu = fan_rpm_read(FAN_GPU);
			fan_rpm_note(cpu, gpu, &best_cpu, &best_gpu, &quiet);
			if (best_cpu >= 2500 && quiet >= 4)
				break;
		}
	}

	if (fan_iface == OMEN_FAN_IF_VICTUS_S)
		fan_victus_wmi_speed_set(HP_FAN_SPEED_AUTOMATIC);

	if (best_cpu < 1000 || best_cpu > 20000) {
		pr_info("max fan RPM probe got cpu %d gpu %d\n",
			best_cpu, best_gpu);
		return -EIO;
	}
	if (best_gpu < 0 || best_gpu > 20000)
		best_gpu = 0;

	fan_learned_cpu_max = best_cpu;
	fan_learned_gpu_max = best_gpu;
	pr_info("learned max fan RPM: cpu %d gpu %d\n", best_cpu, best_gpu);
	return 0;
}

static void fan_max_rpm_probe_work_fn(struct work_struct *work)
{
	int ret;

	if (fan_learned_cpu_max >= 1000 && fan_learned_cpu_max <= 20000)
		return;
	if (!fan_max_rpm_load()) {
		pr_info("max fan RPM already stored: cpu %d gpu %d\n",
			fan_learned_cpu_max, fan_learned_gpu_max);
		return;
	}

	fan_rpm_learning = true;
	ret = fan_learn_max_rpm();
	fan_rpm_learning = false;
	if (!ret) {
		fan_max_rpm_store();
		mutex_lock(&fan_lock);
		cpu_curve.last_pct = -1;
		gpu_curve.last_pct = -1;
		curve_last_spd_valid = false;
		fan_curve_schedule_locked();
		mutex_unlock(&fan_lock);
		return;
	}

	if (ret == -EIO && fan_max_rpm_probe_tries < 3) {
		fan_max_rpm_probe_tries++;
		schedule_delayed_work(&fan_max_rpm_probe_work, 30 * HZ);
		return;
	}
	pr_info("max fan RPM not learned (%d); percent maps across the BIOS table\n",
		ret);
}

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
			pr_info("Victus fan table loaded (cpu %u..%u = %u..%u RPM, gpu delta %+d)\n",
				fan_tbl_min, fan_tbl_max,
				fan_tbl_min * 100, fan_tbl_max * 100,
				fan_gpu_delta);
	}

	INIT_DELAYED_WORK(&fan_max_keepalive, fan_max_keepalive_fn);
	INIT_DELAYED_WORK(&fan_curve_work, fan_curve_work_fn);
	INIT_WORK(&fan_persist_work, fan_persist_work_fn);
	INIT_WORK(&fan_max_rpm_work, fan_max_rpm_save_work);
	INIT_DELAYED_WORK(&fan_max_rpm_probe_work, fan_max_rpm_probe_work_fn);
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

	if (!fan_max_rpm_load()) {
		pr_info("max fan RPM already stored: cpu %d gpu %d\n",
			fan_learned_cpu_max, fan_learned_gpu_max);
	} else if (have_persist && loaded.learned_cpu_max_rpm >= 1000 &&
		   loaded.learned_cpu_max_rpm <= 20000 &&
		   loaded.learned_gpu_max_rpm <= 20000) {
		fan_learned_cpu_max = loaded.learned_cpu_max_rpm;
		fan_learned_gpu_max = loaded.learned_gpu_max_rpm;
		pr_info("max fan RPM taken from saved fan state: cpu %d gpu %d\n",
			fan_learned_cpu_max, fan_learned_gpu_max);
		fan_max_rpm_store();
		flush_work(&fan_max_rpm_work);
	} else {
		fan_rpm_learning = true;
		ret = fan_learn_max_rpm();
		fan_rpm_learning = false;
		if (!ret) {
			fan_max_rpm_store();
			flush_work(&fan_max_rpm_work);
		} else if (ret == -EIO) {
			pr_info("max fan RPM not learned yet; will retry\n");
			fan_max_rpm_probe_tries = 1;
			schedule_delayed_work(&fan_max_rpm_probe_work, 30 * HZ);
		} else {
			pr_info("max fan RPM not learned (%d); percent maps across the BIOS table\n",
				ret);
		}
	}

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
	cancel_delayed_work_sync(&fan_max_rpm_probe_work);
	flush_work(&fan_persist_work);
	cancel_work_sync(&fan_persist_work);
	flush_work(&fan_max_rpm_work);
	cancel_work_sync(&fan_max_rpm_work);

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
	cpu_curve.npoints = 0;
	cpu_curve.user_saved = false;
	cpu_curve.last_pct = -1;
	gpu_curve.npoints = 0;
	gpu_curve.user_saved = false;
	gpu_curve.last_pct = -1;
	curve_last_spd_valid = false;
	curve_tz_name[0] = '\0';
	gpu_tz_name[0] = '\0';
#if IS_ENABLED(CONFIG_THERMAL)
	curve_tz = NULL;
	gpu_tz = NULL;
#endif
	power_limits_known = false;
	power_pl1 = HP_POWER_LIMIT_DEFAULT;
	power_pl2 = HP_POWER_LIMIT_DEFAULT;
	strscpy(thermal_profile_name, "normal", sizeof(thermal_profile_name));
#if IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)
	active_platform_profile = PLATFORM_PROFILE_BALANCED;
#endif
}
