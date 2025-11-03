// SPDX-License-Identifier: GPL-3
/*
 * HP OMEN RGB Keyboard Driver - WMI Communication
 *
 * WMI/BIOS communication layer for HP OMEN laptops
 *
 * Author: alessandromrc
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/wmi.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>

#include "omen_wmi.h"

#define OMEN_KEY_SCANCODE 0x21a5

static struct input_dev *hp_wmi_input_dev;

static const struct key_entry hp_wmi_keymap[] = {
	{ KE_KEY, OMEN_KEY_SCANCODE, { KEY_MSDOS } },
	{ KE_END, 0 }
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

int hp_wmi_perform_query(int query, enum hp_wmi_command command,
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
	if (obj->type != ACPI_TYPE_BUFFER) {
		ret = -EINVAL;
		goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;
	if (ret) {
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

static void hp_wmi_notify(union acpi_object *obj, void *context)
{
	u32 event_id = 0;
	u32 event_data = 0;

	if (!obj)
		return;

	if (obj->type == ACPI_TYPE_BUFFER) {
		if (obj->buffer.length >= 8) {
			event_id = *((u32 *)obj->buffer.pointer);
			event_data = *((u32 *)(obj->buffer.pointer + 4));
		}
	} else if (obj->type == ACPI_TYPE_INTEGER) {
		event_id = obj->integer.value;
	}

	switch (event_id) {
	case 0x1d:
		/* Omen key press event */
		if (hp_wmi_input_dev) {
			sparse_keymap_report_event(hp_wmi_input_dev, event_data, 1, true);
		}
		break;
	default:
		pr_debug("Unhandled WMI event: 0x%x\n", event_id);
		break;
	}
}

int hp_wmi_input_setup(void)
{
	int err;

	hp_wmi_input_dev = input_allocate_device();
	if (!hp_wmi_input_dev)
		return -ENOMEM;

	hp_wmi_input_dev->name = "HP Omen Keyboard";
	hp_wmi_input_dev->phys = "hpwmi/input0";
	hp_wmi_input_dev->id.bustype = BUS_HOST;
	hp_wmi_input_dev->id.vendor = 0x03f0;
	hp_wmi_input_dev->id.product = 0x0001;
	hp_wmi_input_dev->id.version = 0x0100;

	err = sparse_keymap_setup(hp_wmi_input_dev, hp_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	err = input_register_device(hp_wmi_input_dev);
	if (err)
		goto err_free_dev;

	/* Register WMI event notifier */
	err = wmi_install_notify_handler(HPWMI_EVENT_GUID, hp_wmi_notify, NULL);
	if (ACPI_FAILURE(err)) {
		pr_err("Failed to register WMI event handler\n");
		goto err_unregister_dev;
	}

	pr_info("HP WMI input device registered (Omen key -> MSDOS)\n");
	return 0;

err_unregister_dev:
	input_unregister_device(hp_wmi_input_dev);
	hp_wmi_input_dev = NULL;
	return -ENODEV;

err_free_dev:
	input_free_device(hp_wmi_input_dev);
	hp_wmi_input_dev = NULL;
	return err;
}

void hp_wmi_input_cleanup(void)
{
	if (hp_wmi_input_dev) {
		wmi_remove_notify_handler(HPWMI_EVENT_GUID);
		input_unregister_device(hp_wmi_input_dev);
		hp_wmi_input_dev = NULL;
		pr_info("HP WMI input device unregistered\n");
	}
}

