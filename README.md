# HP OMEN RGB Keyboard Driver

Linux kernel driver for HP OMEN laptop RGB keyboard lighting. Controls 4-zone RGB lighting with brightness control.

Inspired by the original [hp-omen-linux-module](https://github.com/pelrun/hp-omen-linux-module) by James Churchill (@pelrun).

> [!WARNING]
> ## Support Scope
>
> This project is provided **as-is** and is maintained primarily as a community-developed Linux driver for HP OMEN hardware. It is **not an official HP product or driver**, and the author cannot provide individual support for every laptop model, BIOS version, kernel, distribution, firmware configuration, or third-party software combination.
>
> **Hardware and firmware support is ultimately the responsibility of HP.** HP OMEN laptops are designed and supported by HP, while the Windows-specific firmware interfaces, WMI behavior, and proprietary drivers are outside the control of this project. This driver can only work with the interfaces that HP exposes to Linux.
>
> If an issue is caused by:
> - HP firmware or BIOS behavior
> - missing or broken HP WMI functionality
> - an unsupported or undocumented HP hardware interface
> - HP proprietary Windows drivers or software
> - a particular laptop revision that exposes different hardware/firmware behavior
>
> it may **not be possible for this project to fix it**. In such cases, please report the underlying hardware/firmware compatibility problem to HP as well.
>
> Before opening an issue, please:
> 1. Make sure you are running the latest version of this driver.
> 2. Check whether your exact laptop model is listed under [Supported Hardware](#supported-hardware).
> 3. Check the existing issues for similar reports.
> 4. Follow the troubleshooting steps below.
> 5. Include your kernel version, distribution, laptop model, BIOS version, relevant `dmesg` output, and exact steps to reproduce the problem.
>
> **Issues without sufficient information to reproduce or investigate the problem may be closed without further investigation.**
>
> Please do not open an issue expecting the maintainer to diagnose your entire Linux installation, hardware configuration, or HP firmware. This project is a driver, **not a general Linux or HP support service**.

> [!TIP]
> Need help or want to chat? Join our **official Discord server** for support, questions, or to show off your RGB setups:
> **[➡️ Click here to join Discord](https://discord.gg/8UwyAJ7sBH)**

## Features

- 4-Zone RGB Control - Individual control over each keyboard zone
- All-Zone Control - Set all zones to the same color at once
- Brightness Control - Adjust brightness from 0-100%
- **12 Animation Modes** - Complete animation system with CPU-efficient timer-based updates
- **Omen Key Support** - The Omen key is mapped to KEY_MSDOS for custom shortcuts
- **Mute Button LED Control** - Automatic LED sync with system mute state via HDA codec
- **Fan control** - EC-automatic by default, optional custom curve override (persisted across reboot), max fan, hysteresis, auto max under Performance, thermal presets (silent / normal / performance), CPU PL1/PL2, and PPD via `platform_profile` (see below)
- Real-time Updates - Changes apply immediately
- Hex Color Format - Use standard RGB hex values

## Supported Hardware

- HP OMEN laptops with 4-zone RGB keyboard lighting

| Model Series        | Status            |
|---------------------|-------------------|
| Omen 16-u0000sl     | Tested & working  |
| Omen 16-wf1xxx      | Tested & working  |
| Omen 16-wf0xxx      | Tested & working  |
| Omen 15-dh0025nl    | Tested & working  |
| Omen 15-en1004ns    | Tested & working  |
| Omen 16-wd0xxx      | Tested & working  |
| Omen 14-fb0xxx      | Tested & working  |
| Omen 15-ek0xxx      | Tested & working  |

## Installation

### Prerequisites

```bash
# Arch Linux
sudo pacman -S linux-headers base-devel alsa-lib

# Fedora
sudo dnf install kernel-devel kernel-headers @development-tools dkms alsa-lib-devel

# Ubuntu/Debian
sudo apt install linux-headers-$(uname -r) build-essential libasound2t64
```

**Note**: The ALSA libraries are required for the mute button LED control feature. The driver will still compile without them, but LED sync functionality will be disabled.

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

> [!NOTE]
> If you need to manually interact with DKMS, remember that the DKMS module name uses hyphens (`omen-rgb-keyboard`), while the loaded kernel module uses underscores (`omen_rgb_keyboard`).

### Automatic Loading on Boot
The driver is configured to load automatically on boot. If you need to set this up manually:

```bash
# Create modprobe configuration (for module options)
sudo cp omen_rgb_keyboard.conf /etc/modprobe.d/

# Create systemd module loading configuration
echo "omen_rgb_keyboard" | sudo tee /etc/modules-load.d/omen_rgb_keyboard.conf

# Create state directory (RGB + fan persistence)
sudo mkdir -p /var/lib/omen-rgb-keyboard
```

`install.sh` also makes that directory group-`input` writable and creates the `state` / `fan_state` files. Prefer the script when possible:

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

The mute button LED is **automatically synchronized** with your system's mute state (polls every 200ms). When you mute audio, the LED turns on; when unmuted, it turns off.

**For HDA/ALSA audio devices:**
The driver automatically monitors ALSA controls (Master, Speaker, Headphone, PCM) and syncs the LED.

**For PipeWire/Bluetooth devices:**
If you're using PipeWire or Bluetooth headphones, the mute monitor service is installed and enabled automatically.

**Check service status:**
```bash
# Check if service is running
systemctl --user status omen-mute-monitor.service

# View service logs
journalctl --user -u omen-mute-monitor.service -f

# Check if service is enabled
systemctl --user is-enabled omen-mute-monitor.service

# Restart service if needed
systemctl --user restart omen-mute-monitor.service
```

The service monitors PipeWire mute state using `wpctl` and notifies the kernel driver via sysfs. It runs as your user (for PipeWire access) and uses sudo only for sysfs writes if needed.

**Manual Control:**
```bash
# Manually turn mute button LED on (disables auto-sync)
echo "1" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_led

# Manually turn mute button LED off
echo "0" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_led

# Set mute state from userspace (for PipeWire/Bluetooth)
echo "1" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state  # Muted
echo "0" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/mute_state  # Unmuted
```

**Note**: Manual control via `mute_led` disables automatic synchronization until the driver is reloaded.

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

### Fan control (`/sys/devices/platform/omen-rgb-keyboard/fan/`)

Requires the same HP WMI GUID as RGB; **blacklist `hp_wmi`** if both would load (see above). Custom fan curves need Victus-S WMI, a valid BIOS fan table, and **`CONFIG_THERMAL`** so a thermal zone can be read. **`max_fan` works on all supported fan interfaces.**

By default the **EC owns the fans** (automatic). `fan_curve` shows a reference table for the active thermal preset; writing your own setpoints **overrides** the EC. Mode and custom points are saved under `/var/lib/omen-rgb-keyboard/fan_state` and restored after reboot.

| File | Purpose |
|------|---------|
| `cpu_fan_rpm`, `gpu_fan_rpm` | Read-only RPM (where supported) |
| `max_fan` | `0` / `1` — max fans off/on (always available). Manual `1` is sticky until you write `0` |
| `thermal_profile` | Write `silent`, `normal`, or `performance` (WMI preset). Also applies GPU CTGP/PPAB and re-actualizes PL1/PL2 defaults. Does **not** start a software curve or wipe saved customs |
| `power_limits` | CPU PL1/PL2 in watts (`PL1 PL2`), or `default` / `0 0` to restore BIOS defaults. PL2 must be ≥ PL1. Read shows last written values (firmware has no get) |
| `fan_curve` | Read the active/reference points; write `temp:pct` pairs to save a custom curve and override the EC; write `default` or `ec` to return control to the EC (customs kept) |
| `fan_curve_enable` | `0` returns to EC automatic (keeps saved customs); `1` re-applies the saved custom curve |
| `fan_temp_zone` | Optional thermal zone name (e.g. `x86_pkg_temp`); if unset, the driver tries common zone names |

**Hysteresis:** while a custom curve is applied, fan percent uses a 3°C band so speeds do not chatter around a setpoint. WMI speed is only rewritten when the mapped speed byte changes.

**Auto max (Performance):** when `thermal_profile` / PPD is `performance` and curve demand reaches ≥ **90%**, the driver engages max fan automatically; it releases again at ≤ **80%** (or when leaving Performance). A manual `max_fan=1` is never cleared by this path.

While `thermal_profile` is `performance`, AC plug/unplug re-sends PL1/PL2 defaults so the EC picks the correct wattage for the new power source (same fix as mainline `hp-wmi`).

```bash
F=/sys/devices/platform/omen-rgb-keyboard/fan
echo performance | tee "$F/thermal_profile"
echo "65 65" | tee "$F/power_limits"   # custom PL1/PL2 (watts)
echo default | tee "$F/power_limits"    # back to BIOS defaults for the active mode
```

#### Power Profiles Daemon (PPD)

The driver registers an ACPI `platform_profile` handler (same interface mainline `hp-wmi` uses), so **power-profiles-daemon**, GNOME/KDE power controls, and `powerprofilesctl` drive the Omen thermal modes:

| PPD profile | Kernel `platform_profile` | Driver thermal mode |
|-------------|---------------------------|---------------------|
| `power-saver` | `low-power` | `silent` |
| `balanced` | `balanced` | `normal` |
| `performance` | `performance` | `performance` |

```bash
# Via PPD (D-Bus / desktop UI)
powerprofilesctl set performance

# Or directly via the kernel ABI PPD watches
echo performance | sudo tee /sys/firmware/acpi/platform_profile
cat /sys/firmware/acpi/platform_profile_choices
```

Writing `fan/thermal_profile` keeps PPD in sync (and the reverse). Keep `hp_wmi` blacklisted so only this handler owns the profile.

With udev rules installed, users in the `input` group can use `tee` on these files without `sudo`, same as RGB.

#### Fan curve format

Curves are **space-separated** pairs:

**`temperature_C:fan_percent`**

- **Temperature:** degrees Celsius from the selected thermal zone (typically CPU package).
- **Fan percent:** `0`–`100`, mapped by the driver into the EC’s allowed fan range using the BIOS fan table.

You need **at least 2** points and may use **up to 8** points per curve. Order does not matter; the driver sorts by temperature. Between two adjacent points the driver **linearly interpolates** fan percent vs temperature; below the lowest point it clamps to that point’s percent, and above the highest it clamps to the last point’s percent.

```bash
F=/sys/devices/platform/omen-rgb-keyboard/fan

# Override EC with a custom curve (also saved across reboot)
echo "30:20 40:28 50:38 60:50 72:65 82:78 92:90" | tee "$F/fan_curve"

# Hand fans back to the EC — custom points stay in fan_curve
echo 0 | tee "$F/fan_curve_enable"
# or: echo default | tee "$F/fan_curve"

# Re-apply the saved custom curve later
echo 1 | tee "$F/fan_curve_enable"
```

Until you write custom points, `fan_curve` shows the built-in silent / normal / performance reference for the active `thermal_profile` (five points each). Switching profile updates that reference only when you have **not** saved a custom curve; saved customs are never discarded by profile/PPD changes or restore-to-EC.

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

### Preventing the Desktop From Dimming the Keyboard

Desktop environments dim the keyboard backlight when the session goes idle by
writing to the `omen::kbd_backlight` LED class device. GNOME fades it to
`idle-brightness` and back, on battery and on AC alike.

`brightness_lock` makes the driver ignore brightness changes arriving that way:

```bash
# Ignore them from now on
echo 1 | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness_lock

# Back to normal
echo 0 | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness_lock
```

To have it apply from boot, set the module parameter in
`/etc/modprobe.d/omen_rgb_keyboard.conf`:

```
options omen_rgb_keyboard brightness_lock=1
```

While it is set, the desktop's own keyboard-backlight control no longer has any
effect, since the driver cannot tell an idle fade from a deliberate change. Set
the level directly instead:

```bash
echo 80 | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/brightness
```

Enabling the lock while the backlight happens to be off raises it to full
first, because a locked level of zero could not be undone from the desktop:
the lock is exactly what stops the desktop touching the backlight.

### Animation Modes

The driver supports 12 different animation modes:

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
- **gradient** - Custom color cycling with per-zone-group configuration
- **spectrum** - Slow, even hue cycle with the whole keyboard in unison (20s at speed 1)

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

# Spectrum cycle (uses its own colors, slowest speed gives a 20 second cycle)
echo "spectrum" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "1" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed
```

### Gradient Animation
```bash
# Configure gradient: zones 0,1,2 cycle red→green→blue, zone 3 cycles purple→orange
echo "0,1,2:FF0000,00FF00,0000FF;3:800080,FFA500" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/gradient_config

# Start gradient animation at speed 5
echo "gradient" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_mode
echo "5" | sudo tee /sys/devices/platform/omen-rgb-keyboard/rgb_zones/animation_speed
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

### Secure Boot (Key was rejected by service)
If your Linux distribution enforces strict Secure Boot policies, the kernel will block unsigned drivers from loading, throwing an error like: `modprobe: ERROR: could not insert 'omen_rgb_keyboard': Key was rejected by service`.

To resolve this, you must sign the kernel module using a trusted Machine Owner Key (MOK) so your UEFI firmware allows it to load.

* **If you already have existing keys** (you may already have a MOK/key pair, common if you previously set up Secure Boot module signing, e.g. for NVIDIA drivers): Configure DKMS to use your existing private key and public certificate before running `dkms install`.
* **If you do not have a MOK**: You will need to generate a key pair, enroll it in your firmware via `mokutil`, and script DKMS to use it.

For instructions and automated scripts to handle MOK generation and DKMS signing, see the [Community DKMS Signing Guide](https://gist.github.com/sbueringer/bd8cec239c44d66967cf307d808f10c4) or the [Arch Wiki DKMS Documentation](https://wiki.archlinux.org/title/Dynamic_Kernel_Module_Support#Secure_Boot).



## Technical Details

- Driver Name: `omen-rgb-keyboard`
- WMI Interface: Uses HP's native WMI commands for maximum compatibility
- Buffer Layout: Matches HP's Windows implementation exactly
- Animation System: CPU-efficient timer-based updates with 20 FPS
- State Persistence: RGB/animation → `/var/lib/omen-rgb-keyboard/state`; fan mode/curves → `/var/lib/omen-rgb-keyboard/fan_state`
- Kernel Compatibility: Linux 5.0+

## License

GPL-3.0

## Contributing

Feel free to submit issues and pull requests.

## Disclaimer

This driver is provided as-is, use at your own risk. The author is not responsible for any damage to your hardware.
