#!/usr/bin/env bash
#
# SC0710 Firmware Service Script
#
# Ensures the Elgato 4K Pro's Lattice ECP5 companion FPGA firmware
# (SC0710.FWI.HEX) is present on the system and available for the driver
# to program the FPGA at module load time.
#
# On every boot, the 4K Pro's ECP5 FPGA loses its configuration (volatile SRAM).
# The driver reprograms it via request_firmware() when the module loads, but the
# firmware file must already be in place. This service guarantees that.
#
# Behavior:
#   1. Detects whether the system is immutable (read-only root) or traditional
#   2. Checks if the firmware file already exists in the correct location
#   3. If missing, downloads and extracts it from the official Elgato installer
#   4. Triggers a module reload to reprogram the FPGA if the driver is already
#      loaded but the ECP5 DONE flag is not set
#
# Called by: sc0710-firmware.service (systemd)
# Installed by: scripts/install-sc0710.sh (unified installer)
#

set -eo pipefail
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

DOWNLOAD_URL="https://edge.elgato.com/egc/windows/drivers/4K_Pro/Elgato_4KPro_1.1.0.202.exe"
FIRMWARE_FILE="SC0710.FWI.HEX"
INSTALLER="Elgato_4KPro_1.1.0.202.exe"
DRV_NAME="sc0710"

# Log directory
LOG_DIR="/var/log/sc0710"
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/firmware_$(date '+%Y%m%d_%H%M%S').log"

log() { echo "$*" >> "$LOG_FILE"; echo "$*"; }

