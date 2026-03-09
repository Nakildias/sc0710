#!/bin/bash
# Extract ECP5 firmware from the Elgato 4K Pro Windows driver installer
# and install it to /lib/firmware/sc0710/
#
# Requires: curl, 7z (p7zip)
# Supports: Arch, Fedora, Debian (and derivatives)

set -e

DOWNLOAD_URL="https://edge.elgato.com/egc/windows/drivers/4K_Pro/Elgato_4KPro_1.1.0.202.exe"
FIRMWARE_DIR="/lib/firmware/sc0710"
FIRMWARE_FILE="SC0710.FWI.HEX"
INSTALLER="Elgato_4KPro_1.1.0.202.exe"

# --- Check if firmware is already present ---
if [ -f "$FIRMWARE_DIR/$FIRMWARE_FILE" ]; then
	echo "Firmware already present at $FIRMWARE_DIR/$FIRMWARE_FILE, skipping extraction."
	exit 0
fi

# --- Detect distro and install dependencies ---
install_deps() {
	local need_curl=0
	local need_7z=0

	command -v curl  >/dev/null 2>&1 || need_curl=1
	command -v 7z    >/dev/null 2>&1 || need_7z=1

	if [ "$need_curl" -eq 0 ] && [ "$need_7z" -eq 0 ]; then
		return 0
	fi

	echo "Installing required dependencies..."

	local OS_ID=""
	local OS_ID_LIKE=""
	if [ -f /etc/os-release ]; then
		. /etc/os-release
		OS_ID="$ID"
		OS_ID_LIKE="${ID_LIKE:-}"
	fi

	# Arch / Pacman
	if echo "$OS_ID $OS_ID_LIKE" | grep -qE '(arch|manjaro|endeavouros)' || command -v pacman >/dev/null 2>&1; then
		local pkgs=""
		[ "$need_curl" -eq 1 ] && pkgs="$pkgs curl"
		[ "$need_7z"   -eq 1 ] && pkgs="$pkgs p7zip"
		if [ -n "$pkgs" ]; then
			pacman -S --needed --noconfirm $pkgs
		fi
		return 0
	fi

	# Fedora / DNF
	if echo "$OS_ID $OS_ID_LIKE" | grep -qE '(fedora|rhel|centos)' || command -v dnf >/dev/null 2>&1; then
		local pkgs=""
		[ "$need_curl" -eq 1 ] && pkgs="$pkgs curl"
		[ "$need_7z"   -eq 1 ] && pkgs="$pkgs p7zip p7zip-plugins"
		if [ -n "$pkgs" ]; then
			dnf install -y $pkgs
		fi
		return 0
	fi

	# Debian / APT
	if echo "$OS_ID $OS_ID_LIKE" | grep -qE '(debian|ubuntu|pop|linuxmint|kali|raspbian)' || command -v apt-get >/dev/null 2>&1; then
		local pkgs=""
		[ "$need_curl" -eq 1 ] && pkgs="$pkgs curl"
		[ "$need_7z"   -eq 1 ] && pkgs="$pkgs p7zip-full"
		if [ -n "$pkgs" ]; then
			apt-get update -qq
			apt-get install -y $pkgs
		fi
		return 0
	fi

	echo "ERROR: Could not detect a supported package manager (pacman/dnf/apt)." >&2
	echo "Please install 'curl' and 'p7zip' (or '7z') manually." >&2
	exit 1
}

install_deps

# --- Download and extract ---
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Downloading Elgato 4K Pro driver installer..."
curl -L -o "$TMPDIR/$INSTALLER" "$DOWNLOAD_URL"

echo "Extracting firmware..."
7z x -y -o"$TMPDIR/extracted" "$TMPDIR/$INSTALLER" "Final/Game_Capture_4K_Pro/$FIRMWARE_FILE" > /dev/null

SRC="$TMPDIR/extracted/Final/Game_Capture_4K_Pro/$FIRMWARE_FILE"
if [ ! -f "$SRC" ]; then
	echo "ERROR: $FIRMWARE_FILE not found in installer" >&2
	exit 1
fi

echo "Installing firmware to $FIRMWARE_DIR/$FIRMWARE_FILE ..."
mkdir -p "$FIRMWARE_DIR"
cp "$SRC" "$FIRMWARE_DIR/$FIRMWARE_FILE"

echo "Done. Firmware installed to $FIRMWARE_DIR/$FIRMWARE_FILE"
