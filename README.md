# Elgato 4K60 Pro MK.2 (1cfa:000e) and Elgato 4K Pro (1cfa:0012) Linux Driver

[![Kernel Compatibility](https://img.shields.io/badge/Kernel-6.12%20--%207.0%2B-blueviolet)](https://github.com/Nakildias/sc0710)
[![AUR version](https://img.shields.io/aur/version/sc0710-dkms-git?logo=arch-linux)](https://aur.archlinux.org/packages/sc0710-dkms-git)
[![Status](https://img.shields.io/badge/Status-Maintained-success)](#)
[![GitHub last commit](https://img.shields.io/github/last-commit/Nakildias/sc0710)](https://github.com/Nakildias/sc0710/commits/main)

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/Nakildias/sc0710/blob/main/LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/Nakildias/sc0710?style=flat)](https://github.com/Nakildias/sc0710/stargazers)
[![GitHub issues](https://img.shields.io/github/issues/Nakildias/sc0710)](https://github.com/Nakildias/sc0710/issues)

High-performance, multi-client Linux driver for the Elgato 4K60 Pro MK.2 and Elgato 4K Pro PCI-e capture cards. Engineered for stability on modern kernels (6.12 through 7.0+).

*For older kernels, use the original [stoth68000/sc0710](https://github.com/stoth68000/sc0710) repository (MK.2 only).*

## Supported cards

| Card | Subsystem ID | Notes |
|------|-------------|-------|
| **Elgato 4k60 Pro MK.2** | `1cfa:000e` | Plug-and-play after driver load. No FPGA firmware step. |
| **Elgato 4K Pro** | `1cfa:0012` | Requires **ECP5 FPGA firmware** programming on every **cold boot**. Handled automatically by the installer and the AUR package; manual `insmod` builds need extra steps (see below). |

Both cards share the same `12ab:0710` Magewell chipset but use different board profiles in the driver.

## Kernel compatibility

| Distribution | Status | Notes |
|--------------|--------|-------|
| **Arch Linux** | Stable | DKMS via AUR (`sc0710-dkms-git`) or manual build. Includes `sc0710-cli` and 4K Pro firmware helpers (see below). |
| **Fedora / RHEL** | Stable | DKMS via the automatic installer. |
| **Debian / Ubuntu** | Stable | **Warning:** distro OBS packages may crash; prefer Flatpak OBS. |
| **Fedora Atomic** (Bazzite, Bluefin, Aurora, Silverblue) | Stable | Boot-time build service at `/var/lib/sc0710`. No DKMS. 4K Pro ECP5 stack included. |
| **NixOS** | Supported | Flake module available; firmware stack is simpler than the bash installer (see below). |

Tested on kernel **6.12 through 7.0+**. Newer kernels may work but are not guaranteed until tested.

## Installation

### Automatic installation (recommended)

Unified installer — auto-detects atomic vs standard distros. Supported on Arch, Debian/Ubuntu, Fedora, and Fedora Atomic (Bazzite, Silverblue, Bluefin, Aurora).

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/scripts/install-sc0710.sh)"
```

**Standard distros** get DKMS (optional) or a manual module build, `sc0710-cli`, and boot-time module loading via `modules-load.d`.

**Atomic distros** get:
- Driver source at `/var/lib/sc0710/` (persists across `rpm-ostree` updates)
- `sc0710-build.service` — rebuilds and loads the module on each boot
- `sc0710-cli` with atomic-specific commands (`--rebuild`, etc.)

**4K Pro on any distro** additionally gets:
- Automatic download/extraction of `SC0710.FWI.HEX` from the Elgato Windows driver
- `sc0710-firmware.service` — prepares firmware files and symlinks before driver load
- `sc0710-firmware-verify.service` — verifies ECP5 programming after the module loads, with automatic retries

### 4K Pro ECP5 firmware (cold boot)

The 4K Pro has a Lattice ECP5 FPGA whose configuration is **volatile** — it is lost on full power-off / cold boot. Windows programs it on every boot; this driver does the same.

**Symptoms when ECP5 is not programmed:**
- HDMI input shows as locked/detected
- Video is black
- `dmesg` has no `ECP5 FPGA programmed` message

**After automatic install**, boot services handle programming. If video is still black:

```bash
sudo sc0710-cli --restart
# or
sudo bash /var/lib/sc0710/sc0710-firmware.sh --verify          # atomic
sudo bash /usr/local/libexec/sc0710-firmware.sh --verify     # standard
```

Existing atomic installs can pick up the latest firmware stack with:

```bash
sudo sc0710-cli --update
```

### Arch Linux (AUR)

```bash
yay -S sc0710-dkms-git
```

The AUR package installs:

- **DKMS kernel module** — rebuilds automatically on kernel updates
- **`sc0710-cli`** at `/usr/bin/sc0710-cli` — same management tool as the automatic installer
- **4K Pro firmware helpers** under `/usr/lib/sc0710/` (`extract-firmware.sh`, `sc0710-firmware.sh`, `sc0710-firmware-lib.sh`)

**MK.2 users** can load with `sudo modprobe sc0710` (or enable `/etc/modules-load.d/sc0710.conf` for boot load) and manage the driver with `sc0710-cli`.

**4K Pro users** — if a 4K Pro card (`1cfa:0012`) is present at install/upgrade time, the package hook automatically:

1. Extracts `SC0710.FWI.HEX` when it is not already on disk (needs network; installs `p7zip` if required)
2. Creates and enables `sc0710-firmware.service` and `sc0710-firmware-verify.service`
3. Configures boot module loading via `modules-load.d` and `modprobe.d` softdeps
4. Starts the firmware services and loads the driver

After a **cold boot**, the same systemd units handle ECP5 programming before and after module load. If video is black:

```bash
sudo sc0710-cli --restart
# or
sudo bash /usr/lib/sc0710/sc0710-firmware.sh --verify
```

If the card was added **after** the AUR install, run `sudo sc0710-cli --update` to refresh firmware services, or extract firmware manually:

```bash
sudo /usr/lib/sc0710/extract-firmware.sh
sudo sc0710-cli --restart
```

**Removing the AUR package** — `yay -R sc0710-dkms-git` stops, disables, and deletes `sc0710-firmware.service` and `sc0710-firmware-verify.service` when the installed package includes the current install hook. If you are on an older AUR build, upgrade once first so the hook is present. Firmware files under `/lib/firmware/sc0710/` are not removed by `yay -R`; use `sc0710-cli --remove` for a full cleanup.

**4K Pro on Arch** — the maintainer primarily tests on MK.2 hardware. Cold-boot ECP5 reports from 4K Pro users are especially welcome ([open an issue](https://github.com/Nakildias/sc0710/issues) with `sc0710-cli --dump`).

### NixOS (flakes)

Add `github:Nakildias/sc0710` as a flake input and import `sc0710.nixosModules.default`:

```nix
{
  inputs = {
    # ...
    sc0710.url = "github:Nakildias/sc0710";
  };

  outputs = { self, nixpkgs, sc0710 }: {
    nixosConfigurations.<your-hostname> = nixpkgs.lib.nixosSystem {
      modules = [
        # ...
        sc0710.nixosModules.default
      ];
    };
  };
}
```

Host configuration:

```nix
hardware.sc0710.enable = true;          # kernel module + sc0710-cli
hardware.sc0710.enableFirmware = true;  # basic firmware oneshot service
# hardware.sc0710.kernel = pkgs.linuxPackages_6_19.kernel;  # optional override
```

**Note:** The NixOS module builds the driver and installs `sc0710-cli`, but its firmware service is a single oneshot unit — it does not include the full ECP5 verify/retry stack that the bash installer provides for atomic distros. **4K Pro users on NixOS** may need manual intervention after cold boot until the Nix module is brought up to parity.

### Manual compilation

For development or unsupported distros.

1. **Install dependencies**
   - **Arch:** `sudo pacman -S base-devel linux-headers git`
   - **Debian/Ubuntu:** `sudo apt install build-essential linux-headers-$(uname -r) git`
   - **Fedora/RHEL:** `sudo dnf install kernel-modules-$(uname -r) kernel-devel kernel-headers gcc make git`

2. **Build and load**
   ```bash
   git clone https://github.com/Nakildias/sc0710
   cd sc0710
   make
   sudo insmod build/sc0710.ko
   ```

3. **4K Pro only** — `insmod` alone is not enough on cold boot. You also need:
   - `SC0710.FWI.HEX` in `/lib/firmware/sc0710/` (extract with `scripts/extract-firmware.sh`)
   - Reload the module after cold boot so the driver can program the ECP5, or use the automatic installer

## Driver management (`sc0710-cli`)

Installed by the automatic installer, the AUR package, and the NixOS module. Provides real-time control on both atomic and standard distros.

| Command | Alias | Description |
|---------|-------|-------------|
| `--status` | `-s` | Module state, card info, signal format, scaler, ECP5 status (4K Pro), DKMS or build service status |
| `--load` | `-l` | Load the module. On 4K Pro, verifies ECP5 programming after load |
| `--unload` | `-u` | Unload the module (stops PipeWire consumers if the module is busy) |
| `--restart` | | Full reload. On 4K Pro, runs ECP5 programming with retries |
| `--debug` | `-d` | Toggle verbose `dmesg` logging |
| `--image-toggle` | `-it` | Toggle No Signal images vs colorbars |
| `--software-scaler` | `-ss` | Toggle software scaler modes (all cards) |
| `--toggle-auto-scalar` | `-as` | Toggle automatic safety scaler |
| `--procedural-timings` | `-pt` | Cycle timing mode: merge → procedural-only → static-only |
| `--update` | `-U` | Pull latest source, rebuild, reload. Refreshes firmware services on 4K Pro |
| `--rebuild` | | *(Atomic only)* Force rebuild the module for the running kernel |
| `--dump` | | Save a debug report to the Desktop (`dump-DD-MM-YYYY.txt`) for GitHub issues |
| `--remove` | `-r`, `-R` | Uninstall driver, CLI, services, config files, and 4K Pro firmware |
| `--version` | `-v` | Show installed driver version |
| `--help` | `-h` | Show all options |

After `sc0710-cli --remove`, verify everything is gone:

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/scripts/check-sc0710-removal.sh)"
```

### Verify complete removal

Same command as above — checks module, DKMS, CLI, systemd units, config files, logs, and 4K Pro firmware. No clone or sudo required. Exits `0` when fully clean.

## Features

* **Multi-client support** — multiple apps (e.g. OBS + Discord) can open the device simultaneously
* **DKMS integration** — automatic rebuilds on kernel updates (standard distros)
* **Atomic / immutable support** — boot-time rebuild via `sc0710-build.service` (Bazzite, Silverblue, etc.)
* **4K Pro ECP5 auto-programming** — firmware extraction, cold-boot programming, verify service, and CLI retries
* **Status images** — storage-efficient No Signal / No Device screens
* **Connection sensing** — distinguishes unplugged cables from signal loss (not 100% reliable)
* **Video formats** — 4K60, 1440p144, 1080p240 (EDID changes require Windows)
* **Mode-switch stability** — DMA resync, restart validation, and watchdog recovery during resolution/refresh changes
* **Safety scaling** — auto-scaler and dynamic-resolution paths reduce crash-prone transitions
* **Timing controls** — runtime modes (`merge`, `procedural-only`, `static-only`) via CLI
* **Debug dumps** — `sc0710-cli --dump` collects distro, kernel, `lspci`, driver version, and service state for issue reports

## Troubleshooting

| Problem | What to try |
|---------|-------------|
| **4K Pro: signal locked, black video** (cold boot) | `sudo sc0710-cli --restart` or `sudo sc0710-cli --update` then reboot |
| **4K Pro: ECP5 warning in `--status`** | `sudo bash …/sc0710-firmware.sh --verify` (atomic: `/var/lib/sc0710/`; AUR: `/usr/lib/sc0710/`; standard installer: `/usr/local/libexec/`) |
| **Module won't unload** (app in use) | `sc0710-cli --unload` stops PipeWire first; close OBS/etc. |
| **Atomic: module not built after kernel update** | `sudo sc0710-cli --rebuild` or check `journalctl -u sc0710-build.service -b` |
| **Driver still present after `--remove`** | Run `sudo sc0710-cli --remove` again, then the [removal check script](#verify-complete-removal) |
| **4K60 tearing / frame shifts** at max bandwidth | Under investigation; try lower resolution/refresh or close extra consumers in the meantime |

For GitHub issues, attach output from `sc0710-cli --dump`.

## Known limitations / roadmap

* **4K60 DMA tearing** — horizontal tears or frame shifts at 4K60 (~995 MB/s) under heavy load; **under active investigation**
* **HDR tonemapping (on hold)** — requires opaque I2C commands to the onboard ARM MCU
* **10-bit pixel format (on hold)** — P010/P016 register map unknown
* **EDID switching (on hold)** — EEPROM write protocol unknown; set EDID in Windows first

## Help wanted

Maintainer **[Nakildias](https://github.com/Nakildias)** only has an **Elgato 4k60 Pro MK.2** available for hands-on testing. That limits how much can be validated on other hardware.

Contributions and testing help are especially welcome for:

* **Elgato 4K Pro** — ECP5 cold-boot behavior, atomic/Bazzite installs, and general capture stability
* **Unsupported cards** — other `12ab:0710` subsystem IDs (e.g. HD60 Pro) that are not yet fully supported
* **Open issues** — bugs that need reproduction on hardware the maintainer does not own (4K60 tearing, edge-case distros, etc.)

If you can test, debug, or submit patches for any of the above, please [open an issue](https://github.com/Nakildias/sc0710/issues) or a pull request. Include `sc0710-cli --dump` output when reporting problems.

## Credits and copyright

This fork is maintained by **[Nakildias](https://github.com/Nakildias)** (`nakildiaspro@gmail.com`).

* Based on original reverse engineering by **[Steven Toth (@stoth68000)](https://github.com/stoth68000)** and subsequent work by **[@Subtixx](https://github.com/Subtixx)**.
* Thanks to **[Onhil (@Onhil)](https://github.com/Onhil)** for work on the Elgato 4K Pro.

The kernel module is a derivative of Steven Toth's sc0710 driver (GPL v2). Original copyright notices are preserved in source files; modifications in this fork are attributed separately. Installer scripts are original work in this repository. See **[COPYRIGHT](COPYRIGHT)** for details.

This project is not affiliated with or endorsed by Elgato, Magewell, or Kernel Labs.
