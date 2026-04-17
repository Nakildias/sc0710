#!/usr/bin/env bash
# Extract ECP5 firmware from the Elgato 4K Pro Windows driver installer.
# Unified script for both Atomic and Non-Atomic distros.
#
# Atomic: installs to /var/lib/sc0710/firmware/ with symlink at /etc/firmware/sc0710/
# Non-atomic: installs to /lib/firmware/sc0710/
#
# Requires: curl, 7z (p7zip)
# Usage: sudo bash extract-firmware.sh

set -e

DOWNLOAD_URL="https://edge.elgato.com/egc/windows/drivers/4K_Pro/Elgato_4KPro_1.1.0.202.exe"
FIRMWARE_FILE="SC0710.FWI.HEX"
INSTALLER="Elgato_4KPro_1.1.0.202.exe"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
BOLD='\033[1m'
NC='\033[0m'

msg()  { echo -e "${BLUE}::${NC} ${BOLD}$*${NC}"; }
msg2() { echo -e " ${BLUE}->${NC} $*"; }
error(){ echo -e "${RED}error:${NC} $*" >&2; }

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
    FIRMWARE_PATH="$FIRMWARE_STORE/$FIRMWARE_FILE"
    DISTRO_TYPE="atomic"
else
    FIRMWARE_DIR="/lib/firmware/sc0710"
    FIRMWARE_PATH="$FIRMWARE_DIR/$FIRMWARE_FILE"
    DISTRO_TYPE="non-atomic"
fi

# --- Check if firmware is already present ---
if [[ -f "$FIRMWARE_PATH" ]]; then
    echo -e "${GREEN}[OK]${NC} Firmware already present at $FIRMWARE_PATH"
    exit 0
fi

# On atomic, also check standard location
if [[ "$DISTRO_TYPE" == "atomic" && -f "/lib/firmware/sc0710/$FIRMWARE_FILE" ]]; then
    echo -e "${GREEN}[OK]${NC} Firmware already present at /lib/firmware/sc0710/$FIRMWARE_FILE"
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
    msg "Elgato 4K Pro Firmware Extractor (Atomic Edition)"
else
    msg "Elgato 4K Pro Firmware Extractor"
fi
echo ""

install_deps

# --- Download and extract ---
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

msg "Downloading Elgato 4K Pro driver installer..."
curl -L -o "$TMPDIR/$INSTALLER" "$DOWNLOAD_URL"

msg "Extracting firmware..."
7z x -y -o"$TMPDIR/extracted" "$TMPDIR/$INSTALLER" "Final/Game_Capture_4K_Pro/$FIRMWARE_FILE" > /dev/null

SRC="$TMPDIR/extracted/Final/Game_Capture_4K_Pro/$FIRMWARE_FILE"
if [[ ! -f "$SRC" ]]; then
    error "$FIRMWARE_FILE not found in installer."
    exit 1
fi

# --- Install firmware ---
msg "Installing firmware..."

if [[ "$DISTRO_TYPE" == "atomic" ]]; then
    mkdir -p "$FIRMWARE_STORE"
    cp "$SRC" "$FIRMWARE_STORE/$FIRMWARE_FILE"
    chcon -t firmware_t "$FIRMWARE_STORE/$FIRMWARE_FILE" 2>/dev/null || true
    msg2 "Firmware stored at: $FIRMWARE_STORE/$FIRMWARE_FILE"

    # Create symlink for kernel firmware loader
    if [[ ! -d "/lib/firmware/sc0710" ]]; then
        mkdir -p "/etc/firmware/sc0710"
        ln -sf "$FIRMWARE_STORE/$FIRMWARE_FILE" "/etc/firmware/sc0710/$FIRMWARE_FILE"
        msg2 "Symlink created: /etc/firmware/sc0710/$FIRMWARE_FILE -> $FIRMWARE_STORE/$FIRMWARE_FILE"
    fi
else
    mkdir -p "$FIRMWARE_DIR"
    cp "$SRC" "$FIRMWARE_DIR/$FIRMWARE_FILE"
    msg2 "Firmware installed to $FIRMWARE_DIR/$FIRMWARE_FILE"
fi

echo ""
echo -e "${GREEN}[OK]${NC} Firmware installed successfully."
echo ""
echo -e "${BOLD}Note:${NC} The driver module must be reloaded to pick up the firmware."
echo -e "Run: ${BOLD}sc0710-cli --restart${NC}"
echo ""
