#!/usr/bin/env bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SC0710 Firmware Service Script
#
# Ensures the Elgato 4K Pro ECP5 firmware file is present and the FPGA is
# programmed after boot. On immutable distros the firmware file lives under
# /var/lib/sc0710/firmware/ with a symlink in /etc/firmware/sc0710/.
#
# Usage:
#   sc0710-firmware.sh           Prepare firmware file (runs before driver load)
#   sc0710-firmware.sh --verify  Ensure ECP5 is programmed (runs after driver load)
#

set -eo pipefail
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

DOWNLOAD_URL="https://edge.elgato.com/egc/windows/drivers/4K_Pro/Elgato_4KPro_1.1.0.202.exe"
FIRMWARE_FILE="SC0710.FWI.HEX"
INSTALLER="Elgato_4KPro_1.1.0.202.exe"
DRV_NAME="sc0710"
VERIFY_ONLY=false

[[ "${1:-}" == "--verify" || "${1:-}" == "--verify-ecp5" ]] && VERIFY_ONLY=true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_LIB="${SCRIPT_DIR}/sc0710-firmware-lib.sh"
if [[ ! -f "$FIRMWARE_LIB" ]]; then
    FIRMWARE_LIB="/var/lib/sc0710/sc0710-firmware-lib.sh"
fi
if [[ ! -f "$FIRMWARE_LIB" ]]; then
    FIRMWARE_LIB="/usr/local/libexec/sc0710-firmware-lib.sh"
fi

LOG_DIR="/var/log/sc0710"
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/firmware_$(date '+%Y%m%d_%H%M%S').log"
SC0710_FW_LOG_FILE="$LOG_FILE"

log() { echo "$*" >> "$LOG_FILE"; echo "$*"; }

# shellcheck source=/dev/null
source "$FIRMWARE_LIB"
sc0710_init_firmware_paths

log "=== SC0710 Firmware Service started (${VERIFY_ONLY:+verify}${VERIFY_ONLY:-prepare}) ==="
log "Distro type: $SC0710_DISTRO_TYPE"
log "Timestamp: $(date)"

if ! sc0710_is_4k_pro; then
    log "No Elgato 4K Pro detected (subsystem ${SC0710_4K_PRO_SUBSYS}). Nothing to do."
    exit 0
fi

log "Elgato 4K Pro detected."

install_extract_deps() {
    local need_curl=0
    local need_7z=0

    command -v curl >/dev/null 2>&1 || need_curl=1
    command -v 7z   >/dev/null 2>&1 || need_7z=1
    [[ "$need_curl" -eq 0 && "$need_7z" -eq 0 ]] && return 0

    log "Installing extraction dependencies..."
    local OS_ID="" OS_ID_LIKE=""
    if [[ -f /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        OS_ID="$ID"
        OS_ID_LIKE="${ID_LIKE:-}"
    fi

    if command -v rpm-ostree &>/dev/null; then
        local pkgs=""
        [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
        [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip p7zip-plugins"
        [[ -n "$pkgs" ]] && rpm-ostree install --apply-live $pkgs >> "$LOG_FILE" 2>&1
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

extract_firmware_if_missing() {
    if sc0710_firmware_present; then
        log "Firmware file already present."
        return 0
    fi

    log "Firmware file missing. Extracting from Elgato installer..."
    install_extract_deps || return 1

    local tmpdir src
    tmpdir=$(mktemp -d)
    trap 'rm -rf "$tmpdir"' RETURN

    curl -L -o "$tmpdir/$INSTALLER" "$DOWNLOAD_URL" >> "$LOG_FILE" 2>&1 || {
        log "ERROR: Failed to download firmware installer."
        return 1
    }

    7z x -y -o"$tmpdir/extracted" "$tmpdir/$INSTALLER" "Final/Game_Capture_4K_Pro/$FIRMWARE_FILE" > /dev/null 2>&1 || {
        log "ERROR: Failed to extract firmware from installer."
        return 1
    }

    src="$tmpdir/extracted/Final/Game_Capture_4K_Pro/$FIRMWARE_FILE"
    if [[ ! -f "$src" ]]; then
        log "ERROR: $FIRMWARE_FILE not found in installer archive."
        return 1
    fi

    mkdir -p "$SC0710_FIRMWARE_STORE"
    cp "$src" "$SC0710_FIRMWARE_PATH"
    log "Firmware installed to $SC0710_FIRMWARE_PATH"
}

if [[ "$VERIFY_ONLY" == "true" ]]; then
    if ! sc0710_ensure_firmware_layout; then
        log "ERROR: Firmware layout invalid during verify."
        exit 1
    fi
    if ! sc0710_ensure_ecp5_programmed 5; then
        log "ERROR: ECP5 verify failed."
        exit 1
    fi
    log "=== SC0710 Firmware verify completed (ECP5 OK) ==="
    exit 0
fi

extract_firmware_if_missing || exit 1
sc0710_ensure_firmware_layout || exit 1

# Pre-load phase: if the driver is already loaded (manual load), fix ECP5 now.
if sc0710_driver_loaded; then
    if sc0710_ecp5_programmed_in_dmesg; then
        log "ECP5 FPGA already programmed."
    elif ! sc0710_ensure_ecp5_programmed 3; then
        log "WARNING: ECP5 programming failed in pre-load phase; build service will retry."
    fi
else
    log "Driver not loaded yet; ECP5 will be programmed when the module loads."
fi

log "=== SC0710 Firmware Service completed ==="
