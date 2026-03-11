#!/bin/bash
#
# SC0710 Driver Installer for Fedora Atomic / Bazzite
#
# This installer is specifically designed for immutable/atomic Linux distributions
# based on Fedora (Bazzite, Bluefin, Aurora, Fedora Silverblue, Fedora Kinoite).
#
# Strategy:
#   - Sources are stored in /var/lib/sc0710/ (persists across OS updates)
#   - Build dependencies are layered via rpm-ostree (persists across OS updates)
#   - A systemd service rebuilds the module on every boot against the running kernel
#   - The CLI tool is installed to /usr/local/bin/ (writable bind mount on atomic distros)
#   - The module is loaded via insmod from /var/lib/sc0710/ (read-only /lib/modules/ workaround)
#   - Module autoload and parameters are configured via /etc/ (persists across OS updates)
#
# Usage: sudo bash atomic-install-sc0710.sh [--force] [--noconfirm]

# --- Auto-elevate to root ---
if [[ $EUID -ne 0 ]]; then
    if [[ -f "$0" ]]; then
        exec sudo bash "$(realpath "$0")" "$@"
    else
        echo "Please run with: sudo bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/atomic-install-sc0710.sh)\""
        exit 1
    fi
fi

# --- Ensure sbin paths are in PATH ---
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

# --- Safety & Strict Mode ---
set -euo pipefail
IFS=$'\n\t'

# --- Configuration ---
REPO_URL="https://github.com/Nakildias/sc0710.git"
VERSION_URL="https://raw.githubusercontent.com/Nakildias/sc0710/main/version"
DRV_NAME="sc0710"

if [[ -f "version" ]]; then
    DRV_VERSION="$(cat version | tr -d '[:space:]')"
else
    DRV_VERSION=$(curl -fsSL "$VERSION_URL" | tr -d '[:space:]')
fi

SRC_DIR="/var/lib/sc0710"
KERNEL_VER="$(uname -r)"
SERVICE_NAME="sc0710-build"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

# --- Logging ---
LOG_TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
LOG_DIR="/var/log/sc0710"
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/install_${LOG_TIMESTAMP}.log"

# --- Visual Definition ---
BOLD='\033[1m'
BLUE='\033[1;34m'
GREEN='\033[1;32m'
RED='\033[1;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# --- State Variables ---
NOCONFIRM=false
FORCE_INSTALL=false
TEMP_DIR=""
VIDEO_GROUP_CHANGED=false

# --- Essential Files (for verification) ---
ESSENTIAL_FILES=("sc0710.h" "sc0710-core.c" "sc0710-video.c" "Makefile")

# --- Helper Functions ---

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG_FILE"
}

msg() {
    printf "${BLUE}::${NC} ${BOLD}%s${NC}\n" "$1"
    log "INFO: $1"
}

msg2() {
    printf " ${BLUE}->${NC} ${BOLD}%s${NC}\n" "$1"
    log "INFO: $1"
}

warning() {
    printf "${YELLOW}warning:${NC} %s\n" "$1"
    log "WARNING: $1"
}

error() {
    printf "${RED}error:${NC} %s\n" "$1"
    log "ERROR: $1"
}

die() {
    error "$1"
    exit 1
}

cleanup() {
    if [[ -n "$TEMP_DIR" && -d "$TEMP_DIR" ]]; then
        rm -rf "$TEMP_DIR"
        log "Cleaned up temp directory: $TEMP_DIR"
    fi
}

verify_essential_files() {
    local src_dir="$1"
    local missing=()

    for file in "${ESSENTIAL_FILES[@]}"; do
        if [[ ! -f "$src_dir/$file" ]]; then
            missing+=("$file")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        error "Missing essential files: ${missing[*]}"
        return 1
    fi
    return 0
}

confirm() {
    local prompt_text="$1"
    local default_ans="$2"

    if [[ "$NOCONFIRM" == "true" ]]; then
        return 0
    fi

    local brackets
    if [[ "$default_ans" == "Y" ]]; then brackets="[Y/n]"; else brackets="[y/N]"; fi

    printf "${BLUE}::${NC} ${BOLD}%s %s${NC} " "$prompt_text" "$brackets"
    read -r -n 1 response
    echo ""

    if [[ -z "$response" ]]; then response="$default_ans"; fi
    if [[ ! "$response" =~ ^[yY]$ ]]; then return 1; fi
    return 0
}

check_video_group() {
    local users
    users=$(awk -F: '$3 >= 1000 && $3 < 65000 {print $1}' /etc/passwd)

    for user in $users; do
        if id "$user" >/dev/null 2>&1; then
            if ! groups "$user" | grep -q "\bvideo\b"; then
                msg2 "Adding user '$user' to the 'video' group..."
                if usermod -aG video "$user"; then
                    log "Added user $user to video group"
                    VIDEO_GROUP_CHANGED=true
                else
                    warning "Failed to add '$user' to video group. Run: sudo usermod -aG video $user"
                fi
            else
                log "User '$user' is already in video group"
            fi
        fi
    done
}

# --- Argument Parsing ---
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--force) FORCE_INSTALL=true; shift ;;
        --noconfirm) NOCONFIRM=true; shift ;;
        *) shift ;;
    esac
done

# Set trap AFTER argument parsing
trap cleanup EXIT INT TERM

# ==============================================================================
# MAIN EXECUTION
# ==============================================================================

log "=== SC0710 Atomic Driver Installation Started ==="
log "Version: $DRV_VERSION | Kernel: $KERNEL_VER"

echo ""
echo -e "${BOLD}${BLUE}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${BLUE}║       SC0710 Driver Installer — Fedora Atomic Edition     ║${NC}"
echo -e "${BOLD}${BLUE}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

# --- 1. Verify this is an atomic distro ---
msg "Verifying system compatibility..."

if ! command -v rpm-ostree >/dev/null 2>&1; then
    echo ""
    error "rpm-ostree not found. This installer is for Fedora Atomic distros only."
    echo -e "  ${YELLOW}Supported:${NC} Bazzite, Bluefin, Aurora, Fedora Silverblue, Fedora Kinoite"
    echo -e "  ${YELLOW}For traditional distros:${NC} Use ${BOLD}install-sc0710.sh${NC} instead."
    echo ""
    exit 1
fi

IS_BAZZITE=false
IS_BLUEFIN=false
DISTRO_NAME="Fedora Atomic"

if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    case "$ID" in
        bazzite) IS_BAZZITE=true; DISTRO_NAME="Bazzite" ;;
        bluefin) IS_BLUEFIN=true; DISTRO_NAME="Bluefin" ;;
        aurora)  DISTRO_NAME="Aurora" ;;
        fedora)
            if [[ "${VARIANT_ID:-}" == "silverblue" ]]; then
                DISTRO_NAME="Fedora Silverblue"
            elif [[ "${VARIANT_ID:-}" == "kinoite" ]]; then
                DISTRO_NAME="Fedora Kinoite"
            fi
            ;;
    esac
fi

msg2 "Detected: $DISTRO_NAME"
log "Detected distro: $DISTRO_NAME (ID=$ID)"

# --- 2. Permission Check ---
check_video_group

# --- 3. Layer build dependencies via rpm-ostree ---
msg "Checking build dependencies..."

NEEDS_LAYER=false
LAYER_PKGS=()

