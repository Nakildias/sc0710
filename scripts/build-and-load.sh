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

if [[ ! -d "$SRC_DIR" || ! -f "$SRC_DIR/Makefile" ]]; then
    log "ERROR: Source directory $SRC_DIR is missing or incomplete."
    exit 1
fi

if [[ ! -d "/lib/modules/${KERNEL_VER}/build" ]]; then
    log "ERROR: Kernel headers for $KERNEL_VER are missing."
    log "Run: sudo rpm-ostree install kernel-devel"
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
fi

for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
    modprobe "$dep" 2>/dev/null || log "WARNING: Failed to load dependency: $dep"
done

LOADED=false
for attempt in 1 2 3; do
    if lsmod | grep -q "^${DRV_NAME}[[:space:]]"; then
        LOADED=true
        break
    fi
    if insmod "$SRC_DIR/build/${DRV_NAME}.ko" 2>>"$LOG_FILE"; then
        LOADED=true
        break
    fi
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
