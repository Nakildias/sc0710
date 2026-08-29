#!/bin/bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SC0710 Boot-time Build and Load Script
# Called by systemd on every boot to compile the driver against the running kernel.
#

set -eo pipefail
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

DRV_NAME="sc0710"
SRC_DIR="/var/lib/sc0710"
KERNEL_VER="$(uname -r)"
LOG_FILE="/var/log/sc0710/build_$(date '+%Y%m%d_%H%M%S').log"
FIRMWARE_LIB="${SRC_DIR}/sc0710-firmware-lib.sh"

mkdir -p /var/log/sc0710

log() { echo "$*" >> "$LOG_FILE"; echo "$*"; }

log "=== SC0710 boot-time build started ==="
log "Kernel: $KERNEL_VER"
log "Timestamp: $(date)"

# --- SteamOS self-healing ---------------------------------------------------
# A SteamOS A/B update swaps in a fresh rootfs: the kernel headers, anything
# under /usr/local, and the /var/lib/sc0710 symlink can all be gone. The
# persistent tree in /home/sc0710 survives, so put the system back together
# from it before building. See scripts/sc0710-steamos-lib.sh.
IS_STEAMOS=false
STEAMOS_LIB=""
for _cand in /home/sc0710/sc0710-steamos-lib.sh \
             /home/sc0710/scripts/sc0710-steamos-lib.sh \
             "${SRC_DIR}/sc0710-steamos-lib.sh"; do
    if [[ -f "$_cand" ]]; then
        STEAMOS_LIB="$_cand"
        break
    fi
done

if [[ -n "$STEAMOS_LIB" ]]; then
    # shellcheck source=/dev/null
    SC0710_STEAMOS_LOG_FILE="$LOG_FILE" source "$STEAMOS_LIB"
    if sc0710_is_steamos; then
        IS_STEAMOS=true
    fi
fi

steamos_needs_unlock() {
    sc0710_steamos_have_headers "$KERNEL_VER" || return 0
    sc0710_steamos_tools_missing && return 0
    [[ -f "/etc/modprobe.d/${DRV_NAME}.conf" ]] || return 0
    [[ -f "/etc/modprobe.d/${DRV_NAME}-atomic.conf" ]] || return 0
    if lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi "1cfa:0012"; then
        [[ -e "/etc/firmware/sc0710/SC0710.FWI.HEX" ]] || return 0
    fi
    return 1
}

if [[ "$IS_STEAMOS" == "true" ]]; then
    log "SteamOS detected — persistent source tree: ${SC0710_STEAMOS_HOME}"
    sc0710_steamos_ensure_layout || log "WARNING: could not repair ${SRC_DIR} -> ${SC0710_STEAMOS_HOME}"

    if steamos_needs_unlock; then
        log "Unlocking the read-only rootfs to restore the driver environment..."
        trap 'sc0710_steamos_relock' EXIT
        if ! sc0710_steamos_unlock; then
            log "ERROR: could not unlock the rootfs; cannot restore headers or tools."
            exit 1
        fi

        if ! sc0710_steamos_have_headers "$KERNEL_VER"; then
            log "Waiting for the network (kernel headers must be downloaded)..."
            if sc0710_steamos_wait_online 90; then
                sc0710_steamos_ensure_build_tools || true
                if ! sc0710_steamos_ensure_headers "$KERNEL_VER"; then
                    log "ERROR: kernel headers unavailable — the driver cannot be rebuilt this boot."
                    log "Fix it once online with: sudo sc0710-cli --rebuild"
                    exit 1
                fi
            else
                log "ERROR: no network connection; cannot install kernel headers for $KERNEL_VER."
                log "Connect to the network and run: sudo sc0710-cli --rebuild"
                exit 1
            fi
        fi

        # /usr/local is emptied by an OS update — put the CLI and GUIs back.
        if sc0710_steamos_tools_missing; then
            log "Reinstalling sc0710-cli and GUIs into /usr/local/bin..."
            sc0710_steamos_install_tools "${SC0710_STEAMOS_HOME}" || \
                log "WARNING: could not reinstall the sc0710 userspace tools."
        fi

        # modprobe.d config lives on the rootfs and is lost with it.
        if [[ ! -f "/etc/modprobe.d/${DRV_NAME}.conf" ]]; then
            mkdir -p /etc/modprobe.d
            cat > "/etc/modprobe.d/${DRV_NAME}.conf" <<EOF
# Parameter persistence for sc0710 (loaded via insmod by sc0710-build.service)
# Blacklist stops stale copies under /lib/modules/extra/ from loading at boot.
blacklist $DRV_NAME
softdep $DRV_NAME pre: videodev videobuf2-v4l2 videobuf2-vmalloc videobuf2-dma-sg videobuf2-common snd-pcm
EOF
        fi
    fi
fi

