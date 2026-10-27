#!/bin/bash

# Installation script for HP OMEN RGB Keyboard Driver

set -e

echo "Installing HP OMEN RGB Keyboard Driver..."

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# Install DKMS if not present
if ! command -v dkms &> /dev/null; then
    echo "Installing DKMS..."
    if command -v pacman &> /dev/null; then
        pacman -S dkms
    elif command -v apt &> /dev/null; then
        apt update && apt install -y dkms
    elif command -v dnf &> /dev/null; then
        dnf install -y dkms
    else
        echo "Please install DKMS manually for your distribution"
        exit 1
    fi
fi

# Unload hp_wmi and blacklist it from boot
echo "Checking for hp_wmi module..."
if lsmod | grep -q "^hp_wmi"; then
    echo "Unloading hp_wmi module..."
    modprobe -r hp_wmi 2>/dev/null || true
fi

# Install the module
echo "Installing module with DKMS..."
make install

# Create modprobe configuration (for module options)
echo "Creating modprobe configuration..."
cp omen_rgb_keyboard.conf /etc/modprobe.d/

# Create systemd module loading configuration
echo "Creating systemd module loading configuration..."
echo "omen_rgb_keyboard" > /etc/modules-load.d/omen_rgb_keyboard.conf

# Create the state directory (group input can update state files from sysfs)
echo "Creating state directory..."
mkdir -p /var/lib/omen-rgb-keyboard
if getent group input >/dev/null 2>&1; then
    chgrp input /var/lib/omen-rgb-keyboard || true
    chmod 2775 /var/lib/omen-rgb-keyboard
    touch /var/lib/omen-rgb-keyboard/state /var/lib/omen-rgb-keyboard/fan_state
    chgrp input /var/lib/omen-rgb-keyboard/state /var/lib/omen-rgb-keyboard/fan_state || true
    chmod 664 /var/lib/omen-rgb-keyboard/state /var/lib/omen-rgb-keyboard/fan_state
else
    chmod 755 /var/lib/omen-rgb-keyboard
fi

# Load the module immediately
echo "Loading module..."
modprobe omen_rgb_keyboard

# Install mute monitor script and systemd service
echo "Installing mute monitor script..."
if [ -f "scripts/omen-mute-monitor.sh" ]; then
    # Copy script to /usr/local/bin
    cp scripts/omen-mute-monitor.sh /usr/local/bin/omen-mute-monitor
    chmod +x /usr/local/bin/omen-mute-monitor
    echo "  Mute monitor script installed to: /usr/local/bin/omen-mute-monitor"
    
    SYSFS_PATH="/sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state"
    
    # Create systemd user service
    echo "Creating systemd user service..."
    mkdir -p /etc/systemd/user
    cat > /etc/systemd/user/omen-mute-monitor.service << 'EOF'
[Unit]
Description=HP OMEN Mute LED Monitor (PipeWire/Bluetooth)
After=pipewire.service wireplumber.service
Wants=pipewire.service

[Service]
Type=simple
ExecStart=/usr/local/bin/omen-mute-monitor
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
    echo "  Systemd user service created at: /etc/systemd/user/omen-mute-monitor.service"
    
    # Create tmpfiles.d config to set permissions on mute_state sysfs file
    echo "Creating tmpfiles.d configuration for mute_state permissions..."
    mkdir -p /etc/tmpfiles.d
    cat > /etc/tmpfiles.d/omen-mute-monitor.conf << EOF
# Set permissions for mute_state sysfs file
w /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state - - - - 0666
EOF
    # Apply immediately if file exists
    if [ -f "/sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state" ]; then
        chmod 666 /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state 2>/dev/null || true
        echo "  Permissions set on mute_state file"
    fi
    systemd-tmpfiles --create /etc/tmpfiles.d/omen-mute-monitor.conf 2>/dev/null || true
    echo "  tmpfiles.d configuration created (will apply on boot)"
    
    # Enable the service for the user who ran sudo
    REAL_USER="${SUDO_USER:-$USER}"
    if [ -n "$REAL_USER" ] && [ "$REAL_USER" != "root" ]; then
        echo "  Enabling service for user: $REAL_USER"
        # Enable user lingering so services can run without active session
        loginctl enable-linger "$REAL_USER" 2>/dev/null || true
        
        # Get user's runtime directory
        USER_ID=$(id -u "$REAL_USER" 2>/dev/null)
        if [ -n "$USER_ID" ]; then
            RUNTIME_DIR="/run/user/$USER_ID"
            if [ -d "$RUNTIME_DIR" ]; then
                # Use runuser with proper environment
                runuser -l "$REAL_USER" -c "XDG_RUNTIME_DIR=$RUNTIME_DIR systemctl --user daemon-reload" 2>/dev/null || true
                runuser -l "$REAL_USER" -c "XDG_RUNTIME_DIR=$RUNTIME_DIR systemctl --user enable omen-mute-monitor.service" 2>/dev/null || true
                runuser -l "$REAL_USER" -c "XDG_RUNTIME_DIR=$RUNTIME_DIR systemctl --user start omen-mute-monitor.service" 2>/dev/null || true
                echo "  Service enabled and started"
            else
                echo "  Note: User session not active, service will start on next login"
                echo "  Or enable manually: systemctl --user enable --now omen-mute-monitor.service"
            fi
        fi
    else
        echo "  Warning: Could not determine user to enable service for"
        echo "  Enable manually with: systemctl --user enable --now omen-mute-monitor.service"
    fi
else
    echo "Warning: scripts/omen-mute-monitor.sh not found"
fi

# Install udev rules (required for mute monitor service to work without sudo)
echo ""
echo "Installing udev rules for non-root access (required for mute monitor)..."
if [ -f "install-udev-rules.sh" ]; then
    bash install-udev-rules.sh
    
    # Ensure user is in input group for the service
    REAL_USER="${SUDO_USER:-$USER}"
    if [ -n "$REAL_USER" ] && [ "$REAL_USER" != "root" ]; then
        if ! groups "$REAL_USER" | grep -q "\binput\b"; then
            echo "  Adding user '$REAL_USER' to 'input' group..."
            usermod -aG input "$REAL_USER"
            echo "  WARNING: User added to 'input' group. Log out and back in for changes to take effect."
            echo "  Or run: newgrp input (in current session)"
        fi
    fi
    
    # Trigger udev to apply rules immediately
    echo "  Triggering udev to apply rules..."
    udevadm trigger --subsystem-match=platform --attr-match=kernel=omen-rgb-keyboard 2>/dev/null || true
else
    echo "Warning: install-udev-rules.sh not found, skipping udev rules installation"
    echo "  The mute monitor service requires udev rules to work properly"
fi

echo ""
echo "Installation complete!"
echo "The driver will now load automatically on boot."
echo ""
echo "Mute LED Control:"
echo "  - For HDA/ALSA: Works automatically"
echo "  - For PipeWire/Bluetooth: Service installed and enabled automatically"
echo ""
echo "You can control the RGB keyboard using:"
echo "  echo 'rainbow' | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode"
echo "  echo '5' | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed"
echo ""
echo "If you installed udev rules, you can omit 'sudo' after logging out and back in."
echo "See README.md for more examples and controls."
