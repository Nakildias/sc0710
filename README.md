# Elgato 4K60 Pro MK.2 Linux Driver
[![Kernel Compatibility](https://img.shields.io/badge/Kernel-6.12%20--%206.18%2B-blueviolet)](https://github.com/Nakildias/sc0710)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/Nakildias/sc0710/blob/main/LICENSE)
[![Status](https://img.shields.io/badge/Status-Maintained-success)](#)

A high-performance, multi-client Linux driver for the Elgato 4K60 Pro mk.2. 
This project is a modern reimagining of the original driver, engineered for stability on current kernels.

> [!IMPORTANT]
> **Kernel Compatibility Verification**
>
> This driver has been rigorously tested and validated for stability on the following kernels:
> * **Arch Linux**: Kernel `6.18.3-arch1-1` — Works perfectly with standard configuration.
> * **Fedora**: Kernel `6.17.12-300.fc43.x86_64` — Works perfectly with standard configuration.
> * **Debian**: Kernel `6.12.57+deb13-amd64` — **Note:** The OBS version from `dnf`/repositories is currently broken and crashes when accessing the capture card. Debian users **must** use the **Flatpak** version of OBS for stable operation.

## Features

This enhanced driver architecture delivers enterprise-grade stability and extended functionality over the reference implementation:

*   **Modern Kernel Compatibility**: Re-engineered codebase ensuring seamless compilation and operation on bleeding-edge Linux kernels.
*   **Fail-Safe Signal Generation**: Implements automatic SMPTE color bar generation during signal loss or physical disconnection. This maintains the V4L2 buffer stream, preventing consumer application capture failures or pipeline crashes.
*   **Automated DKMS Lifecycle**: Fully integrated Dynamic Kernel Module Support (DKMS). The driver automatically recompiles and links against new kernel headers during system updates, ensuring persistent availability without manual intervention.
*   **Concurrent Access Architecture**: Unlocks multi-client capabilities, allowing simultaneous stream acquisition by multiple applications.
*   **High-Bandwidth Throughput**: Optimized Direct Memory Access (DMA) pathing for consistent 4K 60fps capture performance (subject to host PCIe bandwidth availability).
*   **Resilient Hotplug Mechanism**: Hardened interrupt handling for HDMI hotplug events, eliminating kernel panics and hard lockups during physical cable reconnections.
*   **Atomic Signal Restoration**: Corrected frame alignment and synchronization logic prevents image tearing or buffer desynchronization upon signal restoration.

# Installation

## Quick Install (Recommended)
This command automatically handles dependencies, compiles the driver, and installs it for you.

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/install-sc0710.sh)"
```

---

## Help & Advanced Usage

### Dependencies
If you need to compile manually, ensure you have the necessary kernel headers and build tools installed for your distribution:

| Distro | Dependencies Command |
|--------|---------------------|
| **Arch** | `sudo pacman -S base-devel linux-headers git` |
| **Debian** | `sudo apt install build-essential linux-headers-$(uname -r) git` |
| **Fedora** | `sudo dnf install kernel-devel kernel-headers gcc make git` |

### Manual Compilation
To compile the driver from source:

```bash
git clone https://github.com/Nakildias/sc0710 && cd ./sc0710 && make
```

### Driver Management
Commands to manually load or unload the kernel module:

**Load Driver:**
```bash
sudo insmod sc0710.ko
```
*(Note: If you have issues, you may need to manually load dependencies like `videodev` first.)*

**Unload Driver:**
```bash
sudo rmmod sc0710
```

## Usage

Once the driver is loaded, the card will be recognized as **Elgato 4k60 Pro mk.2** (typically `/dev/video0`). 

### Compatible Software
The driver is engineered for seamless integration with modern streaming and playback tools:

* **OBS Studio**: Full 4K60 support with multi-client capabilities.
* **FFmpeg**: Optimized for high-bandwidth raw video acquisition.
* **VLC Media Player**: Low-latency hardware preview.
* **Discord**: Native camera support for high-quality screen sharing.

> [!TIP]
> **Multi-Client Power**: Because of the re-engineered architecture, you can keep OBS open while simultaneously sharing your capture card on Discord.

> [!TIP]
> **EDID Switching**: You can change the EDID on windows and the setting will stay applied in linux, so if you need 1080p 240hz setting it on windows is the way to go for now.

## Roadmap

### Current Goals
- **Kernel Compatibility** — Keeping the card functional on future kernels
- **Stability Improvements** — Enhancing reliability and error handling
- **Driver-Level Features** — Features that don't require hardware register manipulation
- **Resolution-Switch Recovery** — Making the card recover properly when changing resolution on source
- **BMP Support for no signal** — Making the video display the classic Elgato no signal image instead of colorbars

### Completed
- [x] Multi-client streaming support
- [x] 4K60 and 1080p120 capture
- [x] Signal loss recovery with color bars
- [x] DKMS integration
- [x] Hotplug stability fixes
- [x] Professional installer with logging

### On Hold — Requires Reverse Engineering

> [!WARNING]
> **Hardware Feature Development Paused**
> 
> The following features require reading/writing to the capture card's ARM MCU via I2C commands. After extensive investigation, the device remains a black box — the exact register addresses, command sequences, and communication protocols are unknown or uncertain to be accurate.
> 
> **If you have RE experience and want to help, contributions are welcome!**

| Feature | Status | Why It's Hard |
|---------|--------|---------------|
| HDR to SDR Tonemapping Toggle | On Hold | Requires unknown I2C commands to ARM MCU |
| P010 (10-bit) Pixel Format | On Hold | Hardware pixel format configuration unknown |
| EDID Mode Switching | On Hold | EEPROM write protocol not fully understood |
| Firmware Version Readback | On Hold | MCU register layout unknown |
| XRGB 4:4:4 Pixel Format | On Hold | Chroma configuration registers unknown |

### How to Contribute to On-Hold Features

If you have experience with:
- Logic analyzers / I2C bus sniffing
- PCIe protocol analysis
- Windows driver reverse engineering
- ARM MCU debugging

You can help by capturing register writes from the Windows Elgato 4K Capture Utility and submitting traces. Any contributions will be reviewed and merged.

## Credits
This project is built upon the incredible reverse engineering and initial development work of:
*   **@stoth68000** (Steven Toth)
*   **@Subtixx**

Original repository: [stoth68000/sc0710](https://github.com/stoth68000/sc0710)

Special thanks to them, as this version of the driver would not exist without their foundational work.
