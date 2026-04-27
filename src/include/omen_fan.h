// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - Fan control (sysfs)
 *
 * Author: alessandromrc
 */

#ifndef OMEN_FAN_H
#define OMEN_FAN_H

struct platform_device;

int omen_fan_setup(struct platform_device *pdev);
void omen_fan_cleanup(void);

#endif /* OMEN_FAN_H */
