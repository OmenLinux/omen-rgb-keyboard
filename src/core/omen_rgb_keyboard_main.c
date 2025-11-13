// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN FourZone RGB Keyboard Driver - Main Module
 *
 * A clean, lightweight Linux kernel driver for HP OMEN laptop RGB keyboard lighting.
 * Provides full control over 4-zone RGB lighting with brightness control.
 *
 * Author: alessandromrc
 * Based on reverse engineering of HP's Windows WMI interface
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/wmi.h>
#include <generated/utsrelease.h>

#include "omen_rgb_keyboard.h"
#include "omen_wmi.h"
#include "omen_zones.h"
#include "omen_animations.h"
#include "omen_state.h"
#include "omen_hda_led.h"

MODULE_AUTHOR("alessandromrc");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

static struct platform_device *hp_wmi_platform_dev;

static int __init hp_wmi_bios_setup(struct platform_device *device)
{
	int ret;
	
	/* Initialize animation system */
	animation_init();
	
	/* Load saved state */
	load_animation_state();
	
	/* Setup zones and create sysfs attributes */
	ret = fourzone_setup(device);
	if (ret) {
		animation_cleanup();
		return ret;
	}
	
	/* Setup input device for Omen key handling */
	ret = hp_wmi_input_setup();
	if (ret) {
		pr_warn("Failed to setup input device: %d\n", ret);
	}
	
	/* Initialize HDA LED control for mute button */
	ret = omen_hda_led_init();
	if (ret) {
		pr_warn("Failed to initialize HDA LED control: %d\n", ret);
		/* Non-fatal, continue anyway */
	}
	
	/* Start animation if not static */
	if (animation_get_mode() != ANIMATION_STATIC) {
		animation_start();
	}
	
	return 0;
}

static struct platform_driver hp_wmi_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.remove = NULL,
};

static int __init hp_wmi_init(void)
{
	int bios_capable = wmi_has_guid(HPWMI_BIOS_GUID);
	int err;
	
	/* Print driver info */
	pr_info("== HP OMEN RGB Keyboard Driver v%s (kernel %s) by alessandromrc ==\n", 
		DRIVER_VERSION, UTS_RELEASE);
	
	if (!bios_capable) {
		pr_err("HP WMI BIOS GUID %s not found, driver not loaded\n", HPWMI_BIOS_GUID);
		return -ENODEV;
	}

	hp_wmi_platform_dev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(hp_wmi_platform_dev)) {
		pr_err("failed to register platform device\n");
		return PTR_ERR(hp_wmi_platform_dev);
	}

	err = platform_driver_probe(&hp_wmi_driver, hp_wmi_bios_setup);
	if (err) {
		pr_err("platform_driver_probe failed with %d\n", err);
		platform_device_unregister(hp_wmi_platform_dev);
		return err;
	}
	
	pr_info("Driver loaded successfully (RGB zones, animations, mute LED sync enabled)\n");
	return 0;
}
module_init(hp_wmi_init);

static void __exit hp_wmi_exit(void)
{
	/* Cleanup HDA LED control */
	omen_hda_led_cleanup();
	
	/* Cleanup input device */
	hp_wmi_input_cleanup();
	
	/* Stop animations and cleanup */
	animation_cleanup();
	
	/* Cleanup zones */
	fourzone_cleanup();
	
	if (hp_wmi_platform_dev) {
		platform_device_unregister(hp_wmi_platform_dev);
		platform_driver_unregister(&hp_wmi_driver);
	}
	
	pr_info("Driver unloaded\n");
}
module_exit(hp_wmi_exit);
