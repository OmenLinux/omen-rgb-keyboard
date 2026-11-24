#!/bin/bash
# Remind that the out-of-tree HP OMEN keyboard driver must be removed
# before a distribution upgrade.
#
# Installed as /usr/local/sbin/omen-driver-warning and called from
# /etc/apt/apt.conf.d/99omen-driver-warning.
#
# Ordinary "apt update" / "apt upgrade" / "apt full-upgrade" stays silent:
# the message is only printed when do-release-upgrade is somewhere in the
# process tree.

set -u

short=0
force=0

for arg in "$@"; do
    case "$arg" in
    --short) short=1 ;;
    --force) force=1 ;;
    esac
done

upgrade_context() {
    local pid=$$
    local cmd

    while [ "$pid" -gt 1 ]; do
        cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)
        case "$cmd" in
        *do-release-upgrade*)
            return 0
            ;;
        esac
        pid=$(awk '/^PPid:/ { print $2 }' "/proc/$pid/status" 2>/dev/null)
        [ -n "$pid" ] || return 1
    done

    return 1
}

if [ "$force" -eq 0 ] && ! upgrade_context; then
    exit 0
fi

dkms_entries=""

if command -v dkms >/dev/null 2>&1; then
    dkms_entries=$(dkms status 2>/dev/null |
        grep -E '^(omen-rgb-keyboard|hp-omen-wmi|hp-wmi-backlight)/' || true)
fi

if [ -z "$dkms_entries" ] &&
    [ ! -e /etc/modules-load.d/omen_rgb_keyboard.conf ] &&
    [ ! -e /etc/modprobe.d/omen_rgb_keyboard.conf ] &&
    [ ! -e /etc/udev/rules.d/99-omen-rgb-keyboard.rules ]; then
    exit 0
fi

if [ "$short" -eq 1 ]; then
    echo "WARNING: the out-of-tree omen-rgb-keyboard driver is installed. Remove it before upgrading the Ubuntu release (details: /usr/local/sbin/omen-driver-warning)."
    exit 0
fi

echo
echo "=================================================================="
echo " WARNING: the out-of-tree HP OMEN keyboard driver is installed"
echo "=================================================================="

if [ -n "$dkms_entries" ]; then
    echo " DKMS:"
    printf '%s\n' "$dkms_entries" | sed 's/^/   /'
fi

echo
echo " Remove it before upgrading the Ubuntu release (do-release-upgrade):"
echo

if [ -n "$dkms_entries" ]; then
    printf '%s\n' "$dkms_entries" | cut -d, -f1 | sort -u |
        sed 's/^/  sudo dkms remove /; s/$/ --all/'
fi

echo "  sudo rm -f /etc/modules-load.d/omen_rgb_keyboard.conf"
echo "  sudo rm -f /etc/modprobe.d/omen_rgb_keyboard.conf"
echo "  sudo rm -f /etc/udev/rules.d/99-omen-rgb-keyboard.rules"
echo "  sudo rm -f /etc/modprobe.d/blacklist-hp_wmi.conf"
echo "  sudo udevadm control --reload-rules"
echo "  sudo update-initramfs -u"
echo "  sudo reboot"
echo
echo " After the upgrade the driver can be installed again."
echo " The stock hp_wmi will come back, but it does not support 4-zone RGB."
echo "=================================================================="
echo
