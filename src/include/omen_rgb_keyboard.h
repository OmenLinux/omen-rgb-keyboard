// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Main Header
 *
 * Common definitions and includes for the HP OMEN RGB keyboard driver
 *
 * Author: alessandromrc
 */

#ifndef OMEN_RGB_KEYBOARD_H
#define OMEN_RGB_KEYBOARD_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/platform_device.h>

/* Module information */
#define DRIVER_VERSION "1.2"
#define DRIVER_NAME "omen-rgb-keyboard"
#define DRIVER_DESC "HP OMEN FourZone RGB Keyboard Lighting Driver"

/* Forward declarations from sub-modules */
struct color_platform;
struct platform_zone;
enum hp_wmi_command;
enum animation_mode;

#endif /* OMEN_RGB_KEYBOARD_H */