# Check for kernel-devel (needed for module compilation)
if ! rpm -q kernel-devel >/dev/null 2>&1; then
    LAYER_PKGS+=("kernel-devel")
    NEEDS_LAYER=true
fi

# Check for essential build tools
if ! rpm -q gcc >/dev/null 2>&1; then
    LAYER_PKGS+=("gcc")
    NEEDS_LAYER=true
fi

if ! rpm -q make >/dev/null 2>&1; then
    LAYER_PKGS+=("make")
    NEEDS_LAYER=true
fi

if ! rpm -q git >/dev/null 2>&1; then
    LAYER_PKGS+=("git")
    NEEDS_LAYER=true
fi

if [[ "$NEEDS_LAYER" == "true" ]]; then
    msg2 "The following packages need to be layered: ${LAYER_PKGS[*]}"
    echo ""
    echo -e "  ${YELLOW}NOTE:${NC} On atomic distros, system packages are installed via ${BOLD}rpm-ostree${NC}."
    echo -e "  This will layer them into your system image. A reboot may be required"
    echo -e "  after this step for the packages to become available."
    echo ""

    if confirm "Layer build dependencies now?" "Y"; then
        msg2 "Layering packages via rpm-ostree..."
        if ! rpm-ostree install --idempotent --allow-inactive "${LAYER_PKGS[@]}" 2>&1 | tee -a "$LOG_FILE"; then
            error "Failed to layer packages via rpm-ostree."
            echo -e "  ${YELLOW}Try manually:${NC} ${BOLD}sudo rpm-ostree install ${LAYER_PKGS[*]}${NC}"
            exit 1
        fi
        log "rpm-ostree install completed for: ${LAYER_PKGS[*]}"

        # Check if a reboot is needed (packages not yet available)
        if ! rpm -q kernel-devel >/dev/null 2>&1 || ! command -v gcc >/dev/null 2>&1; then
            echo ""
            echo -e "${YELLOW}╔═══════════════════════════════════════════════════════════╗${NC}"
            echo -e "${YELLOW}║                   REBOOT REQUIRED                         ║${NC}"
            echo -e "${YELLOW}╠═══════════════════════════════════════════════════════════╣${NC}"
            echo -e "${YELLOW}║${NC}  Build dependencies have been layered but require a       ${YELLOW}║${NC}"
            echo -e "${YELLOW}║${NC}  reboot to become available.                               ${YELLOW}║${NC}"
            echo -e "${YELLOW}║${NC}                                                            ${YELLOW}║${NC}"
            echo -e "${YELLOW}║${NC}  Please ${BOLD}reboot${NC} and run this installer again.              ${YELLOW}║${NC}"
            echo -e "${YELLOW}╚═══════════════════════════════════════════════════════════╝${NC}"
            echo ""
            log "Reboot required after rpm-ostree install"
            exit 0
        fi
    else
        die "Cannot proceed without build dependencies."
    fi
else
    msg2 "All build dependencies are present."
fi

# Verify kernel headers exist for the running kernel
if [[ ! -d "/lib/modules/${KERNEL_VER}/build" ]]; then
    echo ""
    error "Kernel headers for $KERNEL_VER are missing."
    echo -e "  ${YELLOW}This can happen if:${NC}"
    echo -e "    1. A system update changed the kernel but you have not rebooted"
    echo -e "    2. The kernel-devel package does not match the running kernel"
    echo ""
    echo -e "  ${BOLD}Try:${NC} Reboot and run this installer again."
    echo -e "  ${BOLD}Or:${NC}  ${BOLD}sudo rpm-ostree install kernel-devel-${KERNEL_VER}${NC}"
    exit 1
fi

