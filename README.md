# Elgato 4K60 Pro MK.2 Linux Driver

[![Kernel Compatibility](https://img.shields.io/badge/Kernel-6.12%20--%206.18%2B-blueviolet)](https://github.com/Nakildias/sc0710)
[![AUR version](https://img.shields.io/aur/version/sc0710-dkms-git?logo=arch-linux)](https://aur.archlinux.org/packages/sc0710-dkms-git)
[![Status](https://img.shields.io/badge/Status-Maintained-success)](#)
[![GitHub last commit](https://img.shields.io/github/last-commit/Nakildias/sc0710)](https://github.com/Nakildias/sc0710/commits/main)

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/Nakildias/sc0710/blob/main/LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/Nakildias/sc0710?style=flat)](https://github.com/Nakildias/sc0710/stargazers)
[![GitHub issues](https://img.shields.io/github/issues/Nakildias/sc0710)](https://github.com/Nakildias/sc0710/issues)

High-performance, multi-client Linux driver for the Elgato 4K60 Pro mk.2 PCI-e capture card. Engineered for stability on modern kernels (6.12+).
*For older kernels, please use the original [stoth68000/sc0710](https://github.com/stoth68000/sc0710) repository.*

## Kernel Compatibility

| Distribution | Kernel Version | Status | Notes |
|--------------|---------------|--------|-------|
| **Arch Linux** | 6.18.7+ | Stable | Standard configuration. |
| **Fedora** | 6.17.12+ | Stable | Standard configuration. |
| **Debian** | 6.12.57+ | Stable | **Warning:** Repository version of OBS may crash. Use Flatpak OBS. |

## Installation

### Automatic Installation (Recommended)
Supported on Arch Linux, Debian/Ubuntu, and Fedora. This script handles dependencies, DKMS compilation, and user permissions.

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/install-sc0710.sh)"
```

### Arch Linux (AUR)
Install `sc0710-dkms-git` using your preferred helper. Note that the CLI utility is not currently included in the AUR package.

```bash
yay -S sc0710-dkms-git
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
    sudo insmod sc0710.ko
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
| `sc0710-cli --update` | Pull latest code and rebuild. |
| `sc0710-cli --remove` | Completely uninstall driver and CLI. |
| `sc0710-cli --version` | Show installed driver version. |
| `sc0710-cli --help` | Show usage information. |

## Features & capabilities
*   **Multi-Client Support:** Direct access for multiple simultaneous applications (e.g., OBS + Discord).
*   **DKMS Integration:** Automatically rebuilds on kernel updates.
*   **Status Images:** Storage-efficient implementation of "No Signal" / "No Device" screens.
*   **Connection Sensing:** Distinguishes between unplugged cables and signal loss (not 100% reliable).
*   **Video Formats:** Supports 4K60, 1440p144, 1080p240 (requires Windows to change the EDID).
*   **Hotplug Stability:** Implemented. (Close software that uses card when changing resolution otherwise this can cause a Kernel Panic).

## Known Limitations / Roadmap
*   **HDR Tonemapping (On Hold):** Requires opaque I2C commands to onboard ARM MCU.
*   **10-bit Pixel Format (On Hold):** Hardware register map for P010/P016 unknown.
*   **EDID Switching (On Hold):** EEPROM write protocol unknown. (Workaround: Set EDID in Windows first).

## Credits
Based on original reverse engineering by **[Steven Toth (@stoth68000)](https://github.com/stoth68000)** and subsequent work by **[@Subtixx](https://github.com/Subtixx)**.
