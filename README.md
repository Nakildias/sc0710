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

### Prerequisites
Ensure you have the necessary kernel headers and build tools installed for your distribution.

### Build and Install
1.  Compile the driver:
    ```bash
    make
    ```
2.  Load the kernel module:
    ```bash
    sudo insmod sc0710.ko
    ```

## Usage
Once loaded, the device will appear as a standard V4L2 device (e.g., `/dev/video0`). You can use it with any compatible software such as:
*   OBS Studio
*   FFmpeg
*   VLC
*   Discord (via browser or app if supported)

## Credits
This project is built upon the incredible reverse engineering and initial development work of:
*   **@stoth68000** (Steven Toth)
*   **@Subtixx**

Original repository: [stoth68000/sc0710](https://github.com/stoth68000/sc0710)

Special thanks to them, as this driver would not exist without their foundational work.
