#!/usr/bin/env bash
# Write lib/sc0710-version.h from the root version file.
# Called by update-version.sh and the Makefile before each build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
VERSION_FILE="${ROOT_DIR}/version"
HEADER_FILE="${ROOT_DIR}/lib/sc0710-version.h"

[[ -f "$VERSION_FILE" ]] || { echo "Error: ${VERSION_FILE} not found." >&2; exit 1; }

VERSION="$(head -n 1 "$VERSION_FILE" | tr -d '[:space:]')"

cat > "$HEADER_FILE" <<EOF
/* Auto-generated from version — do not edit manually. */
#ifndef SC0710_VERSION_H
#define SC0710_VERSION_H

#define SC0710_DRV_VERSION_STRING "${VERSION}"

#endif /* SC0710_VERSION_H */
EOF

echo "Synced ${HEADER_FILE} -> ${VERSION}"
