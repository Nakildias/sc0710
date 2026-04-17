# Elgato 4K60 Pro MK.2 (1cfa:000e) and Elgato 4K Pro (1cfa:0012) Linux Driver

[![Kernel Compatibility](https://img.shields.io/badge/Kernel-6.12%20--%206.18%2B-blueviolet)](https://github.com/Nakildias/sc0710)
[![AUR version](https://img.shields.io/aur/version/sc0710-dkms-git?logo=arch-linux)](https://aur.archlinux.org/packages/sc0710-dkms-git)
[![Status](https://img.shields.io/badge/Status-Maintained-success)](#)
[![GitHub last commit](https://img.shields.io/github/last-commit/Nakildias/sc0710)](https://github.com/Nakildias/sc0710/commits/main)

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/Nakildias/sc0710/blob/main/LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/Nakildias/sc0710?style=flat)](https://github.com/Nakildias/sc0710/stargazers)
[![GitHub issues](https://img.shields.io/github/issues/Nakildias/sc0710)](https://github.com/Nakildias/sc0710/issues)

High-performance, multi-client Linux driver for the Elgato 4K60 Pro MK.2 and Elgato 4K Pro PCI-e capture card. Engineered for stability on modern kernels (6.12+).
*For older kernels, please use the original [stoth68000/sc0710](https://github.com/stoth68000/sc0710) repository (Only for MK.2).*

## Kernel Compatibility

| Distribution | Kernel Version | Status | Notes |
|--------------|---------------|--------|-------|
| **Arch Linux** | 6.18.7+ | Stable | Standard configuration. |
| **Fedora** | 6.17.12+ | Stable | Standard configuration. |
| **Debian** | 6.12.57+ | Stable | **Warning:** Repository version of OBS may crash. Use Flatpak OBS. |
| **Fedora Atomic** | 6.17.7+ | **Experimental** | Supported on Bazzite, Bluefin, Aurora, Silverblue. |

## Installation

### Automatic Installation (Recommended)
Unified installer that auto-detects your distro (Atomic or standard) and runs the appropriate flow. Supported on Arch Linux, Debian/Ubuntu, Fedora, and Fedora Atomic (Bazzite, Silverblue, Bluefin, Aurora).

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/scripts/install-sc0710.sh)"
```

On Fedora Atomic distros, the installer sets up a boot-time build service instead of DKMS. Both the above URL and `atomic-install-sc0710.sh` work (they use the same unified script).

### Arch Linux (AUR)
Install `sc0710-dkms-git` using your preferred helper. Note that the CLI utility is not currently included in the AUR package.

```bash
yay -S sc0710-dkms-git
```

### NixOS (flakes)

In your system flake, add `github:Nakildias/sc0710` as an input, and import the module `sc0710.nixosModules.default`.
```nix
{
  inputs = {
    # ...
    sc0710.url = "github:Nakildias/sc0710";
  };

  outputs = { self, nixpkgs, sc0710 }: {
    # replace <your-hostname> with your actual hostname
    nixosConfigurations.<your-hostname> = nixpkgs.lib.nixosSystem {
      # ...
      modules = [
        # ...
        sc0710.nixosModules.default
      ];
    };
  };
}
```

In your host configuration, you can use following options:
```nix
# enable the kernel module and `sc0710-cli` script
hardware.sc0710.enable = true;

# enable automatic firmware updates with the systemd service
hardware.sc0710.enableFirmware = true;

# if, for whatever reason, you need to override the kernel version that the module is built for.
# default is your current kernel (what `boot.kernelPackages.kernel` is set to)
#hardware.sc0710.kernel = pkgs.linuxPackages_6_19.kernel;
```

### Manual Compilation
For other distributions or development usage.

1.  **Install Dependencies**
    *   **Arch Linux:** `sudo pacman -S base-devel linux-headers git dkms`
    *   **Debian/Ubuntu:** `apt install build-essential linux-headers-$(uname -r) git dkms`
    *   **Fedora/RHEL:** `dnf install kernel-modules-$(uname -r) kernel-devel kernel-headers gcc make git dkms`
2.  **Build and Load**
    ```bash
    git clone https://github.com/Nakildias/sc0710
    cd sc0710
    make
    sudo insmod build/sc0710.ko
    ```

## Driver Management (CLI)
The `sc0710-cli` tool (installed via the automatic script) provides real-time control.

| Command | Description |
|---------|-------------|
| `sc0710-cli --status` | Show signal format, DKMS status, and module state. |
| `sc0710-cli --load` | Load the kernel module. |
| `sc0710-cli --unload` | Unload the kernel module (safely checks for usage). |
| `sc0710-cli --restart` | Reload the module. |
| `sc0710-cli --debug` | Toggle verbose dmesg logging. |
| `sc0710-cli --image-toggle` | Toggle between No Signal images and Colorbars. |
| `sc0710-cli --software-scaler` | Toggle software scaler modes (all cards). |
| `sc0710-cli --toggle-auto-scalar` | Toggle automatic safety scaler on/off. |
| `sc0710-cli --procedural-timings` | Toggle timing mode (`merge`, `procedural-only`, `static-only`). |
| `sc0710-cli --update` | Pull latest code and rebuild. |
| `sc0710-cli --remove` | Completely uninstall driver and CLI. |
| `sc0710-cli --version` | Show installed driver version. |
| `sc0710-cli --help` | Show usage information. |

## Features & capabilities
*   **Multi-Client Support:** Direct access for multiple simultaneous applications (e.g., OBS + Discord).
*   **DKMS Integration:** Automatically rebuilds on kernel updates (Standard distros).
*   **Atomic Support:** Custom systemd service for boot-time rebuilds on immutable/atomic distros (Bazzite, etc.).
*   **Status Images:** Storage-efficient implementation of "No Signal" / "No Device" screens.
*   **Connection Sensing:** Distinguishes between unplugged cables and signal loss (not 100% reliable).
*   **Video Formats:** Supports 4K60, 1440p144, 1080p240 (requires Windows to change the EDID).
*   **Mode-Switch Stability:** Atomic DMA resync, restart validation, and watchdog recovery significantly improve resolution/refresh switching while apps are open.
*   **Safety Scaling Paths:** Auto-scaler and dynamic-resolution compatibility keep streams alive during geometry mismatches and reduce crash-prone transitions.
*   **Timing Controls:** Runtime timing calculation modes (`merge`, `procedural-only`, `static-only`) via CLI.

## Known Limitations / Roadmap
*   **HDR Tonemapping (On Hold):** Requires opaque I2C commands to onboard ARM MCU.
*   **10-bit Pixel Format (On Hold):** Hardware register map for P010/P016 unknown.
*   **EDID Switching (On Hold):** EEPROM write protocol unknown. (Workaround: Set EDID in Windows first).

## Credits
* ### Based on original reverse engineering by **[Steven Toth (@stoth68000)](https://github.com/stoth68000)** and subsequent work by **[@Subtixx](https://github.com/Subtixx)**.

* ### Thanks to **[Onhil (@Onhil)](https://github.com/Onhil)** for his work on the Elgato 4K Pro
