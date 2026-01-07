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

## Roadmap

Features planned or under investigation for future releases:

### High Priority
- [ ] **HDR to SDR Tonemapping Control** — Toggle the card's built-in hardware tonemapping via module parameter
- [ ] **P010 (10-bit) Pixel Format** — Enable proper HDR capture for applications that support it
- [ ] **EDID Mode Switching** — Support for 4k60 Pro, Passthrough, and Merged EDID modes

### Medium Priority
- [ ] **XRGB 4:4:4 Pixel Format** — Full chroma capture for PC/desktop sources
- [ ] **REC.601 Colorspace** — Proper colorimetry reporting for standard definition devices
- [ ] **High Refresh Rates** — 1080p @ 240Hz, 1440p @ 144Hz support

### Completed
- [x] Multi-client streaming support
- [x] 4K60 and 1080p120 capture
- [x] Signal loss recovery with color bars
- [x] DKMS integration
- [x] Hotplug stability fixes

> [!CAUTION]
> **Reverse Engineering Required**
> 
> Features like EDID mode switching and HDR tonemapping control require additional reverse engineering of the card's ARM MCU communication protocol. The Windows driver sends specific I2C commands to configure these settings, but the exact register addresses and command sequences are currently unknown.
> 
> If you have experience with logic analyzers, I2C sniffing, or have captured register writes from the Windows Elgato 4K Capture Utility, contributions would be greatly appreciated!

## Credits
This project is built upon the incredible reverse engineering and initial development work of:
*   **@stoth68000** (Steven Toth)
*   **@Subtixx**

Original repository: [stoth68000/sc0710](https://github.com/stoth68000/sc0710)

Special thanks to them, as this version of the driver would not exist without their foundational work.