# --- 4. Unload existing module if loaded ---
if lsmod | grep -q "$DRV_NAME"; then
    warning "Module $DRV_NAME is currently loaded."

    # Attempt 1: Simple unload
    if rmmod "$DRV_NAME" 2>/dev/null; then
        msg2 "Module unloaded."
        log "Module unloaded normally"
    else
        msg2 "Module is in use. Stopping PipeWire and consumers..."
        log "Module in use, attempting PipeWire-aware unload"

        # Collect UIDs of users running PipeWire (save before stopping)
        PW_UIDS=()
        while read -r pid; do
            uid=$(stat -c %u "/proc/$pid" 2>/dev/null) || continue
            PW_UIDS+=("$uid")
        done < <(pgrep -x pipewire 2>/dev/null || true)

        # Stop PipeWire SOCKET first (prevents socket-activation respawn)
        for uid in $(printf '%s\n' "${PW_UIDS[@]}" | sort -u); do
            sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
                systemctl --user stop pipewire.socket pipewire.service wireplumber.service 2>/dev/null || true
        done

        # Kill any remaining processes holding capture device files open
        for vdev in /dev/video*; do
            [[ -e "$vdev" ]] && fuser -k "$vdev" >/dev/null 2>&1 || true
        done
        for sdev in /dev/snd/*; do
            [[ -e "$sdev" ]] && fuser -k "$sdev" >/dev/null 2>&1 || true
        done
        sleep 1

        # Attempt 2: Unload now that PipeWire is properly stopped
        if rmmod "$DRV_NAME" 2>/dev/null; then
            msg2 "Module unloaded successfully."
            log "Module unloaded after stopping PipeWire"
        else
            sleep 2
            # Attempt 3: Final try
            if rmmod "$DRV_NAME" 2>/dev/null; then
                msg2 "Module unloaded successfully."
                log "Module unloaded on third attempt"
            else
                error "Could not unload the module."
                echo -e "  Current reference count: $(awk '/sc0710/{print $3}' /proc/modules 2>/dev/null || echo unknown)"
                lsof /dev/video* /dev/snd/* 2>/dev/null | grep -v "^COMMAND" | sed 's/^/  /' || true
                # Restart PipeWire since we stopped it but couldn't unload
                for uid in $(printf '%s\n' "${PW_UIDS[@]}" | sort -u); do
                    sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
                        systemctl --user start pipewire.socket 2>/dev/null || true
                done
                die "A reboot may be required. Alternatively, close all applications and try again."
            fi
        fi

        # Restart PipeWire for all users
        msg2 "Restarting PipeWire..."
        for uid in $(printf '%s\n' "${PW_UIDS[@]}" | sort -u); do
            sudo -u "#$uid" XDG_RUNTIME_DIR="/run/user/$uid" \
                systemctl --user start pipewire.socket 2>/dev/null || true
        done
    fi
fi

# --- 5. Source Setup ---
msg "Setting up driver source..."

# Clean previous installation
if [[ -d "$SRC_DIR" ]]; then
    if [[ "$FORCE_INSTALL" == "true" ]] || confirm "Previous installation found. Replace it?" "Y"; then
        rm -rf "$SRC_DIR"
        log "Removed previous source directory"
    else
        msg2 "Keeping existing source."
    fi
fi

if [[ ! -d "$SRC_DIR" ]]; then
    mkdir -p "$SRC_DIR"

    # --- Local/Online Mode Detection ---
    LOCAL_MODE=false
    if [[ -f "./Makefile" && -f "./sc0710.h" ]]; then
        msg "Local source detected in current directory."
        if confirm "Use local source instead of downloading?" "Y"; then
            LOCAL_MODE=true
        fi
    fi

    if [[ "$LOCAL_MODE" == "true" ]]; then
        msg2 "Copying local source..."
        cp -r ./* "$SRC_DIR/"
        log "Copied local source to $SRC_DIR"
    else
        msg2 "Downloading source..."
        TEMP_DIR=$(mktemp -d -t sc0710.XXXXXX) || die "Failed to create temp directory"
        log "Created temp directory: $TEMP_DIR"

        if ! git clone --depth 1 "$REPO_URL" "$TEMP_DIR" >/dev/null 2>&1; then
            die "Git clone failed. Check your internet connection."
        fi
        log "Git clone successful"
        cp -r "$TEMP_DIR"/* "$SRC_DIR/"
    fi

    # Verify essential files are present
    msg2 "Verifying source integrity..."
    if ! verify_essential_files "$SRC_DIR"; then
        die "Source verification failed. The download may be corrupted."
    fi
    log "Source verification passed"
fi

# --- 5.5. Firmware Extraction (4K Pro only) ---
if lspci -d ::0400 -nn 2>/dev/null | grep -qi "1cfa:0012"; then
    FIRMWARE_STORE="/var/lib/sc0710/firmware"
    FIRMWARE_FILE="SC0710.FWI.HEX"
    if [[ -f "$FIRMWARE_STORE/$FIRMWARE_FILE" || -f "/lib/firmware/sc0710/$FIRMWARE_FILE" ]]; then
        msg2 "4K Pro firmware already present"
    else
        msg "4K Pro detected — extracting ECP5 firmware (atomic)..."
        EXTRACT_SCRIPT="$SRC_DIR/atomic-extract-firmware.sh"
        if [[ -f "$EXTRACT_SCRIPT" ]]; then
            chmod +x "$EXTRACT_SCRIPT"
            if bash "$EXTRACT_SCRIPT"; then
                msg2 "Firmware extraction completed."
                log "4K Pro firmware extracted via atomic script"
            else
                warning "Firmware extraction failed. The driver will load but ECP5 programming will not work."
                warning "You can retry manually: sudo bash $EXTRACT_SCRIPT"
                log "WARNING: Firmware extraction failed"
            fi
        else
            warning "atomic-extract-firmware.sh not found in source tree. Firmware must be installed manually."
            warning "Place SC0710.FWI.HEX in /var/lib/sc0710/firmware/ and symlink to /etc/firmware/sc0710/"
            log "WARNING: atomic-extract-firmware.sh missing from source"
        fi
    fi
else
    log "No 4K Pro card detected, skipping firmware extraction"
fi

# --- 5.6. Firmware Service (4K Pro only) ---
# Install a systemd service that ensures the ECP5 firmware file is present
# on every boot and triggers a driver reload if the FPGA wasn't programmed.
if lspci -d ::0400 -nn 2>/dev/null | grep -qi "1cfa:0012"; then
    msg "4K Pro detected — installing firmware service..."

    FW_SERVICE_SCRIPT="/var/lib/sc0710/sc0710-firmware.sh"
    FW_SERVICE_SCRIPT_SRC="$SRC_DIR/sc0710-firmware.sh"
    if [[ -f "$FW_SERVICE_SCRIPT_SRC" ]]; then
        cp "$FW_SERVICE_SCRIPT_SRC" "$FW_SERVICE_SCRIPT"
    elif [[ -f "./sc0710-firmware.sh" ]]; then
        cp "./sc0710-firmware.sh" "$FW_SERVICE_SCRIPT"
    else
        warning "sc0710-firmware.sh not found in source tree."
        FW_SERVICE_SCRIPT=""
    fi

    if [[ -n "$FW_SERVICE_SCRIPT" ]]; then
        chmod +x "$FW_SERVICE_SCRIPT"

        cat > "/etc/systemd/system/sc0710-firmware.service" <<FWEOF
[Unit]
Description=SC0710 4K Pro ECP5 Firmware Loader
After=local-fs.target network-online.target
Wants=network-online.target
Before=sc0710-build.service
ConditionPathExists=$FW_SERVICE_SCRIPT

[Service]
Type=oneshot
ExecStart=/bin/bash $FW_SERVICE_SCRIPT
RemainAfterExit=yes
TimeoutStartSec=120
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
FWEOF

        systemctl daemon-reload
        systemctl enable sc0710-firmware.service
        msg2 "Firmware service enabled: sc0710-firmware.service"
        log "Created and enabled sc0710-firmware.service"
    fi
else
    log "No 4K Pro card detected, skipping firmware service installation"
fi

# --- 6. Create the boot-time build script ---
msg "Creating boot-time build script..."

BUILD_SCRIPT="/var/lib/sc0710/build-and-load.sh"
cat > "$BUILD_SCRIPT" <<'BUILDEOF'
#!/bin/bash
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

mkdir -p /var/log/sc0710

log() { echo "$*" >> "$LOG_FILE"; echo "$*"; }

log "=== SC0710 boot-time build started ==="
log "Kernel: $KERNEL_VER"
log "Timestamp: $(date)"

# Verify source directory exists
if [[ ! -d "$SRC_DIR" || ! -f "$SRC_DIR/Makefile" ]]; then
    log "ERROR: Source directory $SRC_DIR is missing or incomplete."
    exit 1
fi

# Verify kernel headers exist
if [[ ! -d "/lib/modules/${KERNEL_VER}/build" ]]; then
    log "ERROR: Kernel headers for $KERNEL_VER are missing."
    log "Run: sudo rpm-ostree install kernel-devel"
    exit 1
fi

# Check if the module is already built for this exact kernel
BUILT_MOD="$SRC_DIR/${DRV_NAME}.ko"
STAMP_FILE="$SRC_DIR/.built-for-kernel"

# cd into source dir — the Makefile uses M=$(PWD) which must resolve to the source tree
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

# Set SELinux context so insmod works from a systemd service.
# Files in /var/lib/ have var_lib_t context by default, which restricted domains
# (like systemd services) cannot load as kernel modules. Kernel modules require
# modules_object_t context.
chcon -t modules_object_t "$SRC_DIR/${DRV_NAME}.ko" 2>/dev/null || true

log "Module built at $SRC_DIR/${DRV_NAME}.ko"

# Load dependency modules (these are part of the immutable kernel image, modprobe works)
for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
    modprobe "$dep" 2>/dev/null || log "WARNING: Failed to load dependency: $dep"
done

# Load the driver via insmod with retry logic.
# At boot, the V4L2/media subsystem may still be initializing after modprobe returns.
# Retry up to 5 times with increasing delays to handle this race condition.
LOADED=false
for attempt in 1 2 3; do
    if insmod "$SRC_DIR/${DRV_NAME}.ko" 2>>"$LOG_FILE"; then
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
BUILDEOF
chmod +x "$BUILD_SCRIPT"
log "Created build script: $BUILD_SCRIPT"

# --- 7. Create the systemd service ---
msg "Creating systemd service for boot-time module build..."

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=SC0710 Capture Card Driver - Build and Load
After=local-fs.target basic.target systemd-udev-settle.service sc0710-firmware.service
Wants=systemd-udev-settle.service
ConditionPathExists=/var/lib/sc0710/build-and-load.sh

[Service]
Type=oneshot
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ExecStart=/bin/bash /var/lib/sc0710/build-and-load.sh
RemainAfterExit=yes
TimeoutStartSec=300
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}.service"
log "Created and enabled systemd service: ${SERVICE_NAME}.service"
msg2 "Systemd service enabled: ${SERVICE_NAME}.service"

# --- 8. Configure module parameters ---
# NOTE: On atomic distros, we do NOT use /etc/modules-load.d/ because
# the module is not in the read-only /lib/modules/ tree (modprobe cannot find it).
# The systemd service (sc0710-build.service) handles building and loading via insmod.
msg "Configuring module parameters..."

cat > "/etc/modprobe.d/${DRV_NAME}.conf" <<EOF
# Parameter persistence for sc0710 (loaded via insmod by sc0710-build.service)
# softdep is informational only; actual dependency loading is handled by the build script.
softdep $DRV_NAME pre: videodev videobuf2-v4l2 videobuf2-vmalloc videobuf2-common snd-pcm
EOF
log "Module parameters configured"

# --- 9. Initial build and load ---
msg "Performing initial build..."

cd "$SRC_DIR"

# Read version from source
if [[ -f "$SRC_DIR/version" ]]; then
    DRV_VERSION="$(cat "$SRC_DIR/version" | tr -d '[:space:]')"
fi

echo ""
if ! make KVERSION="$KERNEL_VER" -j"$(nproc)" 2>&1 | tee -a "$LOG_FILE"; then
    error "Build failed. Check the log at: $LOG_FILE"
    exit 1
fi
log "Initial build completed"

# Record which kernel we built for
echo "$KERNEL_VER" > "$SRC_DIR/.built-for-kernel"

# Set SELinux context so the boot-time service can load the module
chcon -t modules_object_t "$SRC_DIR/${DRV_NAME}.ko" 2>/dev/null || true

log "Module built at $SRC_DIR/${DRV_NAME}.ko"

# Load dependency modules
msg2 "Loading module..."
FAILED_DEPS=()
DEP_ERRORS=""

load_dep() {
    local mod="$1"
    local modname="${mod//-/_}"

    if ! lsmod | grep -q "^${modname}"; then
        local err
        err=$(modprobe "$mod" 2>&1)
        if [[ $? -ne 0 ]]; then
            FAILED_DEPS+=("$mod")
            DEP_ERRORS+="  ${mod}: ${err}\n"
            return 1
        fi
    fi
    return 0
}

load_dep "videodev" || true
load_dep "videobuf2-common" || true
load_dep "videobuf2-v4l2" || true
load_dep "videobuf2-vmalloc" || true
load_dep "snd-pcm" || true

if [[ ${#FAILED_DEPS[@]} -gt 0 ]]; then
    echo ""
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${RED}  KERNEL MODULE ISSUE DETECTED${NC}"
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "  The following required kernel modules failed to load:"
    echo ""
    echo -e "${YELLOW}${DEP_ERRORS}${NC}"
    echo -e "  This indicates a problem with the kernel package, not the driver."
    echo -e "  Possible solutions:"
    echo -e "    1. Reinstall kernel modules: ${BOLD}sudo rpm-ostree override reset kernel${NC}"
    echo -e "    2. Wait for a system update from your distribution"
    echo ""
    echo -e "${BOLD}Recent kernel messages:${NC}"
    dmesg | tail -10 | sed 's/^/  /'
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
    log "ERROR: Failed to load kernel modules: ${FAILED_DEPS[*]}"
fi

# Load the driver via insmod (cannot use modprobe — /lib/modules/ is read-only)
DRIVER_ERR=$(insmod "$SRC_DIR/${DRV_NAME}.ko" 2>&1)
if [[ $? -ne 0 ]]; then
    echo ""
    error "Failed to load $DRV_NAME module."
    echo -e "  ${YELLOW}Error: ${DRIVER_ERR}${NC}"
    echo ""
    echo -e "${BOLD}Recent kernel messages:${NC}"
    dmesg | tail -10 | sed 's/^/  /'
    echo ""
    log "ERROR: insmod $DRV_NAME failed: $DRIVER_ERR"
    warning "The driver was installed but could not be loaded."
    warning "It may work after a reboot."
else
    msg2 "Driver loaded successfully!"
fi

# --- 10. Install CLI Tool ---
msg "Installing CLI utility..."
cat > "/usr/local/bin/sc0710-cli" <<EOF
#!/bin/bash
# SC0710 Control Utility (Atomic Edition)

# --- Configuration ---
CURRENT_VERSION="$DRV_VERSION"
VERSION_URL="$VERSION_URL"
DRV_NAME="$DRV_NAME"
SRC_DIR="$SRC_DIR"

# --- Colors ---
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
BOLD='\033[1m'
NC='\033[0m'

# --- Auto-elevate to root ---
if [[ \$EUID -ne 0 ]]; then
    exec sudo "\$0" "\$@"
fi

# --- Persistence Function ---
save_config() {
    local dbg=0
    if [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]]; then
        dbg=\$(cat /sys/module/sc0710/parameters/sc0710_debug_mode 2>/dev/null || echo 0)
    elif [[ -f /sys/module/sc0710/parameters/debug ]]; then
        dbg=\$(cat /sys/module/sc0710/parameters/debug 2>/dev/null || echo 0)
    fi
    local img=\$(cat /sys/module/sc0710/parameters/use_status_images 2>/dev/null || echo 1)

    echo "options sc0710 sc0710_debug_mode=\$dbg use_status_images=\$img" > /etc/modprobe.d/sc0710-params.conf
    echo -e "\${BLUE}[PERSIST]\${NC} Settings saved to /etc/modprobe.d/sc0710-params.conf"
}

# --- Version Check Function ---
check_version() {
    local REMOTE_VERSION
    REMOTE_VERSION=\$(curl -fsSL "\$VERSION_URL" 2>/dev/null | tr -d '[:space:]')

    if [[ -n "\$REMOTE_VERSION" && "\$REMOTE_VERSION" != "\$CURRENT_VERSION" ]]; then
        echo ""
        echo -e "\${YELLOW}╔═══════════════════════════════════════════════════════════╗\${NC}"
        echo -e "\${YELLOW}║   UPDATE AVAILABLE                                        ║\${NC}"
        echo -e "\${YELLOW}╠═══════════════════════════════════════════════════════════╣\${NC}"
        echo -e "\${YELLOW}║\${NC}  Current: \${RED}\${CURRENT_VERSION}\${NC}                                    \${YELLOW}║\${NC}"
        printf "\${YELLOW}║\${NC}  Latest:  \${GREEN}%-47s\${NC} \${YELLOW}║\${NC}\n" "\$REMOTE_VERSION"
        echo -e "\${YELLOW}╠═══════════════════════════════════════════════════════════╣\${NC}"
        echo -e "\${YELLOW}║\${NC}  Run \${BOLD}sc0710-cli -U\${NC} or \${BOLD}sc0710-cli --update\${NC} to update       \${YELLOW}║\${NC}"
        echo -e "\${YELLOW}╚═══════════════════════════════════════════════════════════╝\${NC}"
        echo ""
    fi
}

# --- Help Function ---
show_help() {
    echo -e "\${BOLD}SC0710\${NC} Driver Control Utility v\${CURRENT_VERSION} (Atomic Edition)"
    echo ""
    echo -e "\${BOLD}USAGE:\${NC}"
    echo -e "    sc0710-cli [OPTION]"
    echo ""
    echo -e "\${BOLD}OPTIONS:\${NC}"
    echo -e "    \${BOLD}-l, --load\${NC}       Load the driver module"
    echo -e "    \${BOLD}-u, --unload\${NC}     Unload the driver module"
    echo -e "    \${BOLD}--restart\${NC}        Restart the driver module"
    echo -e "    \${BOLD}-s, --status\${NC}     Show module and build status"
    echo -e "    \${BOLD}-d, --debug\${NC}      Toggle debug mode on/off"
    echo -e "    \${BOLD}-it, --image-toggle\${NC} Toggle status images on/off"
    echo -e "    \${BOLD}-U, --update\${NC}     Check for updates and reinstall"
    echo -e "    \${BOLD}-r, -R, --remove\${NC} Completely uninstall driver and CLI"
    echo -e "    \${BOLD}--rebuild\${NC}        Force rebuild the module for current kernel"
    echo -e "    \${BOLD}-v, --version\${NC}    Show version information"
    echo -e "    \${BOLD}-h, --help\${NC}       Show this help message"
    echo ""
}

# --- No Arguments Handler ---
if [[ \$# -eq 0 ]]; then
    echo -e "\${BOLD}SC0710\${NC} Driver Control Utility (Atomic Edition)"
    echo -e "Use \${BOLD}-h\${NC} or \${BOLD}--help\${NC} for usage information."
    exit 0
fi

# --- Command Handler ---
case "\$1" in
    -l|--load)
        if lsmod | grep -q \$DRV_NAME; then
            echo -e "\${GREEN}[OK]\${NC} Driver is already loaded."
            exit 0
        fi
        echo -e "\${BLUE}::\${NC} Loading driver..."
        # Load dependencies via modprobe (kernel-tree modules work fine)
        for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
            modprobe "\$dep" 2>/dev/null || true
        done
        # Load our module via insmod (read-only /lib/modules/ workaround)
        if insmod "\$SRC_DIR/\${DRV_NAME}.ko" 2>/dev/null; then
            echo -e "\${GREEN}[OK]\${NC} Driver loaded successfully."
        elif [[ ! -f "\$SRC_DIR/\${DRV_NAME}.ko" ]]; then
            echo -e "\${RED}[ERROR]\${NC} Module not found. Run \${BOLD}sc0710-cli --rebuild\${NC} first."
        else
            echo -e "\${RED}[ERROR]\${NC} Failed to load driver. Run \${BOLD}sc0710-cli --rebuild\${NC} if kernel was updated."
        fi
        ;;
    -u|--unload)
        if ! lsmod | grep -q \$DRV_NAME; then
            echo -e "\${GREEN}[OK]\${NC} Driver is not loaded."
            exit 0
        fi
        echo -e "\${BLUE}::\${NC} Unloading driver..."

        # Attempt 1: Simple unload (works if nothing has the device open)
        if rmmod \$DRV_NAME 2>/dev/null; then
            echo -e "\${GREEN}[OK]\${NC} Driver unloaded successfully."
            exit 0
        fi

        echo -e "\${YELLOW}[BUSY]\${NC} Module is in use. Stopping PipeWire and consumers..."

        # Collect UIDs of users running PipeWire (must save before stopping)
        PW_UIDS=()
        while read -r pid; do
            uid=\$(stat -c %u "/proc/\$pid" 2>/dev/null) || continue
            PW_UIDS+=("\$uid")
        done < <(pgrep -x pipewire 2>/dev/null || true)

        # Stop PipeWire SOCKET first (prevents socket-activation respawn)
        # Then stop the service and WirePlumber
        for uid in \$(printf '%s\n' "\${PW_UIDS[@]}" | sort -u); do
            sudo -u "#\$uid" XDG_RUNTIME_DIR="/run/user/\$uid" \
                systemctl --user stop pipewire.socket pipewire.service wireplumber.service 2>/dev/null || true
        done

        # Kill any remaining processes holding capture device files open
        for vdev in /dev/video*; do
            [[ -e "\$vdev" ]] && fuser -k "\$vdev" >/dev/null 2>&1 || true
        done
        for sdev in /dev/snd/*; do
            [[ -e "\$sdev" ]] && fuser -k "\$sdev" >/dev/null 2>&1 || true
        done
        sleep 1

        # Attempt 2: Unload now that PipeWire is properly stopped
        if rmmod \$DRV_NAME 2>/dev/null; then
            echo -e "\${GREEN}[OK]\${NC} Driver unloaded successfully."
        else
            sleep 2
            if rmmod \$DRV_NAME 2>/dev/null; then
                echo -e "\${GREEN}[OK]\${NC} Driver unloaded successfully."
            else
                echo -e "\${RED}[ERROR]\${NC} Module is still held by the kernel."
                echo -e "  Current reference count: \$(awk '/sc0710/{print \$3}' /proc/modules 2>/dev/null || echo unknown)"
                lsof /dev/video* /dev/snd/* 2>/dev/null | grep -v "^COMMAND" | sed 's/^/  /' || true
                echo -e "  A reboot may be required."
                # Restart PipeWire since we stopped it but couldn't unload
                for uid in \$(printf '%s\n' "\${PW_UIDS[@]}" | sort -u); do
                    sudo -u "#\$uid" XDG_RUNTIME_DIR="/run/user/\$uid" \
                        systemctl --user start pipewire.socket 2>/dev/null || true
                done
                exit 1
            fi
        fi

        # Restart PipeWire for all users (socket activation will bring up the rest)
        echo -e "\${BLUE}[INFO]\${NC} Restarting PipeWire..."
        for uid in \$(printf '%s\n' "\${PW_UIDS[@]}" | sort -u); do
            sudo -u "#\$uid" XDG_RUNTIME_DIR="/run/user/\$uid" \
                systemctl --user start pipewire.socket 2>/dev/null || true
        done
        ;;
    --restart)
        \$0 --unload
        sleep 1
        \$0 --load
        ;;
    -s|--status)
        check_version
        echo -e "\${BLUE}::\${NC} \${BOLD}System Type\${NC}"
        echo -e "   Atomic/Immutable (boot-time build)"
        if [[ -f "\$SRC_DIR/.built-for-kernel" ]]; then
            echo -e "   Last built for: \${BOLD}\$(cat "\$SRC_DIR/.built-for-kernel")\${NC}"
        fi
        echo -e "   Running kernel:  \${BOLD}\$(uname -r)\${NC}"
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Systemd Service\${NC}"
        if systemctl is-enabled sc0710-build.service >/dev/null 2>&1; then
            echo -e "   \${GREEN}●\${NC} sc0710-build.service is enabled"
        else
            echo -e "   \${RED}○\${NC} sc0710-build.service is disabled"
        fi
        if systemctl is-active sc0710-build.service >/dev/null 2>&1; then
            echo -e "   \${GREEN}●\${NC} Last boot build: succeeded"
        else
            echo -e "   \${YELLOW}○\${NC} Last boot build: not run or failed"
        fi
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Kernel Module\${NC}"
        if lsmod | grep -q \$DRV_NAME; then
            echo -e "   \${GREEN}●\${NC} Module is loaded"
            MOD_INFO=\$(lsmod | grep \$DRV_NAME | head -1)
            MOD_SIZE=\$(echo "\$MOD_INFO" | awk '{print \$2}')
            MOD_USED=\$(echo "\$MOD_INFO" | awk '{print \$3}')
            echo "   Size: \$MOD_SIZE bytes, Reference count: \$MOD_USED"
            if [[ "\$MOD_USED" -gt 0 ]]; then
                PIDS=""
                for vdev in /dev/video*; do
                    if [[ -e "\$vdev" ]]; then
                        DEVPIDS=\$(fuser "\$vdev" 2>/dev/null | tr -s ' ')
                        if [[ -n "\$DEVPIDS" ]]; then
                            PIDS="\$PIDS \$DEVPIDS"
                        fi
                    fi
                done
                for sdev in /dev/snd/*; do
                    if [[ -e "\$sdev" ]]; then
                        DEVPIDS=\$(fuser "\$sdev" 2>/dev/null | tr -s ' ')
                        if [[ -n "\$DEVPIDS" ]]; then
                            PIDS="\$PIDS \$DEVPIDS"
                        fi
                    fi
                done
                if [[ -n "\$PIDS" ]]; then
                    echo -e "   \${YELLOW}Processes holding device open:\${NC}"
                    echo "\$PIDS" | tr ' ' '\n' | sort -un | while read -r pid; do
                        if [[ -n "\$pid" && -f "/proc/\$pid/comm" ]]; then
                            PNAME=\$(cat /proc/\$pid/comm 2>/dev/null)
                            echo -e "     PID \${BOLD}\$pid\${NC} - \$PNAME"
                        fi
                    done
                else
                    echo -e "   \${YELLOW}No open device handles found (kernel-internal reference?)\${NC}"
                fi
            fi
        else
            echo -e "   \${RED}○\${NC} Module is not loaded"
        fi
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Card Information\${NC}"
        if lsmod | grep -q \$DRV_NAME; then
            FOUND_CARDS=0
            for pcidir in /sys/bus/pci/drivers/sc0710/0*; do
                if [[ -d "\$pcidir" ]]; then
                    FOUND_CARDS=1
                    PCI_ADDR=\$(basename "\$pcidir")
                    SUBVEN=\$(cat "\$pcidir/subsystem_vendor" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//' )
                    SUBDEV=\$(cat "\$pcidir/subsystem_device" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//' )
                    VEN=\$(cat "\$pcidir/vendor" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//' )
                    DEV=\$(cat "\$pcidir/device" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//' )

                    BOARD_NAME=\$(dmesg 2>/dev/null | grep -E "sc0710.*subsystem: \${SUBVEN}:\${SUBDEV}.*board:" | tail -1 | sed 's/.*board: \([^\[]*\).*/\1/' | sed 's/ *$//')

                    if [[ -z "\$BOARD_NAME" ]]; then
                        if [[ "\$SUBVEN:\$SUBDEV" == "1cfa:000e" ]]; then
                            BOARD_NAME="Elgato 4k60 Pro mk.2"
                        elif [[ "\$SUBVEN:\$SUBDEV" == "1cfa:0012" ]]; then
                            BOARD_NAME="Elgato 4K Pro"
                        elif [[ "\$SUBVEN:\$SUBDEV" == "1cfa:0006" ]]; then
                            BOARD_NAME="Elgato HD60 Pro (1cfa:0006)"
                        else
                            BOARD_NAME="UNKNOWN/GENERIC"
                        fi
                    fi

                    echo -e "   \${GREEN}●\${NC} Device at PCI \${BOLD}\${PCI_ADDR}\${NC}"
                    echo -e "     Board: \${BOARD_NAME}"
                    echo -e "     Hardware: \${VEN}:\${DEV} (Subsys: \${SUBVEN}:\${SUBDEV})"

                    if [[ "\$SUBVEN:\$SUBDEV" == "1cfa:0006" ]]; then
                        echo -e "     \${RED}⚠ WARNING:\${NC} This is an Elgato HD60 Pro."
                        echo -e "     \${RED}          \${NC} It is an entirely different chipset and is \${BOLD}INCOMPATIBLE\${NC} with this driver."
                    fi
                fi
            done
            if [[ \$FOUND_CARDS -eq 0 ]]; then
                echo -e "   \${YELLOW}○\${NC} No devices found currently bound to driver"
            fi
        else
            echo -e "   \${RED}○\${NC} Module is not loaded, cannot retrieve card info"
        fi
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Signal Status\${NC}"
        if [[ -f /proc/sc0710-state ]]; then
            PROC_INFO=\$(cat /proc/sc0710-state 2>/dev/null)
            HDMI_LINE=\$(echo "\$PROC_INFO" | grep "HDMI:" | head -1)
            if [[ -n "\$HDMI_LINE" ]]; then
                if echo "\$HDMI_LINE" | grep -q "no signal"; then
                    echo -e "   \${YELLOW}○\${NC} No signal detected"
                else
                    FMT_NAME=\$(echo "\$HDMI_LINE" | sed 's/.*HDMI: \([^ ]*\).*/\1/')
                    RESOLUTION=\$(echo "\$HDMI_LINE" | sed 's/.*-- \([^ ]*\).*/\1/')
                    TIMING=\$(echo "\$HDMI_LINE" | grep -oP '\([0-9]+x[0-9]+\)' | tr -d '()')
                    echo -e "   \${GREEN}●\${NC} Signal locked"
                    echo -e "   Format: \${BOLD}\${FMT_NAME}\${NC}"
                    if [[ -n "\$RESOLUTION" && "\$RESOLUTION" != "\$HDMI_LINE" ]]; then
                        echo -e "   Resolution: \${RESOLUTION}"
                    fi
                    if [[ -n "\$TIMING" ]]; then
                        echo -e "   Total timing: \${TIMING}"
                    fi
                fi
            else
                echo -e "   \${RED}○\${NC} Could not read HDMI status"
            fi
        else
            LAST_FMT=\$(dmesg 2>/dev/null | grep -E "sc0710.*Detected timing|sc0710.*DTC created" | tail -1)
            if [[ -n "\$LAST_FMT" ]]; then
                FMT_MSG=\$(echo "\$LAST_FMT" | sed 's/.*sc0710[^:]*: //')
                echo -e "   Last detected: \${FMT_MSG}"
            else
                echo -e "   \${RED}○\${NC} No signal info available (check dmesg)"
            fi
        fi
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Debug Mode\${NC}"
        DBG_PATH=""
        if [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]]; then
            DBG_PATH=/sys/module/sc0710/parameters/sc0710_debug_mode
        elif [[ -f /sys/module/sc0710/parameters/debug ]]; then
            DBG_PATH=/sys/module/sc0710/parameters/debug
        fi
        if [[ -n "\$DBG_PATH" ]]; then
            DBG_STATE=\$(cat "\$DBG_PATH")
            if [[ "\$DBG_STATE" == "1" ]]; then
                echo -e "   \${YELLOW}●\${NC} Debug mode enabled (verbose logging)"
            else
                echo -e "   \${GREEN}○\${NC} Debug mode disabled (quiet)"
            fi
        else
            echo -e "   \${RED}○\${NC} Parameter not available (module not loaded)"
        fi
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Status Images\${NC}"
        if [[ -f /sys/module/sc0710/parameters/use_status_images ]]; then
            IMG_STATE=\$(cat /sys/module/sc0710/parameters/use_status_images)
            if [[ "\$IMG_STATE" == "1" ]]; then
                echo -e "   \${GREEN}●\${NC} Status images enabled (No Signal/No Device BMP)"
            else
                echo -e "   \${YELLOW}○\${NC} Status images disabled (showing colorbars)"
            fi
        else
            echo -e "   \${RED}○\${NC} Parameter not available (module not loaded)"
        fi
        echo ""
        # --- ECP5 Firmware Status (4K Pro only) ---
        IS_4KP=false
        for pcidir in /sys/bus/pci/drivers/sc0710/0*; do
            if [[ -d "\$pcidir" ]]; then
                SUBVEN=\$(cat "\$pcidir/subsystem_vendor" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//')
                SUBDEV=\$(cat "\$pcidir/subsystem_device" 2>/dev/null | grep -iE '0x[0-9a-f]+' -o | sed 's/0x//')
                if [[ "\$SUBVEN:\$SUBDEV" == "1cfa:0012" ]]; then
                    IS_4KP=true
                    break
                fi
            fi
        done
        if [[ "\$IS_4KP" == "false" ]]; then
            if lspci -d ::0400 -nn 2>/dev/null | grep -qi "1cfa:0012"; then
                IS_4KP=true
            fi
        fi
        if [[ "\$IS_4KP" == "true" ]]; then
            echo -e "\${BLUE}::\${NC} \${BOLD}ECP5 Firmware\${NC}"
            FW_FOUND=false
            FW_LOCATION=""
            if [[ -f "/var/lib/sc0710/firmware/SC0710.FWI.HEX" ]]; then
                FW_FOUND=true
                FW_LOCATION="/var/lib/sc0710/firmware/SC0710.FWI.HEX"
            elif [[ -f "/etc/firmware/sc0710/SC0710.FWI.HEX" ]]; then
                FW_FOUND=true
                FW_LOCATION="/etc/firmware/sc0710/SC0710.FWI.HEX"
            elif [[ -f "/lib/firmware/sc0710/SC0710.FWI.HEX" ]]; then
                FW_FOUND=true
                FW_LOCATION="/lib/firmware/sc0710/SC0710.FWI.HEX"
            fi
            if [[ "\$FW_FOUND" == "true" ]]; then
                echo -e "   \${GREEN}●\${NC} Firmware file present"
                echo -e "     Location: \${FW_LOCATION}"
            else
                echo -e "   \${RED}○\${NC} Firmware file missing"
                echo -e "     Run: \${BOLD}sudo bash /var/lib/sc0710/sc0710-firmware.sh\${NC}"
            fi
            ECP5_MSG=\$(dmesg 2>/dev/null | grep -E "sc0710.*ECP5" | tail -1)
            if echo "\$ECP5_MSG" | grep -q "firmware programmed successfully"; then
                echo -e "   \${GREEN}●\${NC} ECP5 FPGA programmed"
            elif echo "\$ECP5_MSG" | grep -q "already configured"; then
                echo -e "   \${GREEN}●\${NC} ECP5 FPGA configured (warm reboot)"
            elif echo "\$ECP5_MSG" | grep -q "Failed to load firmware"; then
                echo -e "   \${RED}○\${NC} ECP5 FPGA not programmed (firmware load failed)"
            elif echo "\$ECP5_MSG" | grep -q "programming failed"; then
                echo -e "   \${RED}○\${NC} ECP5 FPGA programming failed"
            elif [[ -n "\$ECP5_MSG" ]]; then
                echo -e "   \${YELLOW}○\${NC} ECP5 status: \$(echo "\$ECP5_MSG" | sed 's/.*sc0710[^:]*: //')"
            else
                echo -e "   \${YELLOW}○\${NC} ECP5 status unknown (module may not be loaded)"
            fi
            if systemctl is-enabled sc0710-firmware.service >/dev/null 2>&1; then
                echo -e "   \${GREEN}●\${NC} Firmware service enabled"
            else
                echo -e "   \${YELLOW}○\${NC} Firmware service not installed"
            fi
            echo ""
        fi
        ;;
    -d|--debug)
        DBG_PATH=""
        if [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]]; then
            DBG_PATH=/sys/module/sc0710/parameters/sc0710_debug_mode
        elif [[ -f /sys/module/sc0710/parameters/debug ]]; then
            DBG_PATH=/sys/module/sc0710/parameters/debug
        fi
        if [[ -z "\$DBG_PATH" ]]; then
            echo -e "\${RED}[ERROR]\${NC} Module not loaded. Load it first with: sc0710-cli --load"
            exit 1
        fi
        CURRENT=\$(cat "\$DBG_PATH")
        if [[ "\$CURRENT" == "1" ]]; then
            echo 0 > "\$DBG_PATH"
            echo -e "\${GREEN}[OK]\${NC} Debug mode disabled (quiet)"
        else
            echo 1 > "\$DBG_PATH"
            echo -e "\${YELLOW}[OK]\${NC} Debug mode enabled (check dmesg for output)"
        fi
        save_config
        ;;
    -it|--image-toggle)
        if [[ ! -f /sys/module/sc0710/parameters/use_status_images ]]; then
            echo -e "\${RED}[ERROR]\${NC} Module not loaded. Load it first with: sc0710-cli --load"
            exit 1
        fi
        CURRENT=\$(cat /sys/module/sc0710/parameters/use_status_images)
        if [[ "\$CURRENT" == "1" ]]; then
            echo 0 > /sys/module/sc0710/parameters/use_status_images
            echo -e "\${YELLOW}[OK]\${NC} Status images disabled (showing colorbars)"
        else
            echo 1 > /sys/module/sc0710/parameters/use_status_images
            echo -e "\${GREEN}[OK]\${NC} Status images enabled (No Signal/No Device BMP)"
        fi
        save_config
        ;;
    -U|--update)
        echo -e "\${BLUE}::\${NC} Checking for updates..."

        if [[ ! -d "\$SRC_DIR" ]]; then
            echo -e "\${RED}[ERROR]\${NC} Source directory is missing. Re-run the full installer."
            exit 1
        fi

        # Download latest source
        echo -e "\${BLUE}::\${NC} Downloading latest source..."
        TEMP_TAR=\$(mktemp /tmp/sc0710-update.XXXXXX.tar.gz)
        if ! curl -fsSL "https://github.com/Nakildias/sc0710/archive/refs/heads/main.tar.gz" -o "\$TEMP_TAR"; then
            echo -e "\${RED}[ERROR]\${NC} Failed to download update. Check your internet connection."
            rm -f "\$TEMP_TAR"
            exit 1
        fi

        # Extract over existing source directory
        if ! tar -xzf "\$TEMP_TAR" --strip-components=1 -C "\$SRC_DIR"; then
            echo -e "\${RED}[ERROR]\${NC} Failed to extract update archive."
            rm -f "\$TEMP_TAR"
            exit 1
        fi
        rm -f "\$TEMP_TAR"

        # Read updated version
        NEW_VER="\${CURRENT_VERSION}"
        if [[ -f "\$SRC_DIR/version" ]]; then
            NEW_VER=\$(cat "\$SRC_DIR/version" | tr -d '[:space:]')
        fi

        # Unload the current module
        if lsmod | grep -q \$DRV_NAME; then
            echo -e "\${BLUE}::\${NC} Unloading current module..."
            \$0 --unload
        fi

        # Rebuild
        echo -e "\${BLUE}::\${NC} Rebuilding module for kernel \$(uname -r)..."
        rm -f "\$SRC_DIR/.built-for-kernel"
        cd "\$SRC_DIR"
        make clean 2>/dev/null || true
        if make KVERSION="\$(uname -r)" -j"\$(nproc)" 2>&1; then
            echo "\$(uname -r)" > "\$SRC_DIR/.built-for-kernel"
            chcon -t modules_object_t "\$SRC_DIR/\${DRV_NAME}.ko" 2>/dev/null || true
            echo -e "\${GREEN}[OK]\${NC} Module rebuilt successfully."
        else
            echo -e "\${RED}[ERROR]\${NC} Build failed."
            exit 1
        fi

        # Reload the module
        echo -e "\${BLUE}::\${NC} Loading updated module..."
        for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
            modprobe "\$dep" 2>/dev/null || true
        done
        if insmod "\$SRC_DIR/\${DRV_NAME}.ko" 2>/dev/null; then
            echo -e "\${GREEN}[OK]\${NC} Driver updated and loaded (v\${NEW_VER})."
        else
            echo -e "\${YELLOW}[WARNING]\${NC} Module rebuilt but failed to load. Try: sc0710-cli --load"
        fi
        ;;
    --rebuild)
        echo -e "\${BLUE}::\${NC} Forcing module rebuild for kernel \$(uname -r)..."
        # Unload the module first if loaded
        if lsmod | grep -q \$DRV_NAME; then
            echo -e "\${BLUE}::\${NC} Unloading current module..."
            rmmod \$DRV_NAME 2>/dev/null || true
        fi
        rm -f "\$SRC_DIR/.built-for-kernel"
        if (cd "\$SRC_DIR" && bash "\$SRC_DIR/build-and-load.sh"); then
            echo -e "\${GREEN}[OK]\${NC} Module rebuilt and loaded successfully."
        else
            echo -e "\${RED}[ERROR]\${NC} Rebuild failed. Check: /var/log/sc0710/ for build logs."
        fi
        ;;
    -r|-R|--remove)
        echo -e "\${BLUE}::\${NC} Uninstalling driver, service, and utility..."

        # Stop and disable the systemd service
        systemctl stop sc0710-build.service 2>/dev/null || true
        systemctl disable sc0710-build.service 2>/dev/null || true
        rm -f /etc/systemd/system/sc0710-build.service
        systemctl daemon-reload

        # Unload the module
        rmmod \$DRV_NAME 2>/dev/null || true

        # Remove source directory
        rm -rf "\$SRC_DIR"

        # Remove firmware service if installed
        systemctl stop sc0710-firmware.service 2>/dev/null || true
        systemctl disable sc0710-firmware.service 2>/dev/null || true
        rm -f /etc/systemd/system/sc0710-firmware.service

        # Remove configuration files (modules-load.d may exist from an older install)
        rm -f "/etc/modules-load.d/\${DRV_NAME}.conf"
        rm -f "/etc/modprobe.d/\${DRV_NAME}.conf"
        rm -f "/etc/modprobe.d/\${DRV_NAME}-params.conf"

        # Remove build logs
        rm -rf /var/log/sc0710

        # Remove CLI tool
        rm -f "/usr/local/bin/sc0710-cli"

        echo -e "\${GREEN}[OK]\${NC} Driver, systemd service, and CLI tool removed."
        echo -e "  \${YELLOW}NOTE:\${NC} Build dependencies (gcc, make, kernel-devel) remain layered."
        echo -e "  To remove them: \${BOLD}sudo rpm-ostree uninstall gcc make kernel-devel git\${NC}"
        ;;
    -v|--version)
        echo -e "\${BOLD}SC0710\${NC} Driver Control Utility (Atomic Edition)"
        echo -e "Version: \${BOLD}\${CURRENT_VERSION}\${NC}"
        check_version
        ;;
    -h|--help)
        show_help
        ;;
    *)
        echo -e "\${RED}error:\${NC} Unknown option '\$1'"
        echo -e "Use \${BOLD}-h\${NC} or \${BOLD}--help\${NC} for usage information."
        exit 1
        ;;
