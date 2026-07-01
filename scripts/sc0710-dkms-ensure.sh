#!/bin/bash
# Post-transaction safety net: force-install sc0710 if the stock DKMS hook left it at "built".
set -euo pipefail

LIB="/usr/lib/sc0710/sc0710-dkms-lib.sh"
[[ -f "$LIB" ]] || exit 0

# shellcheck source=/dev/null
source "$LIB"
sc0710_dkms_ensure_installed
