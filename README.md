# Elgato 4K60 Pro mk.2 Linux Driver

An improved, modern Linux driver for the Elgato 4K60 Pro mk.2 capture card.

> [!NOTE]
> This driver has been specifically tested and optimized for **Kernel 6.18.3-arch1-1**.

## Features

This improved version brings significant stability and functionality updates over the original driver:

*   **Modern Kernel Support**: It actually compiles! Fixed build issues for newer Linux kernels (tested on 6.18+).
*   **Multi-Application Support**: The capture card can now be accessed by multiple applications simultaneously.
*   **Robust Hotplug Stability**: Can survive HDMI unplug and replug events without crashing the kernel (no more hard lockups!).
*   **Correct Signal Restoration**: Fixes image alignment issues upon HDMI reconnection (no more "cut" or swapped frames).
*   **Active Support**: If you encounter issues, please open a ticket!

## Installation

### 1. Prerequisites (Install Dependencies)
Before you start, you need to install the necessary tools to compile the driver. Open your terminal and run the command for your Linux distribution:

#### **Arch Linux / Manjaro / EndeavourOS**
```bash
sudo pacman -S base-devel linux-headers git
```
*(Note: If you use a custom kernel like `linux-zen`, install `linux-zen-headers` instead)*

#### **Ubuntu / Debian / Linux Mint / Pop!_OS**
```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) git
```

#### **Fedora**
```bash
sudo dnf install kernel-devel kernel-headers gcc make git
```

---

### 2. Build and Install ("Idiot Proof" Guide)

**Step 1: Download the driver**
```bash
git clone https://github.com/Nakildias/sc0710.git
cd sc0710
```

**Step 2: Compile the driver**
Type `make` and press Enter. You should see a lot of text scrolling by.
```bash
make
```
*If this fails, double-check that you installed the prerequisites above!*

**Step 3: Load the driver**
This command loads the driver into your kernel so you can use the card immediately.
```bash
sudo insmod sc0710.ko
```

**Step 4: Verify it's working**
Check if a new video device appeared (usually `/dev/video0` or `/dev/video1`):
```bash
ls /dev/video*
```

---
### 3. Making it Permanent (Not Recommended)

> [!WARNING]
> **I strongly recommend loading the driver manually (Step 3 above) only when you need to use the capture card.**
>
> Since this driver is compiled manually for your specific kernel version, **a system update that changes your Linux kernel will break the driver** if you install it permanently. You would then have to re-compile and re-install it every time you update your system.

If you *really* want it to load automatically at boot (and understand you must re-do this after every kernel update), follow these steps:

1.  **Copy the driver to your kernel modules folder:**
    ```bash
    sudo cp sc0710.ko /lib/modules/$(uname -r)/kernel/drivers/media/pci/
    ```

2.  **Update module dependencies:**
    ```bash
    sudo depmod -a
    ```

3.  **Tell the system to load it at boot:**
    ```bash
    echo "sc0710" | sudo tee /etc/modules-load.d/sc0710.conf
    ```

Now you can reboot, and the capture card will be ready automatically!

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