esac
EOF
chmod +x /usr/local/bin/sc0710-cli

# --- Final Success Message ---
log "=== Atomic Installation completed successfully ==="
echo ""
echo -e "${BOLD}${GREEN}::${NC} ${BOLD}Installation Complete.${NC}"
echo ""
echo -e " ${BLUE}->${NC} Installed for: ${BOLD}${DISTRO_NAME}${NC} (Fedora Atomic)"
echo ""
echo -e " ${BLUE}->${NC} ${BOLD}How it works on atomic distros:${NC}"
echo -e "    The driver source is stored in ${BOLD}/var/lib/sc0710/${NC} (persists across updates)."
echo -e "    A systemd service (${BOLD}sc0710-build.service${NC}) automatically rebuilds the"
echo -e "    module on each boot if the kernel version has changed."
echo ""
echo -e " ${BLUE}->${NC} New command available: ${BOLD}sc0710-cli${NC}"
echo -e "    Usage:"
echo -e "      ${BOLD}sc0710-cli -s${NC}  or  ${BOLD}--status${NC}   Check driver health"
echo -e "      ${BOLD}sc0710-cli -l${NC}  or  ${BOLD}--load${NC}     Load driver"
echo -e "      ${BOLD}sc0710-cli -u${NC}  or  ${BOLD}--unload${NC}   Unload driver"
echo -e "      ${BOLD}sc0710-cli --restart${NC}        Reload driver"
echo -e "      ${BOLD}sc0710-cli -d${NC}  or  ${BOLD}--debug${NC}    Toggle debug output"
echo -e "      ${BOLD}sc0710-cli -it${NC} or  ${BOLD}--image-toggle${NC}  Toggle status images"
echo -e ""
echo -e "      ${BOLD}sc0710-cli --rebuild${NC}        Force rebuild for current kernel"
echo -e "      ${BOLD}sc0710-cli -U${NC}  or  ${BOLD}--update${NC}   Pull latest & rebuild"
echo -e "      ${BOLD}sc0710-cli -r/R${NC} or ${BOLD}--remove${NC} Complete uninstall"
echo -e "      ${BOLD}sc0710-cli -h${NC}  or  ${BOLD}--help${NC}     Show all options"
echo ""
echo -e " ${BLUE}->${NC} Installation log available at: ${BOLD}$LOG_FILE${NC}"
echo ""

if [[ "$VIDEO_GROUP_CHANGED" == "true" ]]; then
    echo -e "${YELLOW}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${YELLOW}║                 IMPORTANT NOTICE                          ║${NC}"
    echo -e "${YELLOW}╠═══════════════════════════════════════════════════════════╣${NC}"
    echo -e "${YELLOW}║${NC}  User permissions have been updated.                      ${YELLOW}║${NC}"
    echo -e "${YELLOW}║${NC}  You ${BOLD}MUST REBOOT${NC} or ${BOLD}LOG OUT${NC} and back in for changes       ${YELLOW}║${NC}"
    echo -e "${YELLOW}║${NC}  to take effect. OBS will NOT detect the card otherwise.  ${YELLOW}║${NC}"
    echo -e "${YELLOW}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
fi
