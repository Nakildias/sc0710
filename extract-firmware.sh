#!/bin/bash
# Extract ECP5 firmware from the Elgato 4K Pro Windows driver installer
# and install it to /lib/firmware/sc0710/
#
# Requires: curl, 7z (p7zip)

set -e

DOWNLOAD_URL="https://edge.elgato.com/egc/windows/drivers/4K_Pro/Elgato_4KPro_1.1.0.202.exe"
FIRMWARE_DIR="/lib/firmware/sc0710"
FIRMWARE_FILE="SC0710.FWI.HEX"
INSTALLER="Elgato_4KPro_1.1.0.202.exe"

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
sudo mkdir -p "$FIRMWARE_DIR"
sudo cp "$SRC" "$FIRMWARE_DIR/$FIRMWARE_FILE"

echo "Done. Firmware installed to $FIRMWARE_DIR/$FIRMWARE_FILE"
