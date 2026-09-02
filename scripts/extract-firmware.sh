#!/usr/bin/env bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Extract the Elgato ECP5 bits the driver needs from Elgato's own packages and
# install them under the firmware directory:
#
#   1. SC0710.FWI.HEX (4K Pro) or CAMLINKPRO.FWI.HEX (Cam Link Pro) — ECP5
#      runtime firmware, streamed to the FPGA at every boot. Each ships only in
#      that card's official driver installer (.exe).
#   2. EDID profiles — named EDIDFiles/*.bin inside the Elgato Studio app package
#      (.msix). Installed as .../sc0710/edid/<name>.bin for the edid= module param.
#
# Note the two-package split: the driver installer carries the runtime firmware;
# Studio carries the EDIDs (plus persistent-flash updater payloads, which this
# script deliberately does NOT touch — those reflash the card). Package versions
# and checksums are pinned below for reproducible extraction.
#
# Packages are looked for on disk first (CWD, then next to this script); if absent
# the script ASKS before downloading — never silently. Every package (local or
# downloaded) is verified against a pinned SHA-256 before use. The EDID stage is
# optional: declining (or running non-interactively) skips it with a note.
#
# Atomic: installs to /var/lib/sc0710/firmware/ with symlink at /etc/firmware/sc0710/
# Non-atomic: installs to /lib/firmware/sc0710/
#
# Requires: 7z (p7zip); curl for approved downloads.
# Usage: sudo bash extract-firmware.sh [--force] [--installer FILE.exe]
#        [--cam-link-pro-installer FILE.exe] [--studio FILE.msix]

set -e

DRIVER_URL="https://edge.elgato.com/egc/windows/drivers/4K_Pro/Elgato_4KPro_1.1.0.202.exe"
DRIVER_EXE="Elgato_4KPro_1.1.0.202.exe"
DRIVER_SHA256="b65fa18fe022b17379a6552a91d11272e3792cf363bc18b96d514db562fd1e71"
CAM_LINK_PRO_DRIVER_URL="https://edge.elgato.com/egc/windows/drivers/cam_link_pro/Cam_Link_Pro_1.1.0.194.6.exe"
CAM_LINK_PRO_DRIVER_EXE="Cam_Link_Pro_1.1.0.194.6.exe"
CAM_LINK_PRO_DRIVER_SHA256="2e9087819d36d679e794ae008c6f73c5cb94622803bf7cf8df6084a9a1fbe19c"
STUDIO_URL="https://edge.elgato.com/egc/windows/estw/1.1.0/Elgato.Studio_1.1.0.1714_x64.msix"
STUDIO_MSIX="Elgato.Studio_1.1.0.1714_x64.msix"
STUDIO_SHA256="cfdefa7529cb35b2f1d2dc7d84cf89a3c15dccba7a442d72a534f3a15e2f185e"
FOUR_K_PRO_FIRMWARE_FILE="SC0710.FWI.HEX"
CAM_LINK_PRO_FIRMWARE_FILE="CAMLINKPRO.FWI.HEX"
CAM_LINK_PRO_FIRMWARE_SHA256="ef3e792e8072c3a1d8b003ed4a92bcbedba6256719f866f2bfcbeed092f50b22"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
BOLD='\033[1m'
NC='\033[0m'

msg()  { echo -e "${BLUE}::${NC} ${BOLD}$*${NC}"; }
msg2() { echo -e " ${BLUE}->${NC} $*"; }
warn() { echo -e "${YELLOW}warning:${NC} $*" >&2; }
error(){ echo -e "${RED}error:${NC} $*" >&2; }

# --- Args ---
DRIVER_PATH=""
CAM_LINK_PRO_DRIVER_PATH=""
STUDIO_PATH=""
FORCE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE=1 ;;
        --installer) DRIVER_PATH="${2:?missing path after --installer}"; shift ;;
        --cam-link-pro-installer) CAM_LINK_PRO_DRIVER_PATH="${2:?missing path after --cam-link-pro-installer}"; shift ;;
        --studio)    STUDIO_PATH="${2:?missing path after --studio}"; shift ;;
        -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *) error "unknown option: $1 (see --help)"; exit 1 ;;
    esac
    shift
