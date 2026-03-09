#!/bin/bash
# update-version.sh — Bump the sc0710 driver version to today's date.
#
# Version format: YYYY.MM.DD-REV
#   - If no version exists for today, starts at revision 1.
#   - If a version for today already exists, increments the revision.
#
# Files updated:
#   ./version   — contains "YYYY.MM.DD-REV"
#   ./dkms.conf — PACKAGE_VERSION="YYYY.MM.DD.REV"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION_FILE="${SCRIPT_DIR}/version"
DKMS_FILE="${SCRIPT_DIR}/dkms.conf"

TODAY=$(date +%Y.%m.%d)

# Read current version from the version file
if [[ ! -f "$VERSION_FILE" ]]; then
    echo "Error: ${VERSION_FILE} not found." >&2
    exit 1
fi

CURRENT=$(head -n 1 "$VERSION_FILE" | tr -d '[:space:]')
echo "Current version: ${CURRENT}"

# Extract the date portion and revision from the current version (format: YYYY.MM.DD-REV)
CURRENT_DATE="${CURRENT%-*}"
CURRENT_REV="${CURRENT##*-}"

if [[ "$CURRENT_DATE" == "$TODAY" ]]; then
    # Same date — increment revision
    NEW_REV=$(( CURRENT_REV + 1 ))
else
    # New date — start at revision 1
    NEW_REV=1
fi

NEW_VERSION="${TODAY}-${NEW_REV}"
# dkms.conf uses dots instead of a dash before the revision
NEW_DKMS_VERSION="${TODAY}.${NEW_REV}"

echo "New version:     ${NEW_VERSION}"

# Update the version file
echo "$NEW_VERSION" > "$VERSION_FILE"

# Update dkms.conf — replace the PACKAGE_VERSION line
sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${NEW_DKMS_VERSION}\"/" "$DKMS_FILE"

echo "Updated:"
echo "  ${VERSION_FILE}  -> ${NEW_VERSION}"
echo "  ${DKMS_FILE} -> PACKAGE_VERSION=\"${NEW_DKMS_VERSION}\""
