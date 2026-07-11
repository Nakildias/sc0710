#!/usr/bin/env bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SC0710 Control Utility - Unified for Atomic and Non-Atomic distros
# Detects distro type at runtime and branches accordingly.

# --- Configuration ---
VERSION_URL="https://raw.githubusercontent.com/Nakildias/sc0710/main/version"
DRV_NAME="sc0710"

# --- Colors ---
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
BOLD='\033[1m'
NC='\033[0m'

# --- Auto-elevate to root ---
if [[ $EUID -ne 0 ]]; then
    exec sudo SC0710_INVOKE_USER="$USER" "$0" "$@"
fi
DUMP_USER="${SC0710_INVOKE_USER:-${SUDO_USER:-root}}"

# --- Detect atomic distro ---
is_atomic() {
    [[ -f /run/ostree-booted ]] || command -v rpm-ostree &>/dev/null
}

# --- Resolve version and paths ---
if is_atomic; then
    IS_ATOMIC=true
    SRC_DIR="/var/lib/sc0710"
    if [[ -f "$SRC_DIR/version" ]]; then
        CURRENT_VERSION="$(cat "$SRC_DIR/version" | tr -d '[:space:]')"
    else
        CURRENT_VERSION="$(curl -fsSL "$VERSION_URL" 2>/dev/null | tr -d '[:space:]')"
    fi
else
    IS_ATOMIC=false
    SRC_DIR=""
    DKMS_SRC=""
    for d in /usr/src/${DRV_NAME}-*; do
        [[ -d "$d" ]] && DKMS_SRC="$d" && break
    done
    if [[ -n "$DKMS_SRC" && -f "$DKMS_SRC/version" ]]; then
        CURRENT_VERSION="$(cat "$DKMS_SRC/version" | tr -d '[:space:]')"
    else
        CURRENT_VERSION="$(curl -fsSL "$VERSION_URL" 2>/dev/null | tr -d '[:space:]')"
    fi
fi

# --- Persistence Function ---
save_config() {
    local dbg=0
    if [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]]; then
        dbg=$(cat /sys/module/sc0710/parameters/sc0710_debug_mode 2>/dev/null || echo 0)
    elif [[ -f /sys/module/sc0710/parameters/debug ]]; then
        dbg=$(cat /sys/module/sc0710/parameters/debug 2>/dev/null || echo 0)
    fi
    local img=$(cat /sys/module/sc0710/parameters/use_status_images 2>/dev/null || echo 1)
    local smode=0
    if [[ -f /sys/module/sc0710/parameters/scaler_mode ]]; then
        smode=$(cat /sys/module/sc0710/parameters/scaler_mode 2>/dev/null || echo 0)
    fi
    local autos=1
    if [[ -f /sys/module/sc0710/parameters/auto_scaler ]]; then
        autos=$(cat /sys/module/sc0710/parameters/auto_scaler 2>/dev/null || echo 1)
    fi
    local pt=0
    if [[ -f /sys/module/sc0710/parameters/procedural_timings ]]; then
        pt=$(cat /sys/module/sc0710/parameters/procedural_timings 2>/dev/null || echo 0)
    fi
    echo "options sc0710 sc0710_debug_mode=$dbg use_status_images=$img scaler_mode=$smode auto_scaler=$autos procedural_timings=$pt" > /etc/modprobe.d/sc0710-params.conf
    echo -e "${BLUE}[PERSIST]${NC} Settings saved to /etc/modprobe.d/sc0710-params.conf"
}

sc0710_is_4k_pro_card() {
    local fw_lib=""

    if [[ "$IS_ATOMIC" == "true" && -f "$SRC_DIR/sc0710-firmware-lib.sh" ]]; then
        fw_lib="$SRC_DIR/sc0710-firmware-lib.sh"
    elif [[ -f /usr/lib/sc0710/sc0710-firmware-lib.sh ]]; then
        fw_lib="/usr/lib/sc0710/sc0710-firmware-lib.sh"
    elif [[ -f /usr/local/libexec/sc0710-firmware-lib.sh ]]; then
        fw_lib="/usr/local/libexec/sc0710-firmware-lib.sh"
    fi

    if [[ -n "$fw_lib" ]]; then
        # shellcheck source=/dev/null
        source "$fw_lib"
        sc0710_is_4k_pro
        return $?
    fi

    lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi "1cfa:0012"
}

sc0710_dkms_lib_path() {
    local path cli_dir
    for path in \
        /usr/lib/sc0710/sc0710-dkms-lib.sh \
        "$(dirname "$(sc0710_cli_path)")/sc0710-dkms-lib.sh" \
        "$(dirname "$(sc0710_cli_path)")/../scripts/sc0710-dkms-lib.sh"; do
        [[ -n "$path" && -f "$path" ]] || continue
        printf '%s\n' "$path"
        return 0
    done
    return 1
}

sc0710_version_to_dkms() {
    local ver="${1//[[:space:]]/}"
    if [[ "$ver" == *-* ]]; then
        printf '%s.%s' "${ver%-*}" "${ver##*-}"
    else
        printf '%s' "$ver"
    fi
}

sc0710_dkms_run_cleanup() {
    local ver_item dkms_lib

    if dkms_lib=$(sc0710_dkms_lib_path); then
        # shellcheck source=/dev/null
        if source "$dkms_lib" && declare -F sc0710_dkms_cleanup >/dev/null 2>&1; then
            sc0710_dkms_cleanup
            return 0
        fi
    fi

    lsmod | grep -q "^${DRV_NAME} " && rmmod "$DRV_NAME" 2>/dev/null || true
    for ver_item in $(dkms status 2>/dev/null | awk -F'[:,]' "/^${DRV_NAME}/ {print \$1}" | tr -d ' '); do
        dkms remove "$ver_item" --all >/dev/null 2>&1 || \
            rm -rf "/var/lib/dkms/$(echo "$ver_item" | tr ',' '/')" 2>/dev/null
    done
    rmdir "/var/lib/dkms/${DRV_NAME}" 2>/dev/null || true
    rm -rf /usr/src/${DRV_NAME}-*
    find /usr/lib/modules -path "*/updates/dkms/${DRV_NAME}.ko*" -delete 2>/dev/null || true
    find /usr/lib/modules -path "*/kernel/drivers/media/pci/${DRV_NAME}.ko*" -delete 2>/dev/null || true
    depmod -a >/dev/null 2>&1 || true
}

sc0710_firmware_lib_path() {
    if [[ "$IS_ATOMIC" == "true" && -f "$SRC_DIR/sc0710-firmware-lib.sh" ]]; then
        echo "$SRC_DIR/sc0710-firmware-lib.sh"
    elif [[ -f /usr/lib/sc0710/sc0710-firmware-lib.sh ]]; then
        echo "/usr/lib/sc0710/sc0710-firmware-lib.sh"
    elif [[ -f /usr/local/libexec/sc0710-firmware-lib.sh ]]; then
        echo "/usr/local/libexec/sc0710-firmware-lib.sh"
    fi
}

# Installed extract-firmware.sh, per layout: AUR, atomic source dir, DKMS source dir.
sc0710_extract_script_path() {
    local p
    for p in /usr/lib/sc0710/extract-firmware.sh \
             "$SRC_DIR/scripts/extract-firmware.sh" \
             "$DKMS_SRC/scripts/extract-firmware.sh"; do
        [[ -f "$p" ]] && { echo "$p"; return 0; }
    done
    echo "scripts/extract-firmware.sh (from the repo)"
}

sc0710_cli_path() {
    readlink -f "${BASH_SOURCE[0]}" 2>/dev/null || printf '%s' "${BASH_SOURCE[0]}"
}

sc0710_installed_aur_package() {
    local pkg owner

    command -v pacman &>/dev/null || return 1
    for pkg in sc0710-dkms-git sc0710-dkms; do
        pacman -Q "$pkg" &>/dev/null && {
            printf '%s' "$pkg"
            return 0
        }
    done

    if [[ -x /usr/bin/sc0710-cli ]]; then
        owner=$(pacman -Qo /usr/bin/sc0710-cli 2>/dev/null | awk '{print $NF}' || true)
        case "$owner" in
            sc0710-dkms-git|sc0710-dkms)
                printf '%s' "$owner"
                return 0
                ;;
        esac
    fi

    return 1
}

sc0710_detect_aur_helper() {
    local helper

    for helper in yay paru trizen pikaur aura; do
        command -v "$helper" &>/dev/null && {
            printf '%s' "$helper"
            return 0
        }
    done
    command -v pacman &>/dev/null && {
        printf '%s' "pacman"
        return 0
    }
    return 1
}

sc0710_run_as_invoke_user() {
    local user="${SC0710_INVOKE_USER:-${SUDO_USER:-root}}"

    if [[ -z "$user" || "$user" == root ]]; then
        "$@"
    else
        sudo -u "$user" -H "$@"
    fi
}

