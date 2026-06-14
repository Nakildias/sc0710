#!/usr/bin/env bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SC0710 Removal Verification Script
#
# Checks whether the driver, CLI, services, and related install artifacts
# are still present on the system. Does not require cloning the repository.
#
# Usage:
#   bash check-sc0710-removal.sh
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/scripts/check-sc0710-removal.sh)"

set -uo pipefail

DRV_NAME="sc0710"
KERNEL_VER="$(uname -r)"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
BOLD='\033[1m'
NC='\033[0m'

TRACE_COUNT=0

trace_found() {
    local label="$1"
    local detail="${2:-}"
    TRACE_COUNT=$((TRACE_COUNT + 1))
    echo -e "  ${RED}●${NC} ${label}"
    [[ -n "$detail" ]] && echo -e "    ${detail}"
}

trace_clear() {
    local label="$1"
    echo -e "  ${GREEN}●${NC} ${label}"
}

section() {
    echo ""
    echo -e "${BLUE}::${NC} ${BOLD}$1${NC}"
}

echo -e "${BOLD}SC0710 Removal Check${NC}"
echo -e "Kernel: ${KERNEL_VER}"
echo -e "Date:   $(date)"

# --- Runtime state ---
section "Kernel module"

if lsmod 2>/dev/null | grep -q "^${DRV_NAME}[[:space:]]"; then
    refs=$(awk -v m="$DRV_NAME" '$1 == m {print $3}' /proc/modules 2>/dev/null || echo "?")
    trace_found "Module is loaded in kernel" "Reference count: ${refs}"
else
    trace_clear "Module is not loaded"
fi

if [[ -d "/sys/module/${DRV_NAME}" ]]; then
    trace_found "Sysfs module directory exists" "/sys/module/${DRV_NAME}"
else
    trace_clear "No /sys/module/${DRV_NAME}"
fi

if modinfo "$DRV_NAME" &>/dev/null; then
    modpath=$(modinfo -n "$DRV_NAME" 2>/dev/null || echo unknown)
    trace_found "Module is registered with the kernel" "$modpath"
else
    trace_clear "modinfo ${DRV_NAME} not available"
fi

# --- Module files on disk ---
section "Module files"

found_ko=false
while IFS= read -r ko; do
    found_ko=true
    trace_found "Kernel module file on disk" "$ko"
done < <(find "/lib/modules/${KERNEL_VER}" \( -name "${DRV_NAME}.ko" -o -name "${DRV_NAME}.ko.*" \) 2>/dev/null || true)

if [[ "$found_ko" == "false" ]]; then
    trace_clear "No ${DRV_NAME}.ko under /lib/modules/${KERNEL_VER}"
fi

# --- DKMS / source trees ---
section "DKMS and source"

if command -v dkms &>/dev/null && dkms status 2>/dev/null | grep -q "^${DRV_NAME}"; then
    while IFS= read -r line; do
        [[ -n "$line" ]] && trace_found "DKMS registration" "$line"
    done < <(dkms status 2>/dev/null | grep "^${DRV_NAME}" || true)
else
    trace_clear "No DKMS registration for ${DRV_NAME}"
fi

if compgen -G "/usr/src/${DRV_NAME}-*" >/dev/null; then
    for src in /usr/src/${DRV_NAME}-*; do
        [[ -e "$src" ]] && trace_found "Driver source tree" "$src"
    done
else
    trace_clear "No /usr/src/${DRV_NAME}-*"
fi

if [[ -d "/var/lib/dkms/${DRV_NAME}" ]]; then
    trace_found "DKMS state directory" "/var/lib/dkms/${DRV_NAME}"
else
    trace_clear "No /var/lib/dkms/${DRV_NAME}"
fi

# --- CLI and atomic install tree ---
section "CLI and install files"

if [[ -x "/usr/local/bin/sc0710-cli" ]]; then
    trace_found "CLI utility installed" "/usr/local/bin/sc0710-cli"
elif command -v sc0710-cli &>/dev/null; then
    trace_found "CLI utility found in PATH" "$(command -v sc0710-cli)"
else
    trace_clear "sc0710-cli not installed"
fi

