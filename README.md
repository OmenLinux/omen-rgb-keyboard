# HP OMEN RGB Keyboard Driver

Linux kernel driver for HP OMEN laptop RGB keyboard lighting. Controls 4-zone RGB lighting with brightness control.

Inspired by the original [hp-omen-linux-module](https://github.com/pelrun/hp-omen-linux-module) by James Churchill (@pelrun).

> [!TIP]  
> Need help or want to chat? Join our **official Discord server** for support, questions, or to show off your RGB setups:  
> **[➡️ Click here to join Discord](https://discord.gg/8UwyAJ7sBH)**  

## Features

- 4-Zone RGB Control - Individual control over each keyboard zone
- All-Zone Control - Set all zones to the same color at once
- Brightness Control - Adjust brightness from 0-100%
- **10 Animation Modes** - Complete animation system with CPU-efficient timer-based updates
- **Omen Key Support** - The Omen key is mapped to KEY_MSDOS for custom shortcuts
- **Mute Button LED Control** - Control the mute button LED via HDA verb commands
- Real-time Updates - Changes apply immediately
- Hex Color Format - Use standard RGB hex values

## Supported Hardware

- HP OMEN laptops with 4-zone RGB keyboard lighting
- Tested on Omen 16 u0000sl

## Installation

### Prerequisites
```bash
# Install kernel headers and build tools
sudo pacman -S linux-headers base-devel  # Arch Linux
# for Fedora
sudo dnf install kernel-devel kernel-headers @development-tools dkms
# or
sudo apt install linux-headers-$(uname -r) build-essential  # Ubuntu/Debian
```

> [!IMPORTANT]
> We've noticed issues when using this driver at the same time with *`hp_wmi`* loaded. Since we implement the same things but in an improved way, there's no reason to keep both fighting each other for WMI events (which **will** cause issues)
> ```bash
> sudo modprobe -r hp_wmi
> echo "blacklist hp_wmi" | sudo tee /etc/modprobe.d/blacklist-hp.conf
> #regenerate your initramfs (dependent on your distro)
> ```

### Build and Install
```bash
# Clone the repository
git clone https://github.com/alessandromrc/omen-rgb-keyboard.git
cd omen-rgb-keyboard

# Build and install
sudo make install
```

The module will be built and installed using DKMS, which will automatically rebuild it on kernel updates.

### Automatic Loading on Boot
The driver is configured to load automatically on boot. If you need to set this up manually:

```bash
# Create modprobe configuration (for module options)
sudo cp omen_rgb_keyboard.conf /etc/modprobe.d/

# Create systemd module loading configuration
echo "omen_rgb_keyboard" | sudo tee /etc/modules-load.d/omen_rgb_keyboard.conf

# Create state directory
sudo mkdir -p /var/lib/omen-rgb-keyboard
```

Alternatively, use the provided installation script:
```bash
sudo ./install.sh
```

### Non-Root Access (Optional)

By default, controlling the RGB keyboard requires sudo privileges. To allow your user to control the RGB keyboard without sudo:

```bash
# Install udev rules and configure user permissions
sudo ./install-udev-rules.sh
```

This script will:
- Install udev rules that grant access to users in the 'input' group
- Add your user to the 'input' group automatically
- Reload udev rules to apply changes immediately

**Important:** After running this script, you need to log out and log back in (or run `newgrp input`) for the group membership to take effect.

After installation, you can control the keyboard without sudo:
```bash
# No sudo needed!
echo "rainbow" | tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "5" | tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed
```

## Usage

### Loading the Module
```bash
# Load the module
sudo modprobe omen_rgb_keyboard

# Check if it loaded successfully
lsmod | grep omen_rgb_keyboard
```

### Controlling RGB Lighting

The driver creates sysfs attributes in `/sys/devices/platform/omen-rgb-keyboard/rgb_zones/`:

#### Individual Zone Control
```bash
# Set zone 0 to red
echo "FF0000" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone00

# Set zone 1 to green  
echo "00FF00" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone01

# Set zone 2 to blue
echo "0000FF" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone02

# Set zone 3 to purple
echo "FF00FF" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone03
```

#### All-Zone Control
```bash
# Set all zones to the same color
echo "FFFFFF" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all
```

#### Brightness Control
```bash
# Set brightness to 50%
echo "50" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness

# Set brightness to 100% (maximum)
echo "100" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness

# Turn off lighting (0% brightness)
echo "0" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness
```

#### Mute Button LED Control
```bash
# Turn mute button LED on
echo "1" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_led

# Turn mute button LED off
echo "0" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_led
```

See [HDA_LED_CONTROL.md](HDA_LED_CONTROL.md) for detailed information about mute LED control.

#### Reading Current Values
```bash
# Check current brightness
cat /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness

# Check current animation mode
cat /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode

# Check current animation speed
cat /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Check current zone colors
cat /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone00
cat /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone01
# etc...
```

### Color Format

Colors are specified in RGB hex format:
- `FF0000` = Red
- `00FF00` = Green  
- `0000FF` = Blue
- `FFFFFF` = White
- `000000` = Black (off)

### Brightness Range

Brightness is specified as a percentage (0-100):
- `0` = Completely off
- `50` = 50% brightness
- `100` = Maximum brightness

### Animation Modes

The driver supports 10 different animation modes:

**Basic Modes:**
- **static** - No animation, static colors (default)
- **breathing** - Smooth breathing effect that fades in and out
- **rainbow** - Rainbow wave that cycles through all colors
- **wave** - Wave effect that moves across the zones
- **pulse** - Pulsing effect with varying intensity

**Advanced Modes:**
- **chase** - Lights follow each other in sequence across zones
- **sparkle** - Random sparkle effect with bright white flashes
- **candle** - Warm flickering candle effect with orange/red colors
- **aurora** - Aurora borealis effect with flowing green/blue waves
- **disco** - Disco strobe effect with bright multi-colored flashes

### Animation Speed

Animation speed is controlled by a value from 1-10:
- `1` = Slowest animation
- `5` = Default speed
- `10` = Fastest animation

## Examples

### Gaming Setup
```bash
# Red gaming theme
echo "FF0000" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all
echo "75" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness
```

### Rainbow Effect
```bash
echo "FF0000" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone00  # Red
echo "FF8000" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone01  # Orange
echo "FFFF00" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone02  # Yellow
echo "00FF00" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/zone03  # Green
```

### Subtle White Lighting
```bash
echo "FFFFFF" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all
echo "25" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness
```

### Animation Examples
```bash
# Breathing red effect
echo "FF0000" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all
echo "breathing" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "3" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Rainbow wave
echo "rainbow" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "5" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Chase effect
echo "00FF00" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all
echo "chase" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "4" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Sparkle effect
echo "FFFFFF" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/all
echo "sparkle" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "2" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Aurora effect (uses its own colors)
echo "aurora" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "3" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Disco strobe effect
echo "disco" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "6" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed

# Candle effect (uses its own warm colors)
echo "candle" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "4" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed
```

## Omen Key Mapping

The driver intercepts the Omen key press and maps it to `KEY_MSDOS`, allowing you to bind custom shortcuts to it.

### Setting Up Shortcuts

**GNOME:**
1. Open Settings → Keyboard → Keyboard Shortcuts
2. Click "+" to add a custom shortcut
3. Press the Omen key when prompted
4. Assign your desired action

**KDE Plasma:**
1. System Settings → Shortcuts → Custom Shortcuts
2. Edit → New → Global Shortcut → Command/URL
3. Set the trigger to the Omen key
4. Assign your command

**i3/Sway:**
Add to your config file:
```
bindsym XF86DOS exec your-command-here
```

### Customizing the Key Mapping

If you want to map the Omen key to a different key, edit `src/wmi/omen_wmi.c`:

```c
static const struct key_entry hp_wmi_keymap[] = {
    { KE_KEY, OMEN_KEY_SCANCODE, { KEY_MSDOS } },  // Change KEY_MSDOS to your preferred key
    { KE_END, 0 }
};
```

After changing, rebuild with `sudo make install`.

## Troubleshooting

### Module Not Loading
```bash
# Check if WMI is supported
sudo dmesg | grep -i wmi

# Check for errors
sudo dmesg | grep -i omen_rgb_keyboard
```

### No RGB Zones Found
```bash
# Verify the module loaded
lsmod | grep omen_rgb_keyboard

# Check sysfs path
ls -la /sys/devices/platform/omen-rgb-keyboard/rgb_zones/
```

### Colors Not Changing
- Ensure you're using the correct hex format (6 characters, uppercase)
- Check that brightness is not set to 0
- Verify the module loaded without errors

## Technical Details

- Driver Name: `omen-rgb-keyboard`
- WMI Interface: Uses HP's native WMI commands for maximum compatibility
- Buffer Layout: Matches HP's Windows implementation exactly
- Animation System: CPU-efficient timer-based updates with 20 FPS
- State Persistence: Saves settings to `/var/lib/omen-rgb-keyboard/state`
- Kernel Compatibility: Linux 5.0+

## License

GPL-3.0

## Contributing

Feel free to submit issues and pull requests.

## Disclaimer

This driver is provided as-is, use at your own risk. The author is not responsible for any damage to your hardware.