sc0710_cli_remove_user_state() {
    sc0710_remove_firmware_files
    systemctl stop sc0710-firmware.service 2>/dev/null || true
    systemctl disable sc0710-firmware.service 2>/dev/null || true
    systemctl stop sc0710-firmware-verify.service 2>/dev/null || true
    systemctl disable sc0710-firmware-verify.service 2>/dev/null || true
    rm -f /etc/systemd/system/sc0710-firmware.service
    rm -f /etc/systemd/system/sc0710-firmware-verify.service
    rm -f /etc/modules-load.d/${DRV_NAME}.conf /etc/modprobe.d/${DRV_NAME}.conf \
        /etc/modprobe.d/${DRV_NAME}-params.conf /etc/modprobe.d/${DRV_NAME}-atomic.conf
    systemctl daemon-reload 2>/dev/null || true
    rm -rf /var/log/sc0710
}

sc0710_cli_remove_aur_install() {
    local pkg="$1" helper

    helper=$(sc0710_detect_aur_helper) || {
        echo -e "${RED}[ERROR]${NC} No AUR helper or pacman found."
        return 1
    }

    echo -e "${BLUE}[INFO]${NC} AUR install detected. Removing ${BOLD}${pkg}${NC} via ${BOLD}${helper} -R ${pkg}${NC}..."
    sc0710_cli_remove_user_state

    if [[ "$helper" == pacman ]]; then
        pacman -R "$pkg" || return 1
    else
        sc0710_run_as_invoke_user "$helper" -R "$pkg" || return 1
    fi

    echo -e "${GREEN}[OK]${NC} ${pkg} removed via ${helper}."
}

sc0710_detect_install_method() {
    local pkg cli_path

    if pkg=$(sc0710_installed_aur_package); then
        printf 'AUR (%s %s)' "$pkg" "$(pacman -Q "$pkg" | awk '{print $2}')"
        return 0
    fi

    if [[ "$IS_ATOMIC" == "true" && -d /var/lib/sc0710 ]]; then
        printf 'GitHub (atomic installer)'
        return 0
    fi

    if [[ -x /usr/local/bin/sc0710-cli ]] || [[ -f /usr/local/libexec/sc0710-firmware.sh ]]; then
        printf 'GitHub (install-sc0710.sh)'
        return 0
    fi

    cli_path=$(sc0710_cli_path)
    case "$cli_path" in
        /usr/bin/sc0710-cli)
            if [[ -f /usr/lib/sc0710/sc0710-firmware-lib.sh ]]; then
                printf 'AUR (sc0710-dkms-git, package database not found)'
                return 0
            fi
            ;;
        /usr/local/bin/sc0710-cli)
            printf 'GitHub (install-sc0710.sh)'
            return 0
            ;;
    esac

    if [[ "$cli_path" != /usr/bin/sc0710-cli && "$cli_path" != /usr/local/bin/sc0710-cli ]]; then
        printf 'Development (local script: %s)' "$cli_path"
        return 0
    fi

    printf 'Unknown'
}

sc0710_cli_ensure_ecp5() {
    local attempts="${1:-3}"
    local fw_lib fw_script
    sc0710_is_4k_pro_card || return 0
    fw_lib=$(sc0710_firmware_lib_path) || return 1
    mkdir -p /var/log/sc0710
    # shellcheck source=/dev/null
    SC0710_FW_LOG_FILE="/var/log/sc0710/cli_$(date '+%Y%m%d_%H%M%S').log" source "$fw_lib"
    sc0710_init_firmware_paths
    sc0710_ensure_ecp5_programmed "$attempts"
}

sc0710_cli_atomic_load() {
    local fw_lib err

    if fw_lib=$(sc0710_firmware_lib_path 2>/dev/null); then
        mkdir -p /var/log/sc0710
        # shellcheck source=/dev/null
        SC0710_FW_LOG_FILE="/var/log/sc0710/load_$(date '+%Y%m%d_%H%M%S').log" source "$fw_lib"
        sc0710_init_firmware_paths
        sc0710_load_driver
        return $?
    fi

    local extra_dir="/lib/modules/$(uname -r)/extra/${DRV_NAME}"
    rmmod "$DRV_NAME" 2>/dev/null || true
    if [[ -d "$extra_dir" ]]; then
        rm -rf "$extra_dir"
        depmod -a "$(uname -r)" 2>/dev/null || depmod -a 2>/dev/null || true
    fi
    for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
        modprobe "$dep" 2>/dev/null || true
    done
    err=$(insmod "$SRC_DIR/build/${DRV_NAME}.ko" 2>&1) || {
        [[ -n "$err" ]] && echo "$err" >&2
        return 1
    }
    return 0
}

sc0710_cli_clear_stale_registration() {
    local fw_lib extra_dir="/lib/modules/$(uname -r)/extra/${DRV_NAME}"

    if fw_lib=$(sc0710_firmware_lib_path 2>/dev/null); then
        mkdir -p /var/log/sc0710
        # shellcheck source=/dev/null
        SC0710_FW_LOG_FILE="/var/log/sc0710/remove_$(date '+%Y%m%d_%H%M%S').log" source "$fw_lib"
        sc0710_init_firmware_paths
        sc0710_clear_stale_kernel_registration
        return 0
    fi

    rmmod "$DRV_NAME" 2>/dev/null || true
    if [[ -d "$extra_dir" ]]; then
        rm -rf "$extra_dir" 2>/dev/null || true
        depmod -a "$(uname -r)" 2>/dev/null || depmod -a 2>/dev/null || true
    fi
    if [[ "$IS_ATOMIC" == "true" ]]; then
        cat > "/etc/modprobe.d/${DRV_NAME}-atomic.conf" <<EOF
blacklist ${DRV_NAME}
EOF
    fi
}

sc0710_remove_firmware_files() {
    rm -f /lib/firmware/sc0710/SC0710.FWI.HEX 2>/dev/null || true
    rmdir /lib/firmware/sc0710 2>/dev/null || true

    rm -f /etc/firmware/sc0710/SC0710.FWI.HEX 2>/dev/null || true
    if [[ -L /etc/firmware/sc0710 ]]; then
        rm -f /etc/firmware/sc0710 2>/dev/null || true
    fi
    rmdir /etc/firmware/sc0710 2>/dev/null || true

    rm -f /var/lib/sc0710/firmware/SC0710.FWI.HEX 2>/dev/null || true
    rmdir /var/lib/sc0710/firmware 2>/dev/null || true

    rm -f /usr/local/libexec/sc0710-firmware-lib.sh 2>/dev/null || true
}

# --- Version Check Function ---
SC0710_BOX_WIDTH=59
SC0710_BOX_RULE='═══════════════════════════════════════════════════════════'

sc0710_box_border() {
    local frame="$1" type="$2"
    local left right

    case "$type" in
        top) left='╔'; right='╗' ;;
        mid) left='╠'; right='╣' ;;
        bot) left='╚'; right='╝' ;;
    esac
    printf '%b%s%s%s%b\n' "$frame" "$left" "$SC0710_BOX_RULE" "$right" "$NC"
}