if [[ -d "/var/lib/sc0710" ]]; then
    trace_found "Atomic install directory" "/var/lib/sc0710"
else
    trace_clear "No /var/lib/sc0710"
fi

for libexec in \
    /usr/local/libexec/sc0710-firmware.sh \
    /usr/local/libexec/sc0710-firmware-lib.sh \
    /var/lib/sc0710/sc0710-firmware.sh \
    /var/lib/sc0710/sc0710-firmware-lib.sh \
    /var/lib/sc0710/build-and-load.sh; do
    if [[ -f "$libexec" ]]; then
        trace_found "Firmware/build script present" "$libexec"
    fi
done

# --- systemd services ---
section "systemd services"

service_traces=0
for unit in \
    sc0710-build.service \
    sc0710-firmware.service \
    sc0710-firmware-verify.service; do
    unit_file="/etc/systemd/system/${unit}"
    if [[ -f "$unit_file" ]]; then
        service_traces=1
        trace_found "Unit file present" "$unit_file"
    elif systemctl list-unit-files --no-legend "${unit}" 2>/dev/null | grep -q "^${unit}"; then
        service_traces=1
        state=$(systemctl is-enabled "${unit}" 2>/dev/null || echo "not-found")
        trace_found "systemd unit registered" "${unit} (${state})"
    fi
done

if [[ "$service_traces" -eq 0 ]]; then
    trace_clear "No sc0710 systemd units found"
fi

# --- boot / modprobe config ---
section "Boot configuration"

for conf in \
    "/etc/modules-load.d/${DRV_NAME}.conf" \
    "/etc/modprobe.d/${DRV_NAME}.conf" \
    "/etc/modprobe.d/${DRV_NAME}-params.conf"; do
    if [[ -f "$conf" ]]; then
        trace_found "Configuration file present" "$conf"
    else
        trace_clear "Not present: $conf"
    fi
done

# --- logs ---
section "Logs"

if [[ -d "/var/log/sc0710" ]]; then
    log_count=$(find /var/log/sc0710 -type f 2>/dev/null | wc -l)
    trace_found "Log directory present" "/var/log/sc0710 (${log_count} file(s))"
else
    trace_clear "No /var/log/sc0710"
fi

# --- 4K Pro firmware ---
section "4K Pro firmware"

firmware_any=false
for fw in \
    "/var/lib/sc0710/firmware/SC0710.FWI.HEX" \
    "/lib/firmware/sc0710/SC0710.FWI.HEX" \
    "/etc/firmware/sc0710/SC0710.FWI.HEX"; do
    if [[ -f "$fw" || -L "$fw" ]]; then
        firmware_any=true
        trace_found "Firmware file or symlink still present" "$fw"
    fi
done

if [[ -L "/etc/firmware/sc0710" ]]; then
    firmware_any=true
    trace_found "Firmware symlink still present" "/etc/firmware/sc0710 -> $(readlink -f /etc/firmware/sc0710 2>/dev/null || readlink /etc/firmware/sc0710)"
fi

if [[ -d "/lib/firmware/sc0710" ]]; then
    firmware_any=true
    trace_found "Firmware directory still present" "/lib/firmware/sc0710"
fi

if [[ "$firmware_any" == "false" ]]; then
    trace_clear "No 4K Pro firmware files found"
fi

# --- Summary ---
echo ""
echo -e "${BOLD}Summary${NC}"

if [[ "$TRACE_COUNT" -eq 0 ]]; then
    echo -e "${GREEN}[CLEAN]${NC} No driver, CLI, service, firmware, or install traces detected."
    exit 0
fi

echo -e "${RED}[TRACES FOUND]${NC} ${TRACE_COUNT} remnant(s) detected."
echo ""
echo -e "If removal was intentional, try:"
echo -e "  ${BOLD}sudo sc0710-cli --remove${NC}   (if the CLI is still installed)"
echo -e "  Stale atomic installs may leave ${BOLD}/lib/modules/\$(uname -r)/extra/${DRV_NAME}/${NC}"
echo -e "  Remove that directory and run ${BOLD}sudo depmod -a${NC} if modinfo still finds the module."
echo -e "  Or remove any paths listed above manually, then run this script again."
exit 1
