# Elgato 4K60 Pro mk.2 Linux Driver

An improved, modern Linux driver for the Elgato 4K60 Pro mk.2 capture card.

> [!NOTE]
> This driver has been specifically tested and optimized for **Kernel 6.18.3-arch1-1**.

## Features

This improved version brings significant stability and functionality updates over the original driver:

*   **Modern Kernel Support**: It actually compiles! Fixed build issues for newer Linux kernels (tested on 6.18+).
*   **Multi-Application Support**: The capture card can now be accessed by multiple applications simultaneously.
*   **4k 60fps support**: It should work flawlessly if you have enough pcie bandwidth. If you don't then there will be artifacts.
*   **Robust Hotplug Stability**: Can survive HDMI unplug and replug events without crashing the kernel (no more hard lockups!).
*   **Correct Signal Restoration**: Fixes image alignment issues upon HDMI reconnection (no more "cut" or swapped frames).
*   **Active Support**: If you encounter issues, please open a ticket!

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

Special thanks to them, as this driver would not exist without their foundational work.