sc0710_box_line() {
    local frame="$1" plain="$2" rendered="${3:-$2}"
    local pad=$(( SC0710_BOX_WIDTH - ${#plain} ))

    (( pad < 0 )) && pad=0
    printf '%b║%b%*s%b║%b\n' "$frame" "$rendered" "$pad" "" "$frame" "$NC"
}

sc0710_version_state() {
    local local_ver="$1" remote_ver="$2"

    [[ -z "$local_ver" || -z "$remote_ver" ]] && { echo "unknown"; return; }
    [[ "$local_ver" == "$remote_ver" ]] && { echo "current"; return; }
    if [[ "$(printf '%s\n' "$local_ver" "$remote_ver" | sort -V | head -1)" == "$local_ver" ]]; then
        echo "behind"
    else
        echo "ahead"
    fi
}

check_version() {
    local REMOTE_VERSION state
    REMOTE_VERSION=$(curl -fsSL "$VERSION_URL" 2>/dev/null | tr -d '[:space:]')
    [[ -z "$REMOTE_VERSION" ]] && return 0

    state=$(sc0710_version_state "$CURRENT_VERSION" "$REMOTE_VERSION")

    case "$state" in
        behind)
            echo ""
            sc0710_box_border "$YELLOW" top
            sc0710_box_line "$YELLOW" "   UPDATE AVAILABLE"
            sc0710_box_border "$YELLOW" mid
            sc0710_box_line "$YELLOW" "  Current: ${CURRENT_VERSION}" "  Current: ${RED}${CURRENT_VERSION}${NC}"
            sc0710_box_line "$YELLOW" "  Latest:  ${REMOTE_VERSION}" "  Latest:  ${GREEN}${REMOTE_VERSION}${NC}"
            sc0710_box_border "$YELLOW" mid
            sc0710_box_line "$YELLOW" "  Run sc0710-cli -U or sc0710-cli --update to update" "  Run ${BOLD}sc0710-cli -U${NC} or ${BOLD}sc0710-cli --update${NC} to update"
            sc0710_box_border "$YELLOW" bot
            echo ""
            ;;
        ahead)
            echo ""
            sc0710_box_border "$BLUE" top
            sc0710_box_line "$BLUE" "   PRE-RELEASE"
            sc0710_box_border "$BLUE" mid
            sc0710_box_line "$BLUE" "  Installed: ${CURRENT_VERSION} (newer than published)" "  Installed: ${GREEN}${CURRENT_VERSION}${NC} (newer than published)"
            sc0710_box_line "$BLUE" "  Published: ${REMOTE_VERSION}" "  Published: ${BOLD}${REMOTE_VERSION}${NC}"
            sc0710_box_border "$BLUE" mid
            sc0710_box_line "$BLUE" "  You are running a pre-release build. No update needed."
            sc0710_box_border "$BLUE" bot
            echo ""
            ;;
    esac
}

# --- Debug Dump Helpers ---
resolve_dump_desktop() {
    local home uid desktop
    if [[ "$DUMP_USER" == "root" ]]; then
        DUMP_DESKTOP="/root/Desktop"
    else
        home=$(getent passwd "$DUMP_USER" 2>/dev/null | cut -d: -f6)
        uid=$(id -u "$DUMP_USER" 2>/dev/null || echo "")
        if [[ -n "$uid" && -d "/run/user/$uid" ]]; then
            desktop=$(sudo -u "$DUMP_USER" XDG_RUNTIME_DIR="/run/user/$uid" xdg-user-dir DESKTOP 2>/dev/null || true)
        fi
        [[ -z "$desktop" && -n "$home" ]] && desktop="${home}/Desktop"
        [[ -z "$desktop" && -n "$home" ]] && desktop="$home"
        DUMP_DESKTOP="${desktop:-/root/Desktop}"
    fi
    mkdir -p "$DUMP_DESKTOP"
}

dump_section() {
    printf '\n=== %s ===\n' "$1" >> "$DUMP_FILE"
}

dump_cmd() {
    local label="$1"
    shift
    printf '\n--- %s ---\n' "$label" >> "$DUMP_FILE"
    if command -v "${1%% *}" &>/dev/null || [[ "$1" == cat ]] || [[ "$1" == ls ]]; then
        "$@" >> "$DUMP_FILE" 2>&1 || printf '(command failed: %s)\n' "$*" >> "$DUMP_FILE"
    else
        printf '(not available)\n' >> "$DUMP_FILE"
    fi
}

dump_file_if_exists() {
    local path="$1"
    if [[ -f "$path" ]]; then
        printf '\n--- %s ---\n' "$path" >> "$DUMP_FILE"
        cat "$path" >> "$DUMP_FILE" 2>&1
    else
        printf '%s: (not present)\n' "$path" >> "$DUMP_FILE"
    fi
}

get_hostname() {
    local name=""
    if [[ -f /etc/hostname ]]; then
        name=$(tr -d '[:space:]' < /etc/hostname)
    fi
    if [[ -z "$name" ]]; then
        name=$(hostname -s 2>/dev/null || hostname 2>/dev/null || uname -n 2>/dev/null || true)
    fi
    printf '%s' "${name:-unknown}"
}

write_debug_dump() {
    local dump_date dump_stamp
    dump_date=$(date '+%d-%m-%Y')
    dump_stamp=$(date '+%Y-%m-%d %H:%M:%S %Z')
    resolve_dump_desktop
    DUMP_FILE="${DUMP_DESKTOP}/dump-${dump_date}.txt"

    {
        printf 'SC0710 Debug Dump\n'
        printf 'Generated: %s\n' "$dump_stamp"
        printf 'Collected by: %s\n' "$DUMP_USER"
    } > "$DUMP_FILE"

    dump_section "System"
    {
        if [[ -f /etc/os-release ]]; then
            # shellcheck disable=SC1091
            . /etc/os-release
            printf 'Linux Distro: %s\n' "${PRETTY_NAME:-$NAME}"
            printf 'ID: %s\n' "${ID:-unknown}"
            printf 'Version: %s\n' "${VERSION_ID:-unknown}"
        else
            printf 'Linux Distro: unknown\n'
        fi
        printf 'Kernel Version: %s\n' "$(uname -r)"
        printf 'System Type: %s\n' "$([[ "$IS_ATOMIC" == "true" ]] && echo Atomic || echo Standard)"
        printf 'Architecture: %s\n' "$(uname -m)"
        printf 'Hostname: %s\n' "$(get_hostname)"
        printf 'Uptime: %s\n' "$(uptime -p 2>/dev/null || uptime 2>/dev/null || echo unknown)"
        if [[ "$IS_ATOMIC" == "true" ]]; then
            printf 'Ostree Booted: %s\n' "$([[ -f /run/ostree-booted ]] && echo yes || echo no)"
            command -v rpm-ostree &>/dev/null && printf 'rpm-ostree: available\n' || printf 'rpm-ostree: not found\n'
        fi
    } >> "$DUMP_FILE"

    dump_section "Driver"
    {
        printf 'Driver Version: %s\n' "${CURRENT_VERSION:-unknown}"
        if command -v sc0710-cli &>/dev/null || [[ -f /usr/local/bin/sc0710-cli ]]; then
            printf 'CLI Installed: yes (%s)\n' "$(command -v sc0710-cli 2>/dev/null || echo /usr/local/bin/sc0710-cli)"
        else
            printf 'CLI Installed: no\n'
        fi
        if lsmod | grep -q "^${DRV_NAME}[[:space:]]"; then
            printf 'Module Loaded: yes\n'
            lsmod | awk -v m="$DRV_NAME" '$1 == m {printf "Module Size: %s bytes\nModule Reference Count: %s\n", $2, $3}'
            awk -v m="$DRV_NAME" '$1 == m {print "Used By:", $4}' /proc/modules 2>/dev/null
        else
            printf 'Module Loaded: no\n'
        fi
        if [[ "$IS_ATOMIC" == "true" ]]; then
            printf 'Source Directory: %s\n' "$SRC_DIR"
            dump_file_if_exists "$SRC_DIR/.built-for-kernel"
        elif [[ -n "$DKMS_SRC" ]]; then
            printf 'DKMS Source Directory: %s\n' "$DKMS_SRC"
        else
            printf 'Installed Source: not found\n'
        fi
    } >> "$DUMP_FILE"

    dump_section "Install State"
    if [[ "$IS_ATOMIC" == "true" ]]; then
        dump_cmd "rpm-ostree status" bash -c "rpm-ostree status 2>/dev/null || echo '(rpm-ostree unavailable)'"
        dump_cmd "Atomic build service" systemctl status sc0710-build.service --no-pager
        dump_cmd "sc0710-build.service journal (last 50 lines)" bash -c "journalctl -u sc0710-build.service -n 50 --no-pager 2>/dev/null || echo '(no journal entries)'"
        dump_file_if_exists "$SRC_DIR/.built-for-kernel"
        dump_file_if_exists "$SRC_DIR/build-and-load.sh"
        [[ -f "$SRC_DIR/build/${DRV_NAME}.ko" ]] && printf 'Built module: %s (%s bytes)\n' "$SRC_DIR/build/${DRV_NAME}.ko" "$(stat -c %s "$SRC_DIR/build/${DRV_NAME}.ko" 2>/dev/null || echo unknown)" >> "$DUMP_FILE" \
            || printf 'Built module: not found\n' >> "$DUMP_FILE"
        printf 'Note: /etc/modules-load.d/%s.conf is not used on Atomic distros (module loaded via insmod).\n' "$DRV_NAME" >> "$DUMP_FILE"
    else
        dump_cmd "DKMS status" dkms status
        dump_cmd "DKMS status (sc0710 only)" bash -c "dkms status 2>/dev/null | grep -i sc0710 || echo '(no sc0710 DKMS entries)'"
        for d in /usr/src/${DRV_NAME}-*; do
            [[ -d "$d" ]] && printf 'DKMS source present: %s\n' "$d" >> "$DUMP_FILE"
        done
        [[ -d "/var/lib/dkms/${DRV_NAME}" ]] && printf 'DKMS lib dir present: /var/lib/dkms/%s\n' "$DRV_NAME" >> "$DUMP_FILE" \
            || printf 'DKMS lib dir: not present\n' >> "$DUMP_FILE"
    fi
    for fw in /var/lib/sc0710/firmware/SC0710.FWI.HEX /etc/firmware/sc0710/SC0710.FWI.HEX /lib/firmware/sc0710/SC0710.FWI.HEX; do
        [[ -f "$fw" ]] && printf 'Firmware present: %s\n' "$fw" >> "$DUMP_FILE"
    done

    dump_section "PCI Devices"
    dump_cmd "lspci (SC0710 / Magewell / Elgato related)" bash -c "lspci -nn 2>/dev/null | grep -iE '12ab:0710|1cfa:|magewell|sc0710' || echo '(no matching PCI devices)'"
    dump_cmd "lspci -nn (full)" lspci -nn
    dump_cmd "lspci -nnv (SC0710 device)" bash -c "lspci -nnv -d 12ab:0710 2>/dev/null || echo '(device 12ab:0710 not found)'"

    dump_section "Video Devices"
    dump_cmd "Video device nodes" bash -c "ls -la /dev/video* 2>/dev/null || echo '(no /dev/video* nodes)'"
    dump_cmd "v4l2 device list" bash -c "v4l2-ctl --list-devices 2>/dev/null || echo '(v4l2-ctl not installed)'"
    dump_cmd "v4l2loopback module" bash -c "lsmod | grep -E '^v4l2loopback|Module' || echo '(v4l2loopback not loaded)'"
    if lsmod | grep -q "^${DRV_NAME}[[:space:]]"; then
        dump_cmd "Driver-bound PCI devices" bash -c "ls -d /sys/bus/pci/drivers/${DRV_NAME}/0* 2>/dev/null | while read -r p; do echo \"\$(basename \"\$p\")\"; done || echo '(none bound)'"
    fi

    dump_section "Device Usage"
    dump_cmd "Processes using video devices (fuser)" bash -c "fuser -v /dev/video* 2>&1 || echo '(none or fuser unavailable)'"
    dump_cmd "Processes using video devices (lsof)" bash -c "lsof /dev/video* 2>/dev/null || echo '(none or lsof unavailable)'"
    dump_cmd "Processes using audio devices (fuser)" bash -c "fuser -v /dev/snd/* 2>&1 || echo '(none or fuser unavailable)'"

    dump_section "Configuration"
    dump_file_if_exists "/etc/modules-load.d/${DRV_NAME}.conf"
    dump_file_if_exists "/etc/modprobe.d/${DRV_NAME}.conf"
    dump_file_if_exists "/etc/modprobe.d/${DRV_NAME}-atomic.conf"
    dump_file_if_exists "/etc/modprobe.d/${DRV_NAME}-params.conf"
    dump_file_if_exists "/etc/systemd/system/sc0710-build.service"
    dump_file_if_exists "/etc/systemd/system/sc0710-firmware.service"
    if [[ "$DUMP_USER" != "root" ]]; then
        printf '\n--- User groups (%s) ---\n' "$DUMP_USER" >> "$DUMP_FILE"
        groups "$DUMP_USER" >> "$DUMP_FILE" 2>&1 || true
    fi

    dump_section "Module Details"
    if lsmod | grep -q "^${DRV_NAME}[[:space:]]"; then
        if modinfo "$DRV_NAME" &>/dev/null; then
            dump_cmd "modinfo" modinfo "$DRV_NAME"
        elif [[ "$IS_ATOMIC" == "true" && -f "$SRC_DIR/build/${DRV_NAME}.ko" ]]; then
            dump_cmd "modinfo (built module)" modinfo "$SRC_DIR/build/${DRV_NAME}.ko"
        fi
        dump_cmd "Module parameters" bash -c "ls -la /sys/module/${DRV_NAME}/parameters/ 2>/dev/null && for p in /sys/module/${DRV_NAME}/parameters/*; do printf '%s=%s\n' \"\$(basename \"\$p\")\" \"\$(cat \"\$p\" 2>/dev/null)\"; done"
        dump_file_if_exists "/proc/sc0710-state"
    else
        printf 'Module not loaded — skipping live parameters.\n' >> "$DUMP_FILE"
        if [[ "$IS_ATOMIC" == "true" && -f "$SRC_DIR/build/${DRV_NAME}.ko" ]]; then
            dump_cmd "modinfo (built module)" modinfo "$SRC_DIR/build/${DRV_NAME}.ko"
        else
            dump_cmd "modinfo (if module file exists)" bash -c "modinfo ${DRV_NAME} 2>/dev/null || echo '(module not available in kernel tree)'"
        fi
    fi

    dump_section "PipeWire / Audio Stack"
    dump_cmd "PipeWire processes" bash -c "pgrep -a pipewire 2>/dev/null || echo '(pipewire not running)'"
    if [[ "$DUMP_USER" != "root" ]]; then
        uid=$(id -u "$DUMP_USER" 2>/dev/null || echo "")
        if [[ -n "$uid" ]]; then
            dump_cmd "PipeWire user services ($DUMP_USER)" bash -c "sudo -u '#$uid' XDG_RUNTIME_DIR='/run/user/$uid' systemctl --user status pipewire.socket pipewire.service wireplumber.service --no-pager 2>&1"
        fi
    fi

    dump_section "Kernel Log (sc0710)"
    dump_cmd "dmesg (sc0710, last 150 lines)" bash -c "dmesg 2>/dev/null | grep -i sc0710 | tail -150 || echo '(no sc0710 messages in dmesg)'"
    dump_cmd "dmesg (v4l2loopback)" bash -c "dmesg 2>/dev/null | grep -i v4l2loopback | tail -50 || echo '(no v4l2loopback messages)'"

    dump_section "Install Logs"
    if [[ -d /var/log/sc0710 ]]; then
        dump_cmd "Install log directory listing" ls -la /var/log/sc0710
        for log in /var/log/sc0710/*; do
            [[ -f "$log" ]] && dump_cmd "$(basename "$log") (last 80 lines)" bash -c "tail -80 '$log'"
        done
    else
        printf '/var/log/sc0710: (not present)\n' >> "$DUMP_FILE"
    fi

    if [[ "$DUMP_USER" != "root" ]]; then
        chown "$DUMP_USER:$DUMP_USER" "$DUMP_FILE" 2>/dev/null || true
    fi
    chmod 0644 "$DUMP_FILE" 2>/dev/null || true

    echo -e "${GREEN}[OK]${NC} Debug dump written to ${BOLD}${DUMP_FILE}${NC}"
    echo -e "${BLUE}[INFO]${NC} Attach this file when reporting issues on GitHub."
}

# --- Help Function ---
show_help() {
    if [[ "$IS_ATOMIC" == "true" ]]; then
        echo -e "${BOLD}SC0710${NC} Driver Control Utility v${CURRENT_VERSION} (Atomic Edition)"
    else
        echo -e "${BOLD}SC0710${NC} Driver Control Utility v${CURRENT_VERSION}"
    fi
    echo ""
    echo -e "${BOLD}USAGE:${NC}"
    echo -e "    sc0710-cli [OPTION]"
    echo ""
    echo -e "${BOLD}OPTIONS:${NC}"
    echo -e "    ${BOLD}-l, --load${NC}       Load the driver module"
    echo -e "    ${BOLD}-u, --unload${NC}     Unload the driver module"
    echo -e "    ${BOLD}--restart${NC}        Restart the driver module"
    echo -e "    ${BOLD}-s, --status${NC}     Show module and build status"
    echo -e "    ${BOLD}-d, --debug${NC}      Toggle debug mode on/off"
    echo -e "    ${BOLD}-it, --image-toggle${NC} Toggle status images on/off"
    echo -e "    ${BOLD}-ss, --software-scaler${NC} Toggle software scaler modes (all cards)"
    echo -e "    ${BOLD}-as, --toggle-auto-scalar${NC} Toggle automatic safety scaler on/off"
    echo -e "    ${BOLD}-pt, --procedural-timings${NC} Toggle timing calculation mode (merge/procedural/static)"
    echo -e "    ${BOLD}-U, --update${NC}     Check for updates and reinstall"
    echo -e "    ${BOLD}-r, -R, --remove${NC} Completely uninstall driver and CLI (AUR: uses yay/paru)"
    echo -e "    ${BOLD}--dump${NC}           Save a debug report to the Desktop"
    if [[ "$IS_ATOMIC" == "true" ]]; then
        echo -e "    ${BOLD}--rebuild${NC}        Force rebuild the module for current kernel"
    fi
    echo -e "    ${BOLD}-v, --version${NC}    Show version information"
    echo -e "    ${BOLD}-h, --help${NC}       Show this help message"
    echo ""
}

# --- No Arguments Handler ---
if [[ $# -eq 0 ]]; then
    if [[ "$IS_ATOMIC" == "true" ]]; then
        echo -e "${BOLD}SC0710${NC} Driver Control Utility (Atomic Edition)"
    else
        echo -e "${BOLD}SC0710${NC} Driver Control Utility"
    fi
    echo -e "Use ${BOLD}-h${NC} or ${BOLD}--help${NC} for usage information."
    exit 0
fi

# --- Command Handler ---
case "$1" in
    -l|--load)
        if lsmod | grep -q "$DRV_NAME"; then
            if sc0710_is_4k_pro_card && sc0710_firmware_lib_path >/dev/null; then
                echo -e "${BLUE}::${NC} Driver loaded — verifying ECP5 FPGA..."
                if sc0710_cli_ensure_ecp5 3; then
                    echo -e "${GREEN}[OK]${NC} Driver loaded and ECP5 FPGA programmed."
                else
                    echo -e "${RED}[ERROR]${NC} ECP5 programming failed."
                    echo -e "  Run: ${BOLD}sc0710-cli --restart${NC}"
                    exit 1
                fi
            else
                echo -e "${GREEN}[OK]${NC} Driver is already loaded."
            fi
            exit 0
        fi
        echo -e "${BLUE}::${NC} Loading driver..."
        if [[ "$IS_ATOMIC" == "true" ]]; then
            if sc0710_cli_atomic_load; then
                if sc0710_is_4k_pro_card && sc0710_firmware_lib_path >/dev/null; then
                    # shellcheck source=/dev/null
                    SC0710_FW_LOG_FILE="/var/log/sc0710/load_$(date '+%Y%m%d_%H%M%S').log" source "$(sc0710_firmware_lib_path)"
                    mkdir -p /var/log/sc0710
                    sc0710_init_firmware_paths
                    if sc0710_card_bound || sc0710_cli_ensure_ecp5 3; then
                        echo -e "${GREEN}[OK]${NC} Driver loaded successfully."
                    else
                        echo -e "${RED}[ERROR]${NC} Driver loaded but ECP5 programming failed."
                        echo -e "  Run: ${BOLD}sc0710-cli --restart${NC}"
                        exit 1
                    fi
                else
                    echo -e "${GREEN}[OK]${NC} Driver loaded successfully."
                fi
            elif [[ ! -f "$SRC_DIR/build/${DRV_NAME}.ko" ]]; then
                echo -e "${RED}[ERROR]${NC} Module not found. Run ${BOLD}sc0710-cli --rebuild${NC} first."
            else
                echo -e "${RED}[ERROR]${NC} Failed to load driver. Run ${BOLD}sc0710-cli --rebuild${NC} if kernel was updated."
                echo -e "  Check: ${BOLD}journalctl -u sc0710-build.service -b${NC}"
            fi
        else
            for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
                modprobe "$dep" 2>/dev/null || true
            done
            if modprobe "$DRV_NAME"; then
                if sc0710_is_4k_pro_card && sc0710_firmware_lib_path >/dev/null; then
                    if sc0710_cli_ensure_ecp5 3; then
                        echo -e "${GREEN}[OK]${NC} Driver loaded and ECP5 FPGA programmed."
                    else
                        echo -e "${RED}[ERROR]${NC} Driver loaded but ECP5 programming failed."
                        echo -e "  Run: ${BOLD}sc0710-cli --restart${NC}"
                        exit 1
                    fi
                else
                    echo -e "${GREEN}[OK]${NC} Driver loaded successfully."
                fi
            else
                echo -e "${RED}[ERROR]${NC} Failed to load driver."
            fi
        fi
        ;;
    -u|--unload)
        if ! lsmod | grep -q "$DRV_NAME"; then
            echo -e "${GREEN}[OK]${NC} Driver is not loaded."
            exit 0
        fi
        echo -e "${BLUE}::${NC} Unloading driver..."

        if [[ "$IS_ATOMIC" == "true" ]]; then
            if rmmod "$DRV_NAME" 2>/dev/null; then
                echo -e "${GREEN}[OK]${NC} Driver unloaded successfully."
                exit 0
            fi
            echo -e "${YELLOW}[BUSY]${NC} Module is in use. Stopping PipeWire and consumers..."
            PW_UIDS=()
            while read -r pid; do
                uid=$(stat -c %u "/proc/$pid" 2>/dev/null) || continue
                PW_UIDS+=("$uid")
            done < <(pgrep -x pipewire 2>/dev/null || true)
            for uid in $(printf '%s\n' "${PW_UIDS[@]}" | sort -u); do
                sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
                    systemctl --user stop pipewire.socket pipewire.service wireplumber.service 2>/dev/null || true
            done
            for vdev in /dev/video*; do
                [[ -e "$vdev" ]] && fuser -k "$vdev" >/dev/null 2>&1 || true
            done
            for sdev in /dev/snd/*; do
                [[ -e "$sdev" ]] && fuser -k "$sdev" >/dev/null 2>&1 || true
            done
            sleep 1
            if rmmod "$DRV_NAME" 2>/dev/null; then
                echo -e "${GREEN}[OK]${NC} Driver unloaded successfully."
            else
                sleep 2
                if rmmod "$DRV_NAME" 2>/dev/null; then
                    echo -e "${GREEN}[OK]${NC} Driver unloaded successfully."
                else
                    echo -e "${RED}[ERROR]${NC} Module is still held by the kernel."
                    for uid in $(printf '%s\n' "${PW_UIDS[@]}" | sort -u); do
                        sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
                            systemctl --user start pipewire.socket 2>/dev/null || true
                    done
                    exit 1
                fi
            fi
            echo -e "${BLUE}[INFO]${NC} Restarting PipeWire..."
            for uid in $(printf '%s\n' "${PW_UIDS[@]}" | sort -u); do
                sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
                    systemctl --user start pipewire.socket 2>/dev/null || true
            done
        else
            if ! rmmod "$DRV_NAME" 2>/dev/null; then
                echo -e "${YELLOW}[BUSY]${NC} Standard unload failed. Attempting force..."
                fuser -k /dev/video* >/dev/null 2>&1 || true
                sleep 0.5
                if rmmod -f "$DRV_NAME" 2>/dev/null; then
                    echo -e "${GREEN}[OK]${NC} Driver force-unloaded successfully."
                else
                    echo -e "${RED}[ERROR]${NC} Kernel refused to force unload. A reboot may be required."
                fi
            else
                echo -e "${GREEN}[OK]${NC} Driver unloaded successfully."
            fi
        fi
        ;;
    --restart)
        # Always a real unload+load, even when the card is bound and healthy:
        # a reload is how new firmware/EDID files and module params get picked
        # up. --load re-verifies the ECP5 on the 4K Pro afterwards.
        "$0" --unload
        sleep 1
        exec "$0" --load
        ;;
    -s|--status)
        check_version
        echo -e "${BLUE}::${NC} ${BOLD}Install Method${NC}"
        INSTALL_METHOD="$(sc0710_detect_install_method)"
        case "$INSTALL_METHOD" in
            AUR*)
                echo -e "   ${GREEN}●${NC} ${INSTALL_METHOD}"
                ;;
            GitHub*)
                echo -e "   ${GREEN}●${NC} ${INSTALL_METHOD}"
                ;;
            Development*)
                echo -e "   ${YELLOW}●${NC} ${INSTALL_METHOD}"
                ;;
            *)
                echo -e "   ${YELLOW}○${NC} ${INSTALL_METHOD}"
                ;;
        esac
        echo ""
        if [[ "$IS_ATOMIC" == "true" ]]; then
            echo -e "${BLUE}::${NC} ${BOLD}System Type${NC}"
            echo -e "   Atomic/Immutable (boot-time build)"
            if [[ -f "$SRC_DIR/.built-for-kernel" ]]; then
                echo -e "   Last built for: ${BOLD}$(cat "$SRC_DIR/.built-for-kernel")${NC}"
            fi
            echo -e "   Running kernel:  ${BOLD}$(uname -r)${NC}"
            echo ""
            echo -e "${BLUE}::${NC} ${BOLD}Systemd Service${NC}"
            if systemctl is-enabled sc0710-build.service >/dev/null 2>&1; then
                echo -e "   ${GREEN}●${NC} sc0710-build.service is enabled"
            else
                echo -e "   ${RED}○${NC} sc0710-build.service is disabled"
            fi
            if systemctl is-active sc0710-build.service >/dev/null 2>&1; then
                echo -e "   ${GREEN}●${NC} Last boot build: succeeded"
            else
                echo -e "   ${YELLOW}○${NC} Last boot build: not run or failed"
            fi
        else
            echo -e "${BLUE}::${NC} ${BOLD}DKMS Status${NC}"
            dkms status "$DRV_NAME" 2>/dev/null || echo "   DKMS not configured for this driver."
        fi
        echo ""
        echo -e "${BLUE}::${NC} ${BOLD}Kernel Module${NC}"
        if lsmod | grep -q "$DRV_NAME"; then
            echo -e "   ${GREEN}●${NC} Module is loaded"
            MOD_INFO=$(lsmod | grep "$DRV_NAME" | head -1)
            MOD_SIZE=$(echo "$MOD_INFO" | awk '{print $2}')
            MOD_USED=$(echo "$MOD_INFO" | awk '{print $3}')
            echo "   Size: $MOD_SIZE bytes, Reference count: $MOD_USED"
            if [[ "$MOD_USED" -gt 0 ]]; then
                PIDS=""
                for vdev in /dev/video*; do
                    if [[ -e "$vdev" ]]; then
                        DEVPIDS=$(fuser "$vdev" 2>/dev/null | tr -s ' ')
                        [[ -n "$DEVPIDS" ]] && PIDS="$PIDS $DEVPIDS"
                    fi
                done
                for sdev in /dev/snd/*; do
                    if [[ -e "$sdev" ]]; then
                        DEVPIDS=$(fuser "$sdev" 2>/dev/null | tr -s ' ')
                        [[ -n "$DEVPIDS" ]] && PIDS="$PIDS $DEVPIDS"
                    fi
                done
                if [[ -n "$PIDS" ]]; then
                    echo -e "   ${YELLOW}Processes holding device open:${NC}"
                    echo "$PIDS" | tr ' ' '\n' | sort -un | while read -r pid; do
                        [[ -n "$pid" && -f "/proc/$pid/comm" ]] && echo -e "     PID ${BOLD}$pid${NC} - $(cat /proc/$pid/comm 2>/dev/null)"
                    done
                else
                    echo -e "   ${YELLOW}No open device handles found (kernel-internal reference?)${NC}"
                fi
            fi
        else
            echo -e "   ${RED}○${NC} Module is not loaded"
        fi
        echo ""
        # Card Information (shared)
        echo -e "${BLUE}::${NC} ${BOLD}Card Information${NC}"
        if lsmod | grep -q "$DRV_NAME"; then
            FOUND_CARDS=0
            for pcidir in /sys/bus/pci/drivers/sc0710/0*; do
                if [[ -d "$pcidir" ]]; then
                    FOUND_CARDS=1
                    PCI_ADDR=$(basename "$pcidir")
                    SUBVEN=$(cat "$pcidir/subsystem_vendor" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//')
                    SUBDEV=$(cat "$pcidir/subsystem_device" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//')
                    VEN=$(cat "$pcidir/vendor" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//')
                    DEV=$(cat "$pcidir/device" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//')
                    BOARD_NAME=$(dmesg 2>/dev/null | grep -E "sc0710.*subsystem: ${SUBVEN}:${SUBDEV}.*board:" | tail -1 | sed 's/.*board: \([^\[]*\).*/\1/' | sed 's/ *$//')
                    if [[ -z "$BOARD_NAME" ]]; then
                        if fw_lib=$(sc0710_firmware_lib_path 2>/dev/null); then
                            # shellcheck source=/dev/null
                            source "$fw_lib"
                            BOARD_NAME=$(sc0710_board_name_from_subsys "${SUBVEN}:${SUBDEV}")
                        else
                            case "$SUBVEN:$SUBDEV" in
                                1cfa:000e) BOARD_NAME="Elgato 4k60 Pro MK.2" ;;
                                1cfa:0012) BOARD_NAME="Elgato 4K Pro" ;;
                                1cfa:0006) BOARD_NAME="Elgato HD60 Pro (1cfa:0006)" ;;
                                *) BOARD_NAME="UNKNOWN/GENERIC" ;;
                            esac
                        fi
                    fi
                    echo -e "   ${GREEN}●${NC} Device at PCI ${BOLD}${PCI_ADDR}${NC}"
                    echo -e "     Board: ${BOARD_NAME}"
                    echo -e "     Hardware: ${VEN}:${DEV} (Subsys: ${SUBVEN}:${SUBDEV})"
                    if [[ "$SUBVEN:$SUBDEV" == "1cfa:0006" ]]; then
                        echo -e "     ${RED}⚠ WARNING:${NC} This is an Elgato HD60 Pro."
                        echo -e "     ${RED}          ${NC} It is an entirely different chipset and is ${BOLD}INCOMPATIBLE${NC} with this driver."
                    fi
                fi
            done
            [[ $FOUND_CARDS -eq 0 ]] && echo -e "   ${YELLOW}○${NC} No devices found currently bound to driver"
        else
            echo -e "   ${RED}○${NC} Module is not loaded, cannot retrieve card info"
        fi
        # Signal Status, Scaler, Debug, Status Images, Timing, ECP5 (shared - use source from atomic as base)
        echo ""
        echo -e "${BLUE}::${NC} ${BOLD}Signal Status${NC}"
        if [[ -f /proc/sc0710-state ]]; then
            PROC_INFO=$(cat /proc/sc0710-state 2>/dev/null)
            HDMI_LINE=$(echo "$PROC_INFO" | grep "HDMI:" | head -1)
            if [[ -n "$HDMI_LINE" ]]; then
                if echo "$HDMI_LINE" | grep -q "no signal"; then
                    echo -e "   ${YELLOW}○${NC} No signal detected"
                else
                    FMT_NAME=$(echo "$HDMI_LINE" | sed 's/.*HDMI: \([^ ]*\).*/\1/')
                    RESOLUTION=$(echo "$HDMI_LINE" | sed 's/.*-- \([^ ]*\).*/\1/')
                    TIMING=$(echo "$HDMI_LINE" | grep -oP '\([0-9]+x[0-9]+\)' | tr -d '()')
                    echo -e "   ${GREEN}●${NC} Signal locked"
                    echo -e "   Format: ${BOLD}${FMT_NAME}${NC}"
                    [[ -n "$RESOLUTION" && "$RESOLUTION" != "$HDMI_LINE" ]] && echo -e "   Resolution: ${RESOLUTION}"
                    [[ -n "$TIMING" ]] && echo -e "   Total timing: ${TIMING}"
                fi
                SCALER_LINE=$(echo "$PROC_INFO" | grep "^      scaler:" | head -1)
                AUTO_SCALER_LINE=$(echo "$PROC_INFO" | grep "^ auto scaler:" | head -1)
                AUTO_SCALER_CFG_LINE=$(echo "$PROC_INFO" | grep "^ auto scaler cfg:" | head -1)
                if [[ -n "$SCALER_LINE" ]]; then
                    echo ""
                    echo -e "${BLUE}::${NC} ${BOLD}Scaler Information${NC}"
                    SCALER_MODE=$(echo "$SCALER_LINE" | sed 's/.*scaler: \([^ ]*.*\)/\1/')
                    [[ "$SCALER_MODE" == "DISABLED" ]] && echo -e "   Software Scaler: ${YELLOW}DISABLED${NC}" || echo -e "   Software Scaler: ${GREEN}${SCALER_MODE}${NC}"
                    if [[ -n "$AUTO_SCALER_LINE" ]]; then
                        AUTO_VAL=$(echo "$AUTO_SCALER_LINE" | sed 's/.*auto scaler: \(.*\)/\1/')
                        echo "$AUTO_VAL" | grep -q "ON" && echo -e "   Auto Scaler: ${GREEN}ON (Prevented Kernel Panic)${NC}" || echo -e "   Auto Scaler: ${BLUE}OFF${NC}"
                    fi
                    # Auto-Scaler Config and Dynamic Resolution are mutually exclusive (only one active at a time)
                    if [[ -n "$AUTO_SCALER_CFG_LINE" ]]; then
                        AUTO_CFG_VAL=$(echo "$AUTO_SCALER_CFG_LINE" | sed 's/.*auto scaler cfg: \([A-Z]*\).*/\1/')
                        if [[ "$AUTO_CFG_VAL" == "ENABLED" ]]; then
                            echo -e "   Auto-Scaler Config: ${GREEN}ENABLED${NC}"
                        else
                            echo -e "   Auto-Scaler Config: ${YELLOW}DISABLED${NC}"
                        fi
                    fi
                    DYN_RES_LINE=$(echo "$PROC_INFO" | grep "^ dynamic res:" | head -1)
                    if [[ -n "$DYN_RES_LINE" ]]; then
                        DYN_RES_VAL=$(echo "$DYN_RES_LINE" | sed 's/.*dynamic res: \([A-Z]*\).*/\1/')
                        if [[ "$DYN_RES_VAL" == "ACTIVE" ]]; then
                            echo -e "   Dynamic Resolution: ${GREEN}ACTIVE${NC}"
                        else
                            echo -e "   Dynamic Resolution: ${YELLOW}INACTIVE${NC}"
                        fi
                    else
                        echo -e "   Dynamic Resolution: ${YELLOW}INACTIVE${NC}"
                    fi
                fi
            else
                echo -e "   ${RED}○${NC} Could not read HDMI status"
            fi
        else
            LAST_FMT=$(dmesg 2>/dev/null | grep -E "sc0710.*Detected timing|sc0710.*DTC created" | tail -1)
            if [[ -n "$LAST_FMT" ]]; then
                echo -e "   Last detected: $(echo "$LAST_FMT" | sed 's/.*sc0710[^:]*: //')"
            else
                echo -e "   ${RED}○${NC} No signal info available (check dmesg)"
            fi
        fi
        echo ""
        echo -e "${BLUE}::${NC} ${BOLD}Debug Mode${NC}"
        DBG_PATH=""
        [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]] && DBG_PATH=/sys/module/sc0710/parameters/sc0710_debug_mode
        [[ -f /sys/module/sc0710/parameters/debug ]] && DBG_PATH=/sys/module/sc0710/parameters/debug
        if [[ -n "$DBG_PATH" ]]; then
            DBG_STATE=$(cat "$DBG_PATH")
            [[ "$DBG_STATE" == "1" ]] && echo -e "   ${YELLOW}●${NC} Debug mode enabled" || echo -e "   ${GREEN}○${NC} Debug mode disabled"
        else
            echo -e "   ${RED}○${NC} Parameter not available (module not loaded)"
        fi
        echo ""
        echo -e "${BLUE}::${NC} ${BOLD}Status Images${NC}"
        if [[ -f /sys/module/sc0710/parameters/use_status_images ]]; then
            [[ "$(cat /sys/module/sc0710/parameters/use_status_images)" == "1" ]] && echo -e "   ${GREEN}●${NC} Status images enabled" || echo -e "   ${YELLOW}○${NC} Status images disabled"
        else
            echo -e "   ${RED}○${NC} Parameter not available (module not loaded)"
        fi
        echo ""
        echo -e "${BLUE}::${NC} ${BOLD}Timing Calculation${NC}"
        if [[ -f /sys/module/sc0710/parameters/procedural_timings ]]; then
            PT_STATE=$(cat /sys/module/sc0710/parameters/procedural_timings 2>/dev/null || echo 0)
            case "$PT_STATE" in 1) echo -e "   ${YELLOW}●${NC} PROCEDURAL_ONLY";; 2) echo -e "   ${YELLOW}●${NC} STATIC_ONLY";; *) echo -e "   ${GREEN}●${NC} MERGE";; esac
        elif [[ -f /proc/sc0710-state ]]; then
            PT_LINE=$(grep "^ timing calc:" /proc/sc0710-state 2>/dev/null | head -1)
            [[ -n "$PT_LINE" ]] && echo -e "   ${YELLOW}●${NC}${PT_LINE# timing calc: }" || echo -e "   ${RED}○${NC} Parameter not available"
        else
            echo -e "   ${RED}○${NC} Parameter not available (module not loaded)"
        fi
        # ECP5 Firmware Status
        if sc0710_is_4k_pro_card; then
            echo ""
            echo -e "${BLUE}::${NC} ${BOLD}ECP5 Firmware${NC}"
            FW_FOUND=false
            for p in /var/lib/sc0710/firmware/SC0710.FWI.HEX /etc/firmware/sc0710/SC0710.FWI.HEX /lib/firmware/sc0710/SC0710.FWI.HEX; do
                if [[ -f "$p" ]]; then FW_FOUND=true; echo -e "   ${GREEN}●${NC} Firmware present: $p"; break; fi
            done
            [[ "$FW_FOUND" == "false" ]] && echo -e "   ${RED}○${NC} Firmware missing. Run: ${BOLD}sudo bash $(sc0710_extract_script_path)${NC}"
            # The driver fails its probe when the ECP5 can't be programmed, so
            # bind state is the FPGA state — no kernel-log parsing.
            if ls /sys/bus/pci/drivers/${DRV_NAME}/0000:* >/dev/null 2>&1; then
                echo -e "   ${GREEN}●${NC} Driver bound — ECP5 FPGA programmed"
            elif lsmod | grep -q "^${DRV_NAME}[[:space:]]"; then
                echo -e "   ${RED}○${NC} Driver loaded but card NOT bound — probe failed (ECP5/firmware)."
                echo -e "     Check: ${BOLD}sudo dmesg | grep sc0710${NC}"
                echo -e "     Then:  ${BOLD}sc0710-cli --restart${NC}"
            else
                echo -e "   ${YELLOW}○${NC} Driver not loaded"
            fi
        fi
        echo ""
        ;;
    -d|--debug)
        DBG_PATH=""
        [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]] && DBG_PATH=/sys/module/sc0710/parameters/sc0710_debug_mode
        [[ -f /sys/module/sc0710/parameters/debug ]] && DBG_PATH=/sys/module/sc0710/parameters/debug
        if [[ -z "$DBG_PATH" ]]; then
            echo -e "${RED}[ERROR]${NC} Module not loaded. Load it first with: sc0710-cli --load"
            exit 1
        fi
        CURRENT=$(cat "$DBG_PATH")
        if [[ "$CURRENT" == "1" ]]; then
            echo 0 > "$DBG_PATH"
            echo -e "${GREEN}[OK]${NC} Debug mode disabled"
        else
            echo 1 > "$DBG_PATH"
            echo -e "${YELLOW}[OK]${NC} Debug mode enabled"
        fi
        save_config
        ;;
    -it|--image-toggle)
        if [[ ! -f /sys/module/sc0710/parameters/use_status_images ]]; then
            echo -e "${RED}[ERROR]${NC} Module not loaded. Load it first with: sc0710-cli --load"
            exit 1
        fi
        CURRENT=$(cat /sys/module/sc0710/parameters/use_status_images)
        if [[ "$CURRENT" == "1" ]]; then
            echo 0 > /sys/module/sc0710/parameters/use_status_images
            echo -e "${YELLOW}[OK]${NC} Status images disabled"
        else
            echo 1 > /sys/module/sc0710/parameters/use_status_images
            echo -e "${GREEN}[OK]${NC} Status images enabled"
        fi
        save_config
        ;;
    -ss|--software-scaler)
        if [[ ! -f /sys/module/sc0710/parameters/scaler_mode ]]; then
            echo -e "${RED}[ERROR]${NC} Module not loaded. Load it first with: sc0710-cli --load"
            exit 1
        fi
        CURRENT=$(cat /sys/module/sc0710/parameters/scaler_mode)
        case "${CURRENT:-0}" in
            0) echo 1 > /sys/module/sc0710/parameters/scaler_mode; echo -e "${GREEN}[OK]${NC} Software Scaler enabled: Upscale Mode" ;;
            1) echo 2 > /sys/module/sc0710/parameters/scaler_mode; echo -e "${YELLOW}[OK]${NC} Software Scaler: Downscale Mode" ;;
            *) echo 0 > /sys/module/sc0710/parameters/scaler_mode; echo -e "${BLUE}[OK]${NC} Software Scaler disabled" ;;
        esac
        save_config
        ;;
    -as|--toggle-auto-scaler|--toggle-auto-scalar)
        if [[ ! -f /sys/module/sc0710/parameters/auto_scaler ]]; then
            echo -e "${RED}[ERROR]${NC} auto_scaler parameter not available."
            exit 1
        fi
        CURRENT=$(cat /sys/module/sc0710/parameters/auto_scaler)
        if [[ "$CURRENT" == "1" ]]; then
            echo 0 > /sys/module/sc0710/parameters/auto_scaler
            echo -e "${YELLOW}[OK]${NC} Automatic safety scaler disabled"
        else
            echo 1 > /sys/module/sc0710/parameters/auto_scaler
            echo -e "${GREEN}[OK]${NC} Automatic safety scaler enabled"
        fi
        save_config
        ;;
    -pt|--procedural-timings)
        if [[ ! -f /sys/module/sc0710/parameters/procedural_timings ]]; then
            echo -e "${RED}[ERROR]${NC} procedural_timings parameter not available."
            exit 1
        fi
        CURRENT=$(cat /sys/module/sc0710/parameters/procedural_timings)
        case "${CURRENT:-0}" in
            0) echo 1 > /sys/module/sc0710/parameters/procedural_timings; echo -e "${YELLOW}[OK]${NC} PROCEDURAL_ONLY" ;;
            1) echo 2 > /sys/module/sc0710/parameters/procedural_timings; echo -e "${YELLOW}[OK]${NC} STATIC_ONLY" ;;
            *) echo 0 > /sys/module/sc0710/parameters/procedural_timings; echo -e "${GREEN}[OK]${NC} MERGE" ;;
        esac
        save_config
        ;;
    -U|--update)
        echo -e "${BLUE}::${NC} Checking for updates..."
        if [[ "$IS_ATOMIC" == "true" ]]; then
            [[ ! -d "$SRC_DIR" ]] && { echo -e "${RED}[ERROR]${NC} Source directory missing."; exit 1; }
            TEMP_TAR=$(mktemp /tmp/sc0710-update.XXXXXX.tar.gz)
            curl -fsSL "https://github.com/Nakildias/sc0710/archive/refs/heads/main.tar.gz" -o "$TEMP_TAR" || { echo -e "${RED}[ERROR]${NC} Download failed."; rm -f "$TEMP_TAR"; exit 1; }
            tar -xzf "$TEMP_TAR" --strip-components=1 -C "$SRC_DIR" || { rm -f "$TEMP_TAR"; exit 1; }
            rm -f "$TEMP_TAR"
            [[ -f "$SRC_DIR/scripts/build-and-load.sh" ]] && cp "$SRC_DIR/scripts/build-and-load.sh" "$SRC_DIR/build-and-load.sh" && chmod +x "$SRC_DIR/build-and-load.sh"
            [[ -f "$SRC_DIR/scripts/sc0710-firmware-lib.sh" ]] && cp "$SRC_DIR/scripts/sc0710-firmware-lib.sh" "$SRC_DIR/sc0710-firmware-lib.sh" && chmod +x "$SRC_DIR/sc0710-firmware-lib.sh"
            NEW_VER="$CURRENT_VERSION"
            [[ -f "$SRC_DIR/version" ]] && NEW_VER=$(cat "$SRC_DIR/version" | tr -d '[:space:]')
            lsmod | grep -q "$DRV_NAME" && "$0" --unload
            rm -f "$SRC_DIR/.built-for-kernel"
            cd "$SRC_DIR"
            make clean 2>/dev/null || true
            if make KVERSION="$(uname -r)" -j"$(nproc)" 2>&1; then
                echo "$(uname -r)" > "$SRC_DIR/.built-for-kernel"
                chcon -t modules_object_t "$SRC_DIR/build/${DRV_NAME}.ko" 2>/dev/null || true
                for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do modprobe "$dep" 2>/dev/null || true; done
                if sc0710_cli_atomic_load; then
                    if sc0710_is_4k_pro_card && sc0710_firmware_lib_path >/dev/null; then
                        if sc0710_cli_ensure_ecp5 5; then
                            echo -e "${GREEN}[OK]${NC} Driver updated (v${NEW_VER}), ECP5 FPGA programmed."
                        else
                            echo -e "${YELLOW}[WARNING]${NC} Driver updated (v${NEW_VER}) but ECP5 programming failed."
                            echo -e "  Run: ${BOLD}sc0710-cli --restart${NC}"
                        fi
                    else
                        echo -e "${GREEN}[OK]${NC} Driver updated (v${NEW_VER})."
                    fi
                else
                    echo -e "${YELLOW}[WARNING]${NC} Try: sc0710-cli --load"
                    [[ ! -f "$SRC_DIR/build/${DRV_NAME}.ko" ]] && echo -e "   ${YELLOW}If module doesn't exist, please reinstall using:${NC}" && echo -e "   sudo bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/scripts/install-sc0710.sh)\""
                fi
            else
                echo -e "${RED}[ERROR]${NC} Build failed."
                exit 1
            fi
        else
            # Best fix: Safely stage files in a temporary directory instead of extracting over the old source
            TEMP_DIR=$(mktemp -d /tmp/sc0710-update.XXXXXX)
            TEMP_TAR="$TEMP_DIR/main.tar.gz"
            
            curl -fsSL "https://github.com/Nakildias/sc0710/archive/refs/heads/main.tar.gz" -o "$TEMP_TAR" || { rm -rf "$TEMP_DIR"; exit 1; }
            tar -xzf "$TEMP_TAR" --strip-components=1 -C "$TEMP_DIR" || { rm -rf "$TEMP_DIR"; exit 1; }
            rm -f "$TEMP_TAR"
            
            REAL_NEW_VER=$(cat "$TEMP_DIR/version" | tr -d '[:space:]')
            NEW_DKMS_VER=$(sc0710_version_to_dkms "$REAL_NEW_VER")

            lsmod | grep -q "$DRV_NAME" && "$0" --unload
            sc0710_dkms_run_cleanup
            NEW_DKMS_SRC="/usr/src/${DRV_NAME}-${NEW_DKMS_VER}"

            mv "$TEMP_DIR" "$NEW_DKMS_SRC"

            dkms add -m "$DRV_NAME" -v "$NEW_DKMS_VER" >/dev/null 2>&1
            if dkms install -m "$DRV_NAME" -v "$NEW_DKMS_VER" -k "$(uname -r)" --force 2>&1; then
                # Refresh the installed firmware lib alongside the driver (the
                # atomic branch does the same); only if it was installed before.
                if [[ -f /usr/local/libexec/sc0710-firmware-lib.sh && -f "$NEW_DKMS_SRC/scripts/sc0710-firmware-lib.sh" ]]; then
                    cp "$NEW_DKMS_SRC/scripts/sc0710-firmware-lib.sh" /usr/local/libexec/sc0710-firmware-lib.sh
                    chmod +x /usr/local/libexec/sc0710-firmware-lib.sh
                fi
                if sc0710_is_4k_pro_card && sc0710_firmware_lib_path >/dev/null; then
                    if sc0710_cli_ensure_ecp5 5; then
                        echo -e "${GREEN}[OK]${NC} Driver updated (v${REAL_NEW_VER}), ECP5 FPGA programmed."
                    else
                        echo -e "${YELLOW}[WARNING]${NC} Driver updated (v${REAL_NEW_VER}) but ECP5 programming failed."
                        echo -e "  Run: ${BOLD}sc0710-cli --restart${NC}"
                    fi
                else
                    echo -e "${GREEN}[OK]${NC} Driver updated (v${REAL_NEW_VER})."
                fi
            else
                echo -e "${RED}[ERROR]${NC} DKMS rebuild failed."
                exit 1
            fi
            modprobe "$DRV_NAME" 2>/dev/null || echo -e "${YELLOW}[WARNING]${NC} Try: sc0710-cli --load"
        fi
        ;;
    --rebuild)
        if [[ "$IS_ATOMIC" != "true" ]]; then
            echo -e "${RED}[ERROR]${NC} --rebuild is only supported on Atomic distros. Use --update instead."
            exit 1
        fi
        echo -e "${BLUE}::${NC} Forcing module rebuild..."
        lsmod | grep -q "$DRV_NAME" && rmmod "$DRV_NAME" 2>/dev/null || true
        rm -f "$SRC_DIR/.built-for-kernel"
        (cd "$SRC_DIR" && bash "$SRC_DIR/build-and-load.sh") && echo -e "${GREEN}[OK]${NC} Module rebuilt." || { echo -e "${RED}[ERROR]${NC} Rebuild failed."; exit 1; }
        ;;
    -r|-R|--remove)
        echo -e "${BLUE}::${NC} Uninstalling driver and utility..."
        read -r -p "Do you want to unload the driver? [Y/n] " UNLOAD_RESP
        [[ -z "$UNLOAD_RESP" || "$UNLOAD_RESP" =~ ^[yY] ]] && "$0" --unload

        if AUR_PKG=$(sc0710_installed_aur_package); then
            sc0710_cli_remove_aur_install "$AUR_PKG"
            exit $?
        fi

        if [[ "$IS_ATOMIC" == "true" ]]; then
            systemctl stop sc0710-build.service 2>/dev/null || true
            systemctl disable sc0710-build.service 2>/dev/null || true
            rm -f /etc/systemd/system/sc0710-build.service
            systemctl daemon-reload
            sc0710_cli_clear_stale_registration
            rm -rf "$SRC_DIR"
            echo -e "  ${YELLOW}NOTE:${NC} Layered build packages were left unchanged (no rpm-ostree changes, no reboot needed)."
        else
            sc0710_dkms_run_cleanup
        fi
        sc0710_cli_remove_user_state
        rm -f /usr/local/bin/sc0710-cli

        if [[ -x /usr/bin/sc0710-cli ]] || \
            pacman -Q sc0710-dkms-git &>/dev/null || \
            pacman -Q sc0710-dkms &>/dev/null; then
            echo -e "${YELLOW}[WARNING]${NC} AUR package files are still installed."
            if AUR_PKG=$(sc0710_installed_aur_package); then
                echo -e "  Attempting package removal via AUR helper..."
                sc0710_cli_remove_aur_install "$AUR_PKG"
                exit $?
            fi
            echo -e "  Run: ${BOLD}yay -R sc0710-dkms-git${NC} or ${BOLD}sudo pacman -R sc0710-dkms-git${NC}"
            exit 1
        fi

        echo -e "${GREEN}[OK]${NC} Driver, CLI, services, and firmware removed."
        ;;
    --dump)
        write_debug_dump
        ;;
    -v|--version)
        [[ "$IS_ATOMIC" == "true" ]] && echo -e "${BOLD}SC0710${NC} Driver Control Utility (Atomic Edition)" || echo -e "${BOLD}SC0710${NC} Driver Control Utility"
        echo -e "Version: ${BOLD}${CURRENT_VERSION}${NC}"
        check_version
        ;;
    -h|--help)
        show_help
        ;;
    *)
        echo -e "${RED}error:${NC} Unknown option '$1'"
        echo -e "Use ${BOLD}-h${NC} or ${BOLD}--help${NC} for usage information."
        exit 1
        ;;
esac
