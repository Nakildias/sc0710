#!/usr/bin/env bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared SC0710 4K Pro ECP5 firmware helpers.
# Sourced by sc0710-firmware.sh and build-and-load.sh — do not execute directly.

[[ -n "${SC0710_FIRMWARE_LIB_LOADED:-}" ]] && return 0
SC0710_FIRMWARE_LIB_LOADED=1

SC0710_FIRMWARE_FILE="${SC0710_FIRMWARE_FILE:-SC0710.FWI.HEX}"
SC0710_DRV_NAME="${SC0710_DRV_NAME:-sc0710}"
SC0710_4K_PRO_SUBSYS="${SC0710_4K_PRO_SUBSYS:-1cfa:0012}"

sc0710_fw_log() {
    if [[ -n "${SC0710_FW_LOG_FILE:-}" ]]; then
        echo "$*" >> "$SC0710_FW_LOG_FILE"
    fi
    echo "$*"
}

sc0710_is_immutable() {
    [[ -f /run/ostree-booted ]] && return 0
    command -v rpm-ostree &>/dev/null && return 0
    [[ -L /lib/firmware && "$(readlink -f /lib/firmware)" == /nix/store/* ]] && return 0
    [[ -L /lib/firmware && "$(readlink -f /lib/firmware)" == /gnu/store/* ]] && return 0
    command -v transactional-update &>/dev/null && return 0
    command -v abroot &>/dev/null && return 0
    if [[ -d /lib/firmware ]]; then
        if ! touch /lib/firmware/.sc0710-write-test 2>/dev/null; then
            return 0
        fi
        rm -f /lib/firmware/.sc0710-write-test 2>/dev/null
    fi
    return 1
}

sc0710_init_firmware_paths() {
    if sc0710_is_immutable; then
        SC0710_DISTRO_TYPE="immutable"
        SC0710_FIRMWARE_STORE="/var/lib/sc0710/firmware"
        SC0710_FIRMWARE_LINK="/etc/firmware/sc0710"
        SC0710_FIRMWARE_PATH="${SC0710_FIRMWARE_STORE}/${SC0710_FIRMWARE_FILE}"
        SC0710_SRC_DIR="/var/lib/sc0710"
    else
        SC0710_DISTRO_TYPE="traditional"
        SC0710_FIRMWARE_STORE="/lib/firmware/sc0710"
        SC0710_FIRMWARE_PATH="${SC0710_FIRMWARE_STORE}/${SC0710_FIRMWARE_FILE}"
        SC0710_SRC_DIR=""
    fi
}

sc0710_is_4k_pro() {
    lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi "$SC0710_4K_PRO_SUBSYS"
}

sc0710_firmware_file_valid() {
    local path="$1"
    [[ -f "$path" ]] || return 1
    [[ "$(stat -c %s "$path" 2>/dev/null || echo 0)" -gt 1024 ]] || return 1
    return 0
}

sc0710_firmware_present() {
    sc0710_firmware_file_valid "$SC0710_FIRMWARE_PATH" && return 0
    if [[ "$SC0710_DISTRO_TYPE" == "immutable" ]]; then
        sc0710_firmware_file_valid "/lib/firmware/sc0710/${SC0710_FIRMWARE_FILE}" && return 0
        sc0710_firmware_file_valid "/etc/firmware/sc0710/${SC0710_FIRMWARE_FILE}" && return 0
    fi
    return 1
}

# Always repair atomic/immutable layout: symlink + SELinux label.
sc0710_ensure_firmware_layout() {
    if ! sc0710_firmware_present; then
        sc0710_fw_log "ERROR: Firmware file missing at ${SC0710_FIRMWARE_PATH}"
        return 1
    fi

    if [[ "$SC0710_DISTRO_TYPE" == "immutable" ]]; then
        mkdir -p "$SC0710_FIRMWARE_STORE" "$SC0710_FIRMWARE_LINK"
        ln -sfn "$SC0710_FIRMWARE_PATH" "${SC0710_FIRMWARE_LINK}/${SC0710_FIRMWARE_FILE}"
        sc0710_fw_log "Firmware symlink: ${SC0710_FIRMWARE_LINK}/${SC0710_FIRMWARE_FILE} -> ${SC0710_FIRMWARE_PATH}"
    fi

    chcon -t firmware_t "$SC0710_FIRMWARE_PATH" 2>/dev/null || true
    if [[ "$SC0710_DISTRO_TYPE" == "immutable" ]]; then
        chcon -h -t firmware_t "${SC0710_FIRMWARE_LINK}/${SC0710_FIRMWARE_FILE}" 2>/dev/null || true
    fi
    return 0
}

sc0710_ecp5_programmed_in_dmesg() {
    local ecp5_log
    ecp5_log=$(dmesg 2>/dev/null | grep -E "sc0710.*ECP5" || true)
    echo "$ecp5_log" | grep -q "firmware programmed successfully" && return 0
    echo "$ecp5_log" | grep -q "already configured" && return 0
    return 1
}

sc0710_ecp5_failed_in_dmesg() {
    local ecp5_log
    ecp5_log=$(dmesg 2>/dev/null | grep -E "sc0710.*ECP5" || true)
    echo "$ecp5_log" | grep -qiE "Failed to load firmware|firmware upload failed|programming failed|Invalid firmware file header|ISC_ENABLE failed|ISC_ERASE failed|bitstream error" && return 0
    return 1
}

sc0710_stop_media_consumers() {
    local uid pid
    local pw_uids=()

    while read -r pid; do
        uid=$(stat -c %u "/proc/$pid" 2>/dev/null) || continue
        pw_uids+=("$uid")
    done < <(pgrep -x pipewire 2>/dev/null || true)

    for uid in $(printf '%s\n' "${pw_uids[@]}" | sort -u); do
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
}

sc0710_restart_media_consumers() {
    local uid
    while read -r uid; do
        [[ -n "$uid" ]] || continue
        sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
            systemctl --user start pipewire.socket 2>/dev/null || true
    done < <(awk -F: '$3 >= 1000 && $3 < 65000 {print $3}' /etc/passwd 2>/dev/null || true)
}

sc0710_driver_loaded() {
    lsmod | grep -q "^${SC0710_DRV_NAME}[[:space:]]"
}

sc0710_unload_driver() {
    sc0710_driver_loaded || return 0

    if rmmod "$SC0710_DRV_NAME" 2>/dev/null; then
        return 0
    fi

    sc0710_stop_media_consumers
    rmmod "$SC0710_DRV_NAME" 2>/dev/null && return 0

    sleep 2
    rmmod "$SC0710_DRV_NAME" 2>/dev/null && return 0

    sc0710_fw_log "WARNING: Could not unload ${SC0710_DRV_NAME} module"
    return 1
}

# Remove stale /lib/modules/*/extra/sc0710 trees left by DKMS, kmod, or old installs.
# Atomic loads via insmod from /var/lib/sc0710/build/ only — a leftover extra/
# registration makes insmod fail with "File exists" or loads the wrong .ko.
# On ostree/Bazzite, extra/ often lives on a read-only deployment; blacklist +
# rmmod is enough — deleting the file is optional.
sc0710_ensure_modprobe_blacklist() {
    local conf="/etc/modprobe.d/${SC0710_DRV_NAME}-atomic.conf"

    [[ "${SC0710_DISTRO_TYPE:-}" == "immutable" ]] || return 0
    [[ -f "$conf" ]] && grep -q "^blacklist ${SC0710_DRV_NAME}\$" "$conf" 2>/dev/null && return 0

    cat > "$conf" <<EOF
# SC0710 on atomic distros: do not auto-load stale copies under /lib/modules/*/extra/.
# sc0710-build.service loads ${SC0710_SRC_DIR:-/var/lib/sc0710}/build/${SC0710_DRV_NAME}.ko via insmod.
blacklist ${SC0710_DRV_NAME}
EOF
    sc0710_fw_log "Installed modprobe blacklist at ${conf}"
}

sc0710_clear_stale_kernel_registration() {
    local kernel_ver extra_dir owner ko_path

    kernel_ver="$(uname -r)"
    extra_dir="/lib/modules/${kernel_ver}/extra/${SC0710_DRV_NAME}"

    sc0710_ensure_modprobe_blacklist
    sc0710_unload_driver || true

    [[ -d "$extra_dir" ]] || return 0

    sc0710_fw_log "Removing stale kernel module tree: ${extra_dir}"
    if rm -rf "$extra_dir" 2>/dev/null; then
        depmod -a "$kernel_ver" 2>/dev/null || depmod -a 2>/dev/null || true
        return 0
    fi

    sc0710_fw_log "WARNING: Could not remove ${extra_dir} (read-only filesystem — common on ostree/Bazzite)"
    sc0710_fw_log "WARNING: The stale .ko can stay on disk; blacklist + unload lets our insmod path work."

    for ko_path in "${extra_dir}/${SC0710_DRV_NAME}.ko.xz" "${extra_dir}/${SC0710_DRV_NAME}.ko"; do
        if [[ -f "$ko_path" ]] && command -v rpm &>/dev/null; then
            owner=$(rpm -qf "$ko_path" 2>/dev/null || true)
            if [[ -n "$owner" ]]; then
                sc0710_fw_log "NOTE: Stale module is owned by RPM: ${owner}"
                sc0710_fw_log "NOTE: To remove permanently: rpm-ostree uninstall <package-name> (then reboot)"
                break
            fi
        fi
    done
}

sc0710_load_driver() {
    local dep mod err

    sc0710_clear_stale_kernel_registration

    for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
        modprobe "$dep" 2>/dev/null || true
    done

    if [[ "$SC0710_DISTRO_TYPE" == "immutable" ]]; then
        mod="${SC0710_SRC_DIR}/build/${SC0710_DRV_NAME}.ko"
        if [[ ! -f "$mod" ]]; then
            sc0710_fw_log "ERROR: Module not found at $mod"
            return 1
        fi
        chcon -t modules_object_t "$mod" 2>/dev/null || true
        if sc0710_driver_loaded; then
            sc0710_fw_log "Driver already loaded."
            return 0
        fi
        err=$(insmod "$mod" 2>&1) || {
            if [[ "$err" == *"File exists"* ]]; then
                sc0710_fw_log "Module slot occupied; unloading and retrying insmod..."
                rmmod "$SC0710_DRV_NAME" 2>/dev/null || true
                sleep 0.5
                if sc0710_driver_loaded; then
                    sc0710_fw_log "Driver still loaded after rmmod attempt."
                elif err=$(insmod "$mod" 2>&1); then
                    return 0
                fi
            fi
            sc0710_fw_log "ERROR: insmod $mod failed: $err"
            dmesg 2>/dev/null | grep -iE "sc0710|insmod|module" | tail -8 >> "${SC0710_FW_LOG_FILE:-/dev/null}" || true
            return 1
        }
        return 0
    fi

    if sc0710_driver_loaded; then
        return 0
    fi
    err=$(modprobe "$SC0710_DRV_NAME" 2>&1) || {
        sc0710_fw_log "ERROR: modprobe ${SC0710_DRV_NAME} failed: $err"
        return 1
    }
    return 0
}

sc0710_wait_for_ecp5_programming() {
    local wait_secs="${1:-8}"
    local i
    for ((i = 0; i < wait_secs; i++)); do
        if sc0710_ecp5_programmed_in_dmesg; then
            return 0
        fi
        if sc0710_ecp5_failed_in_dmesg; then
            return 1
        fi
        sleep 1
    done
    sc0710_ecp5_programmed_in_dmesg
}

# Load (or reload) the driver and verify the ECP5 was programmed.
sc0710_ensure_ecp5_programmed() {
    local max_attempts="${1:-5}"
    local attempt delay

    if ! sc0710_is_4k_pro; then
        return 0
    fi

    sc0710_ensure_firmware_layout || return 1

    if sc0710_driver_loaded && sc0710_ecp5_programmed_in_dmesg; then
        sc0710_fw_log "ECP5 FPGA already programmed."
        return 0
    fi

    for ((attempt = 1; attempt <= max_attempts; attempt++)); do
        delay=$((attempt * 2))
        sc0710_fw_log "ECP5 programming attempt ${attempt}/${max_attempts}..."

        sc0710_unload_driver || true
        sleep "$delay"

        if ! sc0710_load_driver; then
            sc0710_fw_log "WARNING: Driver load failed on attempt ${attempt}"
            sleep 2
            continue
        fi

        if sc0710_wait_for_ecp5_programming 12; then
            sc0710_fw_log "ECP5 FPGA programmed successfully."
            return 0
        fi

        sc0710_fw_log "WARNING: ECP5 not programmed after attempt ${attempt}"
        dmesg 2>/dev/null | grep -E "sc0710.*ECP5" | tail -5 >> "${SC0710_FW_LOG_FILE:-/dev/null}" || true
        sleep 2
    done

    sc0710_fw_log "ERROR: ECP5 FPGA programming failed after ${max_attempts} attempts."
    return 1
}