done

# --- Root check ---
if [[ $EUID -ne 0 ]]; then
    error "This script must be run as root."
    echo -e "Usage: ${BOLD}sudo bash extract-firmware.sh${NC}"
    exit 1
fi

# --- Detect atomic distro ---
is_atomic() {
    [[ -f /run/ostree-booted ]] || command -v rpm-ostree &>/dev/null
}

# --- Set paths based on distro type ---
if is_atomic; then
    FIRMWARE_STORE="/var/lib/sc0710/firmware"
    EDID_DIR="$FIRMWARE_STORE/edid"
    DISTRO_TYPE="atomic"
else
    FIRMWARE_DIR="/lib/firmware/sc0710"
    EDID_DIR="$FIRMWARE_DIR/edid"
    DISTRO_TYPE="non-atomic"
fi

# --- What's still missing? ---
WANT_4K_PRO=0
WANT_CAM_LINK_PRO=0
[[ -n "$DRIVER_PATH" ]] && WANT_4K_PRO=1
[[ -n "$CAM_LINK_PRO_DRIVER_PATH" ]] && WANT_CAM_LINK_PRO=1
lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi '1cfa:0012' && WANT_4K_PRO=1
lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi '1cfa:0011' && WANT_CAM_LINK_PRO=1

firmware_installed_path() {
    local file="$1" path
    if [[ "$DISTRO_TYPE" == "atomic" ]]; then
        for path in "$FIRMWARE_STORE/$file" "/lib/firmware/sc0710/$file" "/etc/firmware/sc0710/$file"; do
            [[ -f "$path" ]] && { printf '%s' "$path"; return 0; }
        done
    elif [[ -f "$FIRMWARE_DIR/$file" ]]; then
        printf '%s' "$FIRMWARE_DIR/$file"
        return 0
    fi
    return 1
}

NEED_4K_PRO_FIRMWARE="$WANT_4K_PRO"
NEED_CAM_LINK_PRO_FIRMWARE="$WANT_CAM_LINK_PRO"
if [[ "$FORCE" -eq 0 && "$WANT_4K_PRO" -eq 1 ]] && path="$(firmware_installed_path "$FOUR_K_PRO_FIRMWARE_FILE")"; then
    echo -e "${GREEN}[OK]${NC} 4K Pro firmware already present at $path"
    NEED_4K_PRO_FIRMWARE=0
fi
if [[ "$FORCE" -eq 0 && "$WANT_CAM_LINK_PRO" -eq 1 ]] && path="$(firmware_installed_path "$CAM_LINK_PRO_FIRMWARE_FILE")"; then
    echo -e "${GREEN}[OK]${NC} Cam Link Pro firmware already present at $path"
    NEED_CAM_LINK_PRO_FIRMWARE=0
fi

NEED_EDID=0
if [[ "$WANT_4K_PRO" -eq 1 || -n "$STUDIO_PATH" ]]; then
    NEED_EDID=1
