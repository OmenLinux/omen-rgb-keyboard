#!/bin/bash

# Installation script for HP OMEN RGB Keyboard udev rules
# This allows non-root users to control RGB keyboard settings

set -e

echo "Installing HP OMEN RGB Keyboard udev rules..."

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# Get the actual user (in case of sudo)
if [ -n "$SUDO_USER" ]; then
    REAL_USER="$SUDO_USER"
else
    REAL_USER="$USER"
fi

# Install udev rules
echo "Installing udev rules to /etc/udev/rules.d/..."
cp 99-omen-rgb-keyboard.rules /etc/udev/rules.d/
chmod 644 /etc/udev/rules.d/99-omen-rgb-keyboard.rules

# Reload udev rules
echo "Reloading udev rules..."
udevadm control --reload-rules
udevadm trigger

# Add user to input group if not already a member
if ! groups "$REAL_USER" | grep -q "\binput\b"; then
    echo "Adding user '$REAL_USER' to 'input' group..."
    usermod -aG input "$REAL_USER"
    USER_ADDED_TO_GROUP=1
else
    echo "User '$REAL_USER' is already a member of 'input' group"
    USER_ADDED_TO_GROUP=0
fi

echo ""
echo "✓ udev rules installed successfully!"
echo ""

# Print usage instructions
if [ $USER_ADDED_TO_GROUP -eq 1 ]; then
    echo "IMPORTANT: You've been added to the 'input' group."
    echo "You need to log out and log back in for the changes to take effect."
    echo ""
    echo "Alternatively, you can use this command to activate the group in the current shell:"
    echo "  newgrp input"
    echo ""
fi

echo "After logging back in (or using 'newgrp input'), you can control the RGB keyboard without sudo:"
echo ""
echo "Examples:"
echo "  echo 'rainbow' | tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode"
echo "  echo '5' | tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed"
echo "  echo 'FF0000' | tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all"
echo "  echo '75' | tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness"
echo ""