# --- Detect if system has an immutable / read-only root filesystem ---
# This determines where the firmware file can be stored persistently.
# We check multiple indicators in order of specificity:
#   1. OSTree-based (Fedora Atomic, Bazzite, Bluefin, Aurora, Silverblue, Kinoite, GNOME OS)
#   2. NixOS / GNU Guix (store-based, /lib/firmware is a symlink into the store)
#   3. openSUSE MicroOS / Aeon (uses transactional-update, read-only root)
#   4. VanillaOS (uses ABRoot, read-only root)
#   5. Practical test: can we actually create a file in /lib/firmware/?
is_immutable() {
    # OSTree-based distros leave this marker at boot
    [[ -f /run/ostree-booted ]] && return 0

    # rpm-ostree is the package manager for Fedora Atomic variants
    command -v rpm-ostree &>/dev/null && return 0

    # NixOS: /lib/firmware is a symlink into /nix/store (read-only)
    [[ -L /lib/firmware && "$(readlink -f /lib/firmware)" == /nix/store/* ]] && return 0

    # GNU Guix: similar store-based model
    [[ -L /lib/firmware && "$(readlink -f /lib/firmware)" == /gnu/store/* ]] && return 0

    # openSUSE MicroOS / Aeon: uses transactional-update with read-only root
    command -v transactional-update &>/dev/null && return 0

    # VanillaOS: uses ABRoot (A/B root partitions, read-only)
    command -v abroot &>/dev/null && return 0

    # Practical test: if /lib/firmware/ exists but is not writable, treat as immutable
    if [[ -d /lib/firmware ]]; then
        if ! touch /lib/firmware/.sc0710-write-test 2>/dev/null; then
            return 0
        fi
        rm -f /lib/firmware/.sc0710-write-test 2>/dev/null
    fi

    return 1
}

# --- Determine firmware paths based on filesystem mutability ---
if is_immutable; then
    FIRMWARE_STORE="/var/lib/sc0710/firmware"
    FIRMWARE_LINK="/etc/firmware/sc0710"
    FIRMWARE_PATH="$FIRMWARE_STORE/$FIRMWARE_FILE"
    DISTRO_TYPE="immutable"
else
    FIRMWARE_STORE="/lib/firmware/sc0710"
    FIRMWARE_PATH="$FIRMWARE_STORE/$FIRMWARE_FILE"
    DISTRO_TYPE="traditional"
fi

log "=== SC0710 Firmware Service started ==="
log "Distro type: $DISTRO_TYPE"
log "Timestamp: $(date)"

# --- Verify a 4K Pro is actually present ---
if ! lspci -d ::0400 -nn 2>/dev/null | grep -qi "1cfa:0012"; then
    log "No Elgato 4K Pro detected (subsystem 1cfa:0012). Nothing to do."
    exit 0
fi

log "Elgato 4K Pro detected."

# --- Check if firmware file already exists ---
firmware_present() {
    # Check primary location
    if [[ -f "$FIRMWARE_PATH" ]]; then
        return 0
    fi
    # On atomic, also check the standard location in case it's there
    if [[ "$DISTRO_TYPE" == "immutable" && -f "/lib/firmware/sc0710/$FIRMWARE_FILE" ]]; then
        return 0
    fi
    return 1
}

if firmware_present; then
    log "Firmware file present at $FIRMWARE_PATH"
else
    log "Firmware file missing. Extracting from Elgato installer..."

    # --- Install extraction dependencies ---
    install_extract_deps() {
        local need_curl=0
        local need_7z=0

        command -v curl >/dev/null 2>&1 || need_curl=1
        command -v 7z   >/dev/null 2>&1 || need_7z=1

        if [[ "$need_curl" -eq 0 && "$need_7z" -eq 0 ]]; then
            return 0
        fi

        log "Installing extraction dependencies..."

        # Detect package manager regardless of distro type
        local OS_ID=""
        local OS_ID_LIKE=""
        if [[ -f /etc/os-release ]]; then
            . /etc/os-release
            OS_ID="$ID"
            OS_ID_LIKE="${ID_LIKE:-}"
        fi

        if command -v rpm-ostree &>/dev/null; then
            # Fedora Atomic / Bazzite / Bluefin / Aurora
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip p7zip-plugins"
            if [[ -n "$pkgs" ]]; then
                rpm-ostree install --apply-live $pkgs >> "$LOG_FILE" 2>&1 || {
                    log "ERROR: Failed to install dependencies via rpm-ostree."
                    return 1
                }
            fi
        elif echo "$OS_ID $OS_ID_LIKE" | grep -qE '(arch|manjaro|endeavouros)' || command -v pacman >/dev/null 2>&1; then
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip"
            [[ -n "$pkgs" ]] && pacman -S --needed --noconfirm $pkgs >> "$LOG_FILE" 2>&1
        elif echo "$OS_ID $OS_ID_LIKE" | grep -qE '(fedora|rhel|centos)' || command -v dnf >/dev/null 2>&1; then
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip p7zip-plugins"
            [[ -n "$pkgs" ]] && dnf install -y $pkgs >> "$LOG_FILE" 2>&1
        elif echo "$OS_ID $OS_ID_LIKE" | grep -qE '(debian|ubuntu|pop|linuxmint|kali|raspbian)' || command -v apt-get >/dev/null 2>&1; then
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip-full"
            if [[ -n "$pkgs" ]]; then
                apt-get update -qq >> "$LOG_FILE" 2>&1
                apt-get install -y $pkgs >> "$LOG_FILE" 2>&1
            fi
        else
            log "ERROR: Could not detect a supported package manager."
            return 1
        fi
    }

    install_extract_deps || {
        log "ERROR: Cannot install dependencies for firmware extraction."
        exit 1
    }

    # --- Download and extract ---
    TMPDIR=$(mktemp -d)
    trap 'rm -rf "$TMPDIR"' EXIT

    log "Downloading Elgato 4K Pro driver installer..."
    if ! curl -L -o "$TMPDIR/$INSTALLER" "$DOWNLOAD_URL" >> "$LOG_FILE" 2>&1; then
        log "ERROR: Failed to download firmware installer."
        exit 1
    fi

    log "Extracting firmware..."
    if ! 7z x -y -o"$TMPDIR/extracted" "$TMPDIR/$INSTALLER" "Final/Game_Capture_4K_Pro/$FIRMWARE_FILE" > /dev/null 2>&1; then
        log "ERROR: Failed to extract firmware from installer."
        exit 1
    fi

    SRC="$TMPDIR/extracted/Final/Game_Capture_4K_Pro/$FIRMWARE_FILE"
    if [[ ! -f "$SRC" ]]; then
        log "ERROR: $FIRMWARE_FILE not found in installer archive."
        exit 1
    fi

    # --- Install firmware to correct location ---
    mkdir -p "$FIRMWARE_STORE"
    cp "$SRC" "$FIRMWARE_PATH"
    log "Firmware installed to $FIRMWARE_PATH"

    # On immutable distros, create the /etc/firmware symlink for the kernel firmware loader
    if [[ "$DISTRO_TYPE" == "immutable" ]]; then
        mkdir -p "$FIRMWARE_LINK"
        ln -sf "$FIRMWARE_PATH" "$FIRMWARE_LINK/$FIRMWARE_FILE"
        log "Symlink created: $FIRMWARE_LINK/$FIRMWARE_FILE -> $FIRMWARE_PATH"
    fi

    # Set SELinux context if applicable
    chcon -t firmware_t "$FIRMWARE_PATH" 2>/dev/null || true
fi

# --- Check ECP5 FPGA status and reload driver if needed ---
# If the driver is loaded, check if the ECP5 was programmed successfully.
# On a cold boot, the ECP5 loses its configuration. If the driver loaded
# before the firmware file was available, the FPGA may be unprogrammed.
if lsmod | grep -q "$DRV_NAME"; then
    # Check ECP5 status via dmesg
    ECP5_STATUS=$(dmesg 2>/dev/null | grep -E "sc0710.*ECP5" | tail -3)

    if echo "$ECP5_STATUS" | grep -q "firmware programmed successfully"; then
        log "ECP5 FPGA is programmed. No action needed."
    elif echo "$ECP5_STATUS" | grep -q "already configured"; then
        log "ECP5 FPGA already configured (warm reboot). No action needed."
    elif echo "$ECP5_STATUS" | grep -q "Failed to load firmware"; then
        log "ECP5 firmware load previously failed. Reloading driver to retry..."
        # Firmware file should now be in place, reload the module
        if rmmod "$DRV_NAME" 2>/dev/null; then
            sleep 1
            # Load dependencies
            for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
                modprobe "$dep" 2>/dev/null || true
            done
            # Load the driver — method depends on distro type
            if [[ "$DISTRO_TYPE" == "immutable" ]]; then
                SRC_DIR="/var/lib/sc0710"
                if [[ -f "$SRC_DIR/build/${DRV_NAME}.ko" ]]; then
                    chcon -t modules_object_t "$SRC_DIR/build/${DRV_NAME}.ko" 2>/dev/null || true
                    insmod "$SRC_DIR/build/${DRV_NAME}.ko" 2>>"$LOG_FILE" || {
                        log "ERROR: Failed to reload driver via insmod."
                        exit 1
                    }
                else
                    log "ERROR: Module file not found at $SRC_DIR/build/${DRV_NAME}.ko"
                    exit 1
                fi
            else
                modprobe "$DRV_NAME" 2>>"$LOG_FILE" || {
                    log "ERROR: Failed to reload driver via modprobe."
                    exit 1
                }
            fi
            log "Driver reloaded. ECP5 firmware should now be programmed."
        else
            log "WARNING: Could not unload driver (in use). ECP5 will be programmed on next reboot."
        fi
    else
        log "ECP5 status indeterminate from dmesg. Firmware file is in place; it will be used on next module load."
    fi
else
    log "Driver module is not currently loaded. Firmware file is in place for next module load."
fi

log "=== SC0710 Firmware Service completed ==="
