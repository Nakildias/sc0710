# Elgato 4K60 Pro mk.2 Linux Driver

An improved, modern Linux driver for the Elgato 4K60 Pro mk.2 capture card.

> [!IMPORTANT]
> **Kernel Compatibility Verification**
>
> This driver has been rigorously tested and validated for stability on the following kernels:
> *   **Arch Linux**: Kernel `6.18.3-arch1-1`
> *   **Fedora Workstation**: Kernel `6.17.12-300.fc43.x86_64`

## Features

This enhanced driver architecture delivers enterprise-grade stability and extended functionality over the reference implementation:

*   **Modern Kernel Compatibility**: Re-engineered codebase ensuring seamless compilation and operation on bleeding-edge Linux kernels (tested on 6.18+).
*   **Fail-Safe Signal Generation**: Implements automatic SMPTE color bar generation during signal loss or physical disconnection. This maintains the V4L2 buffer stream, preventing consumer application capture failures or pipeline crashes.
*   **Automated DKMS Lifecycle**: Fully integrated Dynamic Kernel Module Support (DKMS). The driver automatically recompiles and links against new kernel headers during system updates, ensuring persistent availability without manual intervention.
*   **Concurrent Access Architecture**: Unlocks multi-client capabilities, allowing simultaneous stream acquisition by multiple applications.
*   **High-Bandwidth Throughput**: Optimized Direct Memory Access (DMA) pathing for consistent 4K 60fps capture performance (subject to host PCIe bandwidth availability).
*   **Resilient Hotplug Mechanism**: Hardened interrupt handling for HDMI hotplug events, eliminating kernel panics and hard lockups during physical cable reconnections.
*   **Atomic Signal Restoration**: Corrected frame alignment and synchronization logic prevents image tearing or buffer desynchronization upon signal restoration.

## Installation

### Method 1: One-Line Installation (Recommended)
This command will download and run the installation script. It handles dependencies and driver installation automatically.

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/install-sc0710.sh)"
```

### Method 2: Manual Installation via Script
If you prefer to inspect the code first:

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/Nakildias/sc0710.git
    cd sc0710
    ```

2.  **Run the installer:**
    ```bash
    chmod +x install-sc0710.sh
    sudo ./install-sc0710.sh
    ```

### Method 3: Manual Compilation (Advanced)
Only use this if you cannot use the script for some reason.

1.  **Install Prerequisites:**
    *   **Arch/Manjaro:** `sudo pacman -S base-devel linux-headers git`
    *   **Debian/Ubuntu:** `sudo apt install build-essential linux-headers-$(uname -r) git`
    *   **Fedora:** `sudo dnf install kernel-devel kernel-headers gcc make git`

2.  **Compile:**
    ```bash
    make
    ```

3.  **Load:**
    ```bash
    sudo insmod sc0710.ko
    ```
    *Note: You may need to manually load dependencies like `videodev`, `videobuf2-v4l2`, `videobuf2-vmalloc` first.*

4.  **Install (Permanent):**
    Copy `sc0710.ko` to `/lib/modules/$(uname -r)/kernel/drivers/media/pci/` and run `sudo depmod -a`.

## Usage
Once loaded, the device will appear as a standard V4L2 device (e.g., `/dev/video0`). You can use it with any compatible software such as:
*   OBS Studio
*   FFmpeg
*   VLC
*   Discord

## Credits
This project is built upon the incredible reverse engineering and initial development work of:
*   **@stoth68000** (Steven Toth)
*   **@Subtixx**

Original repository: [stoth68000/sc0710](https://github.com/stoth68000/sc0710)

Special thanks to them, as this version of the driver would not exist without their foundational work.