fi
if [[ "$NEED_EDID" -eq 1 && -d "$EDID_DIR" ]] && ls "$EDID_DIR"/*.bin &>/dev/null; then
    echo -e "${GREEN}[OK]${NC} EDID profiles already present in $EDID_DIR"
    NEED_EDID=0
fi

if [[ "$NEED_4K_PRO_FIRMWARE" -eq 0 && "$NEED_CAM_LINK_PRO_FIRMWARE" -eq 0 && "$NEED_EDID" -eq 0 ]]; then
    exit 0
fi

# --- Install dependencies ---
install_deps() {
    local need_curl=0
    local need_7z=0

    command -v curl >/dev/null 2>&1 || need_curl=1
    command -v 7z   >/dev/null 2>&1 || need_7z=1

    if [[ "$need_curl" -eq 0 && "$need_7z" -eq 0 ]]; then
        return 0
    fi

    # Non-interactive (package hook, service): don't run a package manager that
    # may prompt or deadlock on its own transaction lock.
    if [[ ! -t 0 ]]; then
        local missing=""
        [[ "$need_curl" -eq 1 ]] && missing="$missing curl"
        [[ "$need_7z"   -eq 1 ]] && missing="$missing p7zip"
        error "missing dependencies:${missing} — install them and re-run this script."
        exit 1
    fi

    if [[ "$DISTRO_TYPE" == "atomic" ]]; then
        local pkgs=""
        [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
        [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip p7zip-plugins"
        if [[ -n "$pkgs" ]]; then
            msg "Installing dependencies via rpm-ostree:$pkgs"
            if rpm-ostree install --apply-live $pkgs 2>&1; then
                msg2 "Dependencies installed."
            else
                error "Failed to install dependencies."
                echo -e "Try manually: ${BOLD}sudo rpm-ostree install$pkgs${NC}"
                echo -e "Then reboot and re-run this script."
                exit 1
            fi
        fi
    else
        # Non-atomic: use apt/pacman/dnf
        echo "Installing required dependencies..."
        local OS_ID=""
        local OS_ID_LIKE=""
        if [[ -f /etc/os-release ]]; then
            . /etc/os-release
            OS_ID="$ID"
            OS_ID_LIKE="${ID_LIKE:-}"
        fi

        if echo "$OS_ID $OS_ID_LIKE" | grep -qE '(arch|manjaro|endeavouros)' || command -v pacman >/dev/null 2>&1; then
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip"
            [[ -n "$pkgs" ]] && pacman -S --needed --noconfirm $pkgs
        elif echo "$OS_ID $OS_ID_LIKE" | grep -qE '(fedora|rhel|centos)' || command -v dnf >/dev/null 2>&1; then
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip p7zip-plugins"
            [[ -n "$pkgs" ]] && dnf install -y $pkgs
        elif echo "$OS_ID $OS_ID_LIKE" | grep -qE '(debian|ubuntu|pop|linuxmint|kali|raspbian)' || command -v apt-get >/dev/null 2>&1; then
            local pkgs=""
            [[ "$need_curl" -eq 1 ]] && pkgs="$pkgs curl"
            [[ "$need_7z"   -eq 1 ]] && pkgs="$pkgs p7zip-full"
            if [[ -n "$pkgs" ]]; then
                apt-get update -qq
                apt-get install -y $pkgs
            fi
        else
            error "Could not detect a supported package manager (pacman/dnf/apt)."
            echo "Please install 'curl' and 'p7zip' (or '7z') manually."
            exit 1
        fi
    fi
}

if [[ "$DISTRO_TYPE" == "atomic" ]]; then
    msg "Elgato SC0710 Firmware Extractor (Atomic Edition)"
else
    msg "Elgato SC0710 Firmware Extractor"
fi
echo ""

install_deps

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Every package is verified against its pinned SHA-256 — whether downloaded, found
# on disk, or passed explicitly — so a swapped CDN file or stale local copy fails
# loudly instead of installing unknown bytes. Bump the pins when bumping versions.
verify_sha256() {
    local file="$1" expected="$2" actual
    actual="$(sha256sum "$file" | cut -d' ' -f1)"
    if [[ "$actual" != "$expected" ]]; then
        error "SHA-256 mismatch for $file"
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        echo "Not the pinned package version (or a corrupted/tampered copy)." >&2
        return 1
    fi
    msg2 "SHA-256 verified: $(basename "$file")" >&2
}

# Find a package on disk (CWD, then script dir), else ask before downloading.
# obtain_package NAME URL EXPLICIT_PATH SIZE_HINT SHA256 -> prints the path, or fails.
obtain_package() {
    local name="$1" url="$2" explicit="$3" size="$4" sha256="$5" c answer found=""
    if [[ -n "$explicit" ]]; then
        [[ -f "$explicit" ]] || { error "not found: $explicit"; return 1; }
        msg2 "Using local package: $explicit" >&2
        found="$explicit"
    else
        for c in "$PWD/$name" "$SCRIPT_DIR/$name"; do
            [[ -f "$c" ]] && { msg2 "Found package on disk: $c" >&2; found="$c"; break; }
        done
    fi
    if [[ -z "$found" ]]; then
        if [[ ! -t 0 ]]; then
            error "$name is not on disk and this shell is non-interactive — not downloading."
            echo "Place it in the CWD (or pass its path as an option) and re-run." >&2
            return 1
        fi
        read -r -p "$name is not on disk. Download it from Elgato ($size)? [y/N] " answer
        case "$answer" in
            y|Y|yes|YES) ;;
            *) error "declined — nothing downloaded"; return 1 ;;
        esac
        curl -L --fail -o "$TMPDIR/$name" "$url" >&2
        found="$TMPDIR/$name"
    fi
    verify_sha256 "$found" "$sha256" || return 1
    echo "$found"
}

install_runtime_firmware() {
    local src="$1" file="$2"
    if [[ "$DISTRO_TYPE" == "atomic" ]]; then
        mkdir -p "$FIRMWARE_STORE"
        install -m644 "$src" "$FIRMWARE_STORE/$file"
        chcon -t firmware_t "$FIRMWARE_STORE/$file" 2>/dev/null || true
        msg2 "Firmware stored at: $FIRMWARE_STORE/$file"

        # Symlink for the kernel firmware loader on immutable distros
        mkdir -p "/etc/firmware/sc0710"
        ln -sfn "$FIRMWARE_STORE/$file" "/etc/firmware/sc0710/$file"
        chcon -h -t firmware_t "/etc/firmware/sc0710/$file" 2>/dev/null || true
        msg2 "Symlink created: /etc/firmware/sc0710/$file -> $FIRMWARE_STORE/$file"
    else
        mkdir -p "$FIRMWARE_DIR"
        install -m644 "$src" "$FIRMWARE_DIR/$file"
        msg2 "Firmware installed to $FIRMWARE_DIR/$file"
    fi
}

# --- Stage 1: runtime firmware (required for the detected ECP5 card) ---
if [[ "$NEED_4K_PRO_FIRMWARE" -eq 1 ]]; then
    msg "4K Pro runtime firmware ($FOUR_K_PRO_FIRMWARE_FILE)"
    SRC_EXE="$(obtain_package "$DRIVER_EXE" "$DRIVER_URL" "$DRIVER_PATH" "~5 MB" "$DRIVER_SHA256")" || exit 1

    msg "Extracting firmware from the official Elgato driver..."
    7z x -y -o"$TMPDIR/4k-pro" "$SRC_EXE" "Final/Game_Capture_4K_Pro/$FOUR_K_PRO_FIRMWARE_FILE" > /dev/null
    SRC="$TMPDIR/4k-pro/Final/Game_Capture_4K_Pro/$FOUR_K_PRO_FIRMWARE_FILE"
    [[ -f "$SRC" ]] || { error "$FOUR_K_PRO_FIRMWARE_FILE not found in installer."; exit 1; }

    msg "Installing firmware..."
    install_runtime_firmware "$SRC" "$FOUR_K_PRO_FIRMWARE_FILE"
    echo -e "${GREEN}[OK]${NC} Firmware installed successfully."
    echo ""
fi

if [[ "$NEED_CAM_LINK_PRO_FIRMWARE" -eq 1 ]]; then
    msg "Cam Link Pro runtime firmware ($CAM_LINK_PRO_FIRMWARE_FILE)"
    SRC_EXE="$(obtain_package "$CAM_LINK_PRO_DRIVER_EXE" "$CAM_LINK_PRO_DRIVER_URL" "$CAM_LINK_PRO_DRIVER_PATH" "~4 MB" "$CAM_LINK_PRO_DRIVER_SHA256")" || exit 1

    msg "Extracting firmware from the official Elgato driver..."
    7z e -y -o"$TMPDIR/cam-link-pro" "$SRC_EXE" '*CamLinkPro.HEX' > /dev/null
    SRC="$(find "$TMPDIR/cam-link-pro" -type f -iname 'CamLinkPro.HEX' -print -quit)"
    [[ -n "$SRC" && -f "$SRC" ]] || { error "CamLinkPro.HEX not found in installer."; exit 1; }
    verify_sha256 "$SRC" "$CAM_LINK_PRO_FIRMWARE_SHA256" || exit 1

    msg "Installing firmware..."
    install_runtime_firmware "$SRC" "$CAM_LINK_PRO_FIRMWARE_FILE"
    echo -e "${GREEN}[OK]${NC} Firmware installed successfully."
    echo ""
fi

# --- Stage 2: EDID profiles (optional — the edid= param needs them, capture doesn't) ---
# Map Elgato's EDIDFiles/*.bin name to our profile name; skips other cards' files.
edid_profile_name() {
    local stem="${1%.bin}"
    case "$stem" in
        EDID_4K_Pro)          echo "stock"; return 0 ;;
        Custom_EDID_4K_Pro_*) stem="${stem#Custom_EDID_4K_Pro_}" ;;
        Custom_EDID_4K_S_*|Custom_EDID_4K_X_*) return 1 ;;
        Custom_EDID_*)        stem="${stem#Custom_EDID_}" ;;
        *) return 1 ;;
    esac
    stem="${stem,,}"; stem="${stem//_/-}"; stem="${stem//-for-/-}"
    echo "$stem"
}

if [[ "$NEED_EDID" -eq 1 ]]; then
    msg "EDID profiles (for the edid= module param) — from the Elgato Studio package"
    if SRC_MSIX="$(obtain_package "$STUDIO_MSIX" "$STUDIO_URL" "$STUDIO_PATH" "~90 MB" "$STUDIO_SHA256")"; then
        msg "Extracting EDIDFiles/..."
        7z x -y -o"$TMPDIR/studio" "$SRC_MSIX" 'EDIDFiles/*' > /dev/null
        [[ -d "$TMPDIR/studio/EDIDFiles" ]] || { error "no EDIDFiles/ in $SRC_MSIX"; exit 1; }

        mkdir -p "$EDID_DIR"
        count=0
        # 4K Pro-specific files first so they win name collisions (e.g. 3440x1440).
        for f in "$TMPDIR/studio/EDIDFiles/"*4K_Pro*.bin "$TMPDIR/studio/EDIDFiles/"*.bin; do
            [[ -f "$f" ]] || continue
            name="$(edid_profile_name "$(basename "$f")")" || continue
            while [[ -e "$EDID_DIR/$name.bin" ]]; do
                cmp -s "$f" "$EDID_DIR/$name.bin" && continue 2   # same file, second pass
                name="$name-b"
            done
            install -m644 "$f" "$EDID_DIR/$name.bin"
            count=$((count + 1))
        done

        if [[ "$DISTRO_TYPE" == "atomic" ]]; then
            mkdir -p "/etc/firmware/sc0710"
            ln -sfn "$EDID_DIR" "/etc/firmware/sc0710/edid"
        fi
        msg2 "$count EDID profiles installed to $EDID_DIR:"
        ls "$EDID_DIR" | sed 's/\.bin$//;s/^/      /'
        echo -e "${GREEN}[OK]${NC} Select one with the edid= module param, e.g. ${BOLD}edid=1440p${NC}."
    else
        warn "skipping the EDID profiles — the edid= module param won't work until they're installed (re-run this script)."
    fi
    echo ""
fi

echo -e "${BOLD}Note:${NC} The driver module must be reloaded to pick up new files."
echo -e "Run: ${BOLD}sc0710-cli --restart${NC}"
echo ""
