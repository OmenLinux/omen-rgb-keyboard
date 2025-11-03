// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - WMI Communication
 *
 * WMI/BIOS communication layer for HP OMEN laptops
 *
 * Author: alessandromrc
 */

#ifndef OMEN_WMI_H
#define OMEN_WMI_H

#include <linux/types.h>

#define HPWMI_BIOS_GUID "5FB7F034-2C63-45e9-BE91-3D44E2C707E4"

enum hp_wmi_commandtype {
	HPWMI_GET_PLATFORM_INFO = 1,
	HPWMI_FOURZONE_COLOR_GET = 2,
	HPWMI_FOURZONE_COLOR_SET = 3,
	HPWMI_STATUS = 4,
	HPWMI_SET_BRIGHTNESS = 5,
	HPWMI_SET_LIGHTBAR_COLORS = 11,
};

enum hp_wmi_command {
	HPWMI_READ = 0x01,
	HPWMI_WRITE = 0x02,
	HPWMI_FOURZONE = 0x020009, /* Main lighting command */
	HPWMI_GAMING = 0x020008,   /* Gaming command */
};

struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[128];
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE = 0x02,
	HPWMI_RET_UNKNOWN_COMMAND = 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE = 0x04,
	HPWMI_RET_INVALID_PARAMETERS = 0x05,
};

/**
 * hp_wmi_perform_query - Execute WMI query to HP BIOS
 * @query: Query type (hp_wmi_commandtype)
 * @command: Command type (hp_wmi_command)
 * @buffer: Input/output buffer
 * @insize: Size of input data
 * @outsize: Expected output size
 *
 * Returns: 0 on success, error code otherwise
 */
int hp_wmi_perform_query(int query, enum hp_wmi_command command,
			 void *buffer, int insize, int outsize);

#endif /* OMEN_WMI_H */

