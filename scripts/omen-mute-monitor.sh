#!/bin/bash
SYSFS="/sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state"

# Check if we can write (requires udev rules to be installed)
if [ ! -w "$SYSFS" ]; then
	echo "Cannot write to $SYSFS. Install udev rules: sudo ./install-udev-rules.sh" >&2
	exit 1
fi

command -v wpctl >/dev/null || { echo "wpctl not found" >&2; exit 1; }

last=""
first=true
while true; do
	output=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1)
	ret=$?
	if [ $ret -eq 0 ] && [ -n "$output" ]; then
		if echo "$output" | grep -q "\[MUTED\]"; then
			state="1"
		else
			state="0"
		fi
		if [ "$first" = true ] || [ "$state" != "$last" ]; then
			if ! printf '%s\n' "$state" > "$SYSFS"; then
				echo "omen-mute-monitor: failed to write $state to $SYSFS" >&2
			fi
			last="$state"
			first=false
		fi
	fi
	sleep 0.1
done