if [[ ! -d "$SRC_DIR" || ! -f "$SRC_DIR/Makefile" ]]; then
    log "ERROR: Source directory $SRC_DIR is missing or incomplete."
    exit 1
fi

if [[ ! -d "/lib/modules/${KERNEL_VER}/build" ]]; then
    log "ERROR: Kernel headers for $KERNEL_VER are missing."
    if [[ "$IS_STEAMOS" == "true" ]]; then
        log "Install them with: sudo steamos-readonly disable && sudo pacman -Sy $(sc0710_steamos_headers_pkg "$KERNEL_VER")"
    else
        log "Run: sudo rpm-ostree install kernel-devel"
    fi
    exit 1
fi

BUILT_MOD="$SRC_DIR/build/${DRV_NAME}.ko"
STAMP_FILE="$SRC_DIR/.built-for-kernel"

cd "$SRC_DIR"

if [[ -f "$BUILT_MOD" && -f "$STAMP_FILE" ]]; then
    LAST_KERNEL=$(cat "$STAMP_FILE")
    if [[ "$LAST_KERNEL" == "$KERNEL_VER" ]]; then
        log "Module already built for kernel $KERNEL_VER, skipping rebuild."
    else
        log "Kernel changed ($LAST_KERNEL -> $KERNEL_VER), rebuilding..."
        make clean 2>/dev/null || true
        make KVERSION="$KERNEL_VER" -j"$(nproc)" >> "$LOG_FILE" 2>&1
        echo "$KERNEL_VER" > "$STAMP_FILE"
    fi
else
    log "Building module for kernel $KERNEL_VER..."
    make clean 2>/dev/null || true
    make KVERSION="$KERNEL_VER" -j"$(nproc)" >> "$LOG_FILE" 2>&1
    echo "$KERNEL_VER" > "$STAMP_FILE"
fi

chcon -t modules_object_t "$SRC_DIR/build/${DRV_NAME}.ko" 2>/dev/null || true
log "Module built at $SRC_DIR/build/${DRV_NAME}.ko"

if [[ -f "$FIRMWARE_LIB" ]]; then
    # shellcheck source=/dev/null
    SC0710_FW_LOG_FILE="$LOG_FILE" source "$FIRMWARE_LIB"
    sc0710_init_firmware_paths
    sc0710_clear_stale_kernel_registration

    if sc0710_is_4k_pro; then
        log "Elgato 4K Pro detected — ensuring ECP5 firmware is programmed."
        if ! sc0710_ensure_ecp5_programmed 5; then
            log "ERROR: ECP5 programming failed. See dmesg and $LOG_FILE"
            dmesg 2>/dev/null | grep -E "sc0710.*ECP5" | tail -20 >> "$LOG_FILE" || true
            exit 1
        fi
        log "=== SC0710 boot-time build completed (4K Pro ECP5 OK) ==="
        exit 0
    fi

    LOADED=false
    for attempt in 1 2 3; do
        if sc0710_driver_loaded; then
            LOADED=true
            break
        fi
        if sc0710_load_driver; then
            LOADED=true
            break
        fi
        log "Driver load attempt $attempt failed, retrying in ${attempt}s..."
        sleep "$attempt"
    done

    if [[ "$LOADED" == "true" ]]; then
        log "Driver loaded successfully."
        log "=== SC0710 boot-time build completed ==="
        exit 0
    fi

    log "ERROR: Failed to load driver module after 3 attempts."
    log "Recent kernel messages:"
    dmesg | tail -15 >> "$LOG_FILE"
    exit 1
fi

extra_dir="/lib/modules/${KERNEL_VER}/extra/${DRV_NAME}"
rmmod "$DRV_NAME" 2>/dev/null || true
if [[ -d "$extra_dir" ]]; then
    log "Removing stale kernel module tree: $extra_dir"
    rm -rf "$extra_dir"
    depmod -a "$KERNEL_VER" 2>/dev/null || depmod -a 2>/dev/null || true
fi

for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc videobuf2-dma-sg snd-pcm; do
    modprobe "$dep" 2>/dev/null || log "WARNING: Failed to load dependency: $dep"
done

LOADED=false
for attempt in 1 2 3; do
    if lsmod | grep -q "^${DRV_NAME}[[:space:]]"; then
        LOADED=true
        break
    fi
    if insmod_err=$(insmod "$SRC_DIR/build/${DRV_NAME}.ko" 2>&1); then
        LOADED=true
        break
    fi
    [[ -n "$insmod_err" ]] && log "insmod error: $insmod_err"
    log "insmod attempt $attempt failed, retrying in ${attempt}s..."
    sleep "$attempt"
done

if [[ "$LOADED" == "true" ]]; then
    log "Driver loaded successfully."
else
    log "ERROR: Failed to load driver module after 3 attempts."
    log "Recent kernel messages:"
    dmesg | tail -15 >> "$LOG_FILE"
    exit 1
fi

log "=== SC0710 boot-time build completed ==="
