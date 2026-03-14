#!/bin/bash
#
# SC0710 Driver Installer - Unified for Atomic and Non-Atomic distros
#
# Auto-detects distro type and runs the appropriate installation flow.
# - Atomic (Bazzite, Silverblue, Bluefin, etc.): rpm-ostree, /var/lib/sc0710, boot-time build
# - Non-atomic (Arch, Fedora, Debian, etc.): apt/pacman/dnf, DKMS or manual build
#
# Usage: sudo bash install-sc0710.sh [--force] [--noconfirm]

# --- Auto-elevate to root ---
if [[ $EUID -ne 0 ]]; then
    if [[ -f "$0" ]]; then
        exec sudo bash "$(realpath "$0")" "$@"
    else
        echo "Please run with: sudo bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/scripts/install-sc0710.sh)\""
        exit 1
    fi
fi

# --- Ensure sbin paths are in PATH ---
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

# --- Safety & Strict Mode ---
set -euo pipefail
IFS=$'\n\t'

# --- Project root (for local source when run from scripts/) ---
SCRIPT_PATH="${BASH_SOURCE[0]:-$0}"
if [[ -n "$SCRIPT_PATH" && -f "$SCRIPT_PATH" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "$SCRIPT_PATH")" 2>/dev/null && pwd)" || SCRIPT_DIR=""
    if [[ -n "$SCRIPT_DIR" && "$(basename "$SCRIPT_DIR")" == "scripts" ]]; then
        PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
    else
        PROJECT_ROOT="$(pwd)"
    fi
else
    PROJECT_ROOT="$(pwd)"
fi

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
LOG_FILE_NONATOMIC="/var/log/sc0710-install_${LOG_TIMESTAMP}.log"

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
ESSENTIAL_FILES=("lib/sc0710.h" "lib/sc0710-core.c" "lib/sc0710-video.c" "Makefile")

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

# Extract base kernel version (X.Y.Z) for comparison - avoids false warnings on
# distros like CachyOS where 6.19.6-2-cachyos and 6.19.6-arch1-1 are same base.
kernel_base_version() {
    echo "${1:-}" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0"
}

check_kernel_consistency() {
    msg2 "Verifying kernel consistency..."
    local running_ver=$(uname -r)
    if [[ ! -d "/lib/modules/${running_ver}/build" ]]; then
        echo ""
        error "CRITICAL: Headers for running kernel ($running_ver) are missing."
        printf " ${YELLOW}->${NC} Please ${RED}REBOOT${NC} your system and try again.\n"
        exit 1
    fi
    local newest_ver=$(ls -1 /lib/modules/ 2>/dev/null | sort -V | tail -n 1)
    local running_base=$(kernel_base_version "$running_ver")
    local newest_base=$(kernel_base_version "$newest_ver")
    # Only warn when newest has a strictly higher base version (e.g. 6.20 vs 6.19).
    # Same base (6.19.6-cachyos vs 6.19.6-arch1) = different flavors, no reboot needed.
    if [[ "$running_base" != "$newest_base" ]] && [[ -d "/lib/modules/${newest_ver}/build" ]]; then
        # Use sort -V to check if newest_base > running_base (actual update available)
        if [[ "$(printf '%s\n' "$running_base" "$newest_base" | sort -V | head -1)" == "$running_base" ]] && [[ "$running_base" != "$newest_base" ]]; then
            warning "Kernel update detected. Running: $running_ver, Newest: $newest_ver"
            if ! confirm "Abort and Reboot? (Recommended)" "Y"; then
                msg2 "Proceeding anyway..."
            else
                exit 0
            fi
        fi
    fi
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

# --- 1. Detect distro type (Atomic vs Non-atomic) ---
is_atomic() {
    [[ -f /run/ostree-booted ]] || command -v rpm-ostree &>/dev/null
}

msg "Verifying system compatibility..."

if is_atomic; then
log "=== SC0710 Atomic Driver Installation Started ==="
log "Version: $DRV_VERSION | Kernel: $KERNEL_VER"
IS_ATOMIC=true
SOURCE="/var/lib/sc0710"
SRC_DIR="/var/lib/sc0710"
SERVICE_NAME="sc0710-build"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

echo ""
echo -e "${BOLD}${BLUE}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${BLUE}║       SC0710 Driver Installer — Fedora Atomic Edition     ║${NC}"
echo -e "${BOLD}${BLUE}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

if ! command -v rpm-ostree >/dev/null 2>&1; then
    echo ""
    error "rpm-ostree not found. Atomic detection failed."
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
        # If the module is currently loaded, try to unload it first
        if lsmod | grep -q "$DRV_NAME"; then
            msg2 "Unloading existing module..."
            rmmod "$DRV_NAME" 2>/dev/null || true
        fi
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
    if [[ -f "$PROJECT_ROOT/Makefile" && -f "$PROJECT_ROOT/lib/sc0710.h" ]]; then
        msg "Local source detected at $PROJECT_ROOT"
        if confirm "Use local source instead of downloading?" "Y"; then
            LOCAL_MODE=true
        fi
    fi

    if [[ "$LOCAL_MODE" == "true" ]]; then
        msg2 "Copying local source..."
        cp -r "$PROJECT_ROOT"/* "$SRC_DIR/"
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
if lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi "1cfa:0012"; then
    FIRMWARE_FILE="SC0710.FWI.HEX"
    if [[ ! -f "/var/lib/sc0710/firmware/$FIRMWARE_FILE" && ! -f "/lib/firmware/sc0710/$FIRMWARE_FILE" && ! -f "/etc/firmware/sc0710/$FIRMWARE_FILE" ]]; then
        msg "4K Pro detected — extracting ECP5 firmware..."
        EXT_SCRIPT="$SOURCE/scripts/extract-firmware.sh"
        if [[ -f "$EXT_SCRIPT" ]]; then
            chmod +x "$EXT_SCRIPT"
            if bash "$EXT_SCRIPT"; then
                msg2 "Firmware extraction completed."
                log "4K Pro firmware extracted"
            else
                warning "Firmware extraction failed."
            fi
        else
            warning "scripts/extract-firmware.sh not found. Firmware must be installed manually."
        fi
    else
        msg2 "4K Pro firmware already present"
    fi
else
    log "No 4K Pro card detected, skipping firmware extraction"
fi

# --- 5.6. Firmware Service (4K Pro only) ---
# Install a systemd service that ensures the ECP5 firmware file is present
# on every boot and triggers a driver reload if the FPGA wasn't programmed.
if lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi "1cfa:0012"; then
    if systemctl is-enabled sc0710-firmware.service >/dev/null 2>&1; then
        msg2 "4K Pro firmware service already installed and enabled"
    else
        msg "4K Pro detected — installing firmware service..."

        if [[ "$IS_ATOMIC" == "true" ]]; then
            FW_SERVICE_SCRIPT="/var/lib/sc0710/sc0710-firmware.sh"
        else
            FW_SERVICE_SCRIPT="/usr/local/libexec/sc0710-firmware.sh"
            mkdir -p "$(dirname "$FW_SERVICE_SCRIPT")"
        fi
        FW_SERVICE_SCRIPT_SRC="$SOURCE/scripts/sc0710-firmware.sh"
        if [[ -f "$FW_SERVICE_SCRIPT_SRC" ]]; then
            cp "$FW_SERVICE_SCRIPT_SRC" "$FW_SERVICE_SCRIPT"
        elif [[ -f "./scripts/sc0710-firmware.sh" ]]; then
            cp "./scripts/sc0710-firmware.sh" "$FW_SERVICE_SCRIPT"
        else
            warning "scripts/sc0710-firmware.sh not found in source tree."
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
BUILT_MOD="$SRC_DIR/build/${DRV_NAME}.ko"
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
chcon -t modules_object_t "$SRC_DIR/build/${DRV_NAME}.ko" 2>/dev/null || true

log "Module built at $SRC_DIR/build/${DRV_NAME}.ko"

# Load dependency modules (these are part of the immutable kernel image, modprobe works)
for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do
    modprobe "$dep" 2>/dev/null || log "WARNING: Failed to load dependency: $dep"
done

# Load the driver via insmod with retry logic.
# At boot, the V4L2/media subsystem may still be initializing after modprobe returns.
# Retry up to 5 times with increasing delays to handle this race condition.
LOADED=false
for attempt in 1 2 3; do
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
chcon -t modules_object_t "$SRC_DIR/build/${DRV_NAME}.ko" 2>/dev/null || true

log "Module built at $SRC_DIR/build/${DRV_NAME}.ko"

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
if ! DRIVER_ERR=$(insmod "$SRC_DIR/build/${DRV_NAME}.ko" 2>&1); then
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

else
# --- NON-ATOMIC FLOW ---
log "=== SC0710 Driver Installation Started (Non-Atomic) ==="
log "Version: $DRV_VERSION | Kernel: $KERNEL_VER"
IS_ATOMIC=false
SRC_DEST="/usr/src/${DRV_NAME}-${DRV_VERSION}"
SOURCE="$SRC_DEST"
LOG_FILE="$LOG_FILE_NONATOMIC"

echo ""
echo -e "${BOLD}${BLUE}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${BLUE}║         SC0710 Driver Installer — Standard Edition        ║${NC}"
echo -e "${BOLD}${BLUE}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

msg2 "Checking system dependencies..."
PKG_MANAGER=""
OS_ID=""
OS_ID_LIKE=""
if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    OS_ID="$ID"
    OS_ID_LIKE="${ID_LIKE:-}"
fi
if [[ "$OS_ID" =~ ^(fedora|rhel|centos|almalinux|rocky|ol)$ ]] || [[ "$OS_ID_LIKE" =~ (fedora|rhel) ]] || command -v dnf >/dev/null 2>&1; then
    PKG_MANAGER="dnf"
elif [[ "$OS_ID" =~ ^(arch|manjaro|endeavouros)$ ]] || command -v pacman >/dev/null 2>&1; then
    PKG_MANAGER="pacman"
elif [[ "$OS_ID" =~ ^(debian|ubuntu|pop|linuxmint|kali|raspbian)$ ]] || command -v apt-get >/dev/null 2>&1; then
    PKG_MANAGER="apt"
fi

case "$PKG_MANAGER" in
    pacman)
        msg2 "Installing missing dependencies (pacman)..."
        HEADERS_PKG="linux-headers"
        if [[ "$OS_ID" == "manjaro" ]]; then
            KERNEL_MAJOR=$(echo "$KERNEL_VER" | cut -d. -f1)
            KERNEL_MINOR=$(echo "$KERNEL_VER" | cut -d. -f2)
            MANJARO_HEADERS="linux${KERNEL_MAJOR}${KERNEL_MINOR}-headers"
            pacman -Si "$MANJARO_HEADERS" >/dev/null 2>&1 && HEADERS_PKG="$MANJARO_HEADERS"
        fi
        pacman -S --needed --noconfirm base-devel "$HEADERS_PKG" git dkms >/dev/null 2>&1 || true
        if [[ ! -d "/lib/modules/$KERNEL_VER/build" ]]; then
            error "Kernel headers for $KERNEL_VER still missing."
            exit 1
        fi
        grep -qs '^CONFIG_CC_IS_CLANG=y' "/lib/modules/$KERNEL_VER/build/.config" 2>/dev/null && pacman -S --needed --noconfirm clang lld >/dev/null 2>&1 || true
        ;;
    apt)
        msg2 "Installing dependencies (apt)..."
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq 2>&1 | tee -a "$LOG_FILE"
        apt-get install -y build-essential linux-headers-"$(uname -r)" git dkms 2>&1 | tee -a "$LOG_FILE" || { error "Failed to install dependencies via apt"; exit 1; }
        ;;
    dnf)
        msg2 "Installing dependencies (dnf)..."
        KERNEL_MODULES_PKG="kernel-modules-$(uname -r)"
        rpm -q "$KERNEL_MODULES_PKG" >/dev/null 2>&1 || dnf install -y "$KERNEL_MODULES_PKG" 2>&1 | tee -a "$LOG_FILE" || true
        dnf install -y kernel-devel kernel-headers gcc make git dkms 2>&1 | tee -a "$LOG_FILE" || { error "Failed to install dependencies via dnf"; exit 1; }
        ;;
    *)
        warning "Could not detect package manager (apt/pacman/dnf). Assuming dependencies are met."
        ;;
esac

command -v dkms >/dev/null 2>&1 || { error "DKMS is not installed."; exit 1; }
check_kernel_consistency

if lsmod | grep -q "$DRV_NAME"; then
    warning "Module $DRV_NAME is currently loaded."
    if ! rmmod "$DRV_NAME" 2>/dev/null; then
        if confirm "Force unload now?" "N"; then
            rmmod -f "$DRV_NAME" 2>/dev/null || { error "Force unload failed."; exit 1; }
        else
            die "Cannot proceed while module is in use."
        fi
    fi
fi

[[ -d "$SRC_DEST" ]] && rm -rf "$SRC_DEST"
mkdir -p "$SRC_DEST"
LOCAL_MODE=false
[[ -f "$PROJECT_ROOT/Makefile" && -f "$PROJECT_ROOT/lib/sc0710.h" ]] && confirm "Use local source?" "Y" && LOCAL_MODE=true
if [[ "$LOCAL_MODE" == "true" ]]; then
    cp -r "$PROJECT_ROOT"/* "$SRC_DEST/"
else
    TEMP_DIR=$(mktemp -d -t sc0710.XXXXXX) || die "Failed to create temp directory"
    git clone --depth 1 "$REPO_URL" "$TEMP_DIR" >/dev/null 2>&1 || die "Git clone failed."
    cp -r "$TEMP_DIR"/* "$SRC_DEST/"
fi
verify_essential_files "$SRC_DEST" || die "Source verification failed."

if lspci -n -v -d 12ab:0710 2>/dev/null | grep -qi "1cfa:0012"; then
    if [[ ! -f "/var/lib/sc0710/firmware/SC0710.FWI.HEX" && ! -f "/lib/firmware/sc0710/SC0710.FWI.HEX" && ! -f "/etc/firmware/sc0710/SC0710.FWI.HEX" ]]; then
        msg "4K Pro detected — extracting ECP5 firmware..."
        [[ -f "$SOURCE/scripts/extract-firmware.sh" ]] && bash "$SOURCE/scripts/extract-firmware.sh" && msg2 "Firmware extracted." || warning "Firmware extraction failed."
    fi
    if ! systemctl is-enabled sc0710-firmware.service >/dev/null 2>&1; then
        msg "4K Pro — installing firmware service..."
        FW_SERVICE_SCRIPT="/usr/local/libexec/sc0710-firmware.sh"
        mkdir -p "$(dirname "$FW_SERVICE_SCRIPT")"
        [[ -f "$SOURCE/scripts/sc0710-firmware.sh" ]] && cp "$SOURCE/scripts/sc0710-firmware.sh" "$FW_SERVICE_SCRIPT" && chmod +x "$FW_SERVICE_SCRIPT"
        if [[ -f "$FW_SERVICE_SCRIPT" ]]; then
            cat > /etc/systemd/system/sc0710-firmware.service << 'FWSVC'
[Unit]
Description=SC0710 4K Pro ECP5 Firmware Loader
After=local-fs.target network-online.target
Wants=network-online.target
Before=sc0710-build.service
ConditionPathExists=/usr/local/libexec/sc0710-firmware.sh

[Service]
Type=oneshot
ExecStart=/bin/bash /usr/local/libexec/sc0710-firmware.sh
RemainAfterExit=yes
TimeoutStartSec=120
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
FWSVC
            systemctl daemon-reload
            systemctl enable sc0710-firmware.service
        fi
    fi
fi

USE_DKMS=false
confirm "Enable automatic updates (DKMS)?" "Y" && USE_DKMS=true
if [[ "$USE_DKMS" == "true" ]]; then
    cat > "$SRC_DEST/dkms.conf" << DKMSEOF
PACKAGE_NAME="$DRV_NAME"
PACKAGE_VERSION="$DRV_VERSION"
BUILT_MODULE_NAME[0]="$DRV_NAME"
DEST_MODULE_LOCATION[0]="/kernel/drivers/media/pci/"
AUTOINSTALL="yes"
BUILT_MODULE_LOCATION[0]="build/"
MAKE[0]="make KVERSION=\$kernelver -j\$(nproc)"
DKMSEOF
    if dkms status 2>/dev/null | grep -q "$DRV_NAME"; then
        confirm "Remove existing DKMS and reinstall?" "Y" && for ver_item in $(dkms status 2>/dev/null | awk -F'[:,]' '/^sc0710/ {print $1}' | tr -d ' '); do dkms remove "$ver_item" --all >/dev/null 2>&1 || true; done
    fi
    dkms add -m "$DRV_NAME" -v "$DRV_VERSION" >/dev/null 2>&1 || true
    dkms build -m "$DRV_NAME" -v "$DRV_VERSION" -k "$KERNEL_VER" 2>&1 | tee -a "$LOG_FILE" || { error "DKMS build failed."; exit 1; }
    dkms install -m "$DRV_NAME" -v "$DRV_VERSION" -k "$KERNEL_VER" --force 2>&1 | tee -a "$LOG_FILE" || { error "DKMS install failed."; exit 1; }
else
    cd "$SRC_DEST"
    make -j"$(nproc)" 2>&1 | tee -a "$LOG_FILE" || { error "Build failed."; exit 1; }
    mkdir -p "/lib/modules/$KERNEL_VER/kernel/drivers/media/pci/"
    cp "build/${DRV_NAME}.ko" "/lib/modules/$KERNEL_VER/kernel/drivers/media/pci/"
    depmod -a
fi

confirm "Load driver automatically on boot?" "Y" && echo "$DRV_NAME" > "/etc/modules-load.d/${DRV_NAME}.conf" || rm -f "/etc/modules-load.d/${DRV_NAME}.conf"
echo 'softdep sc0710 pre: videodev videobuf2-v4l2 videobuf2-vmalloc videobuf2-common snd-pcm' > /etc/modprobe.d/${DRV_NAME}.conf

msg2 "Loading module..."
for dep in videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc snd-pcm; do modprobe "$dep" 2>/dev/null || true; done
if ! DRIVER_ERR=$(modprobe "$DRV_NAME" 2>&1); then
    error "Failed to load $DRV_NAME. Error: $DRIVER_ERR"
    warning "Driver installed but could not be loaded. It may work after a reboot."
else
    msg2 "Driver loaded successfully!"
fi

fi

# --- 10. Install CLI Tool ---
msg "Installing CLI utility..."
if [[ -f "$SOURCE/scripts/sc0710-cli.sh" ]]; then
    cp "$SOURCE/scripts/sc0710-cli.sh" /usr/local/bin/sc0710-cli
    chmod +x /usr/local/bin/sc0710-cli
else
    warning "scripts/sc0710-cli.sh not found in source. CLI not installed."
fi


# --- Final Success Message ---
log "=== Installation completed successfully ==="
echo ""
echo -e "${BOLD}${GREEN}::${NC} ${BOLD}Installation Complete.${NC}"
echo ""
if [[ "$IS_ATOMIC" == "true" ]]; then
    echo -e " ${BLUE}->${NC} Installed for: ${BOLD}${DISTRO_NAME:-Fedora Atomic}${NC}"
    echo ""
    echo -e " ${BLUE}->${NC} ${BOLD}How it works on atomic distros:${NC}"
    echo -e "    The driver source is stored in ${BOLD}/var/lib/sc0710/${NC} (persists across updates)."
    echo -e "    A systemd service (${BOLD}sc0710-build.service${NC}) automatically rebuilds the"
    echo -e "    module on each boot if the kernel version has changed."
fi
echo ""
echo -e " ${BLUE}->${NC} New command available: ${BOLD}sc0710-cli${NC}"
echo -e "    Usage:"
echo -e "      ${BOLD}sc0710-cli -s${NC}  or  ${BOLD}--status${NC}   Check driver health"
echo -e "      ${BOLD}sc0710-cli -l${NC}  or  ${BOLD}--load${NC}     Load driver"
echo -e "      ${BOLD}sc0710-cli -u${NC}  or  ${BOLD}--unload${NC}   Unload driver"
echo -e "      ${BOLD}sc0710-cli --restart${NC}        Reload driver"
echo -e "      ${BOLD}sc0710-cli -d${NC}  or  ${BOLD}--debug${NC}    Toggle debug output"
echo -e "      ${BOLD}sc0710-cli -it${NC} or  ${BOLD}--image-toggle${NC}  Toggle status images"
echo -e "      ${BOLD}sc0710-cli -ss${NC} or  ${BOLD}--software-scaler${NC} Toggle software scaler (all cards)"
echo -e "      ${BOLD}sc0710-cli -as${NC} or ${BOLD}--toggle-auto-scalar${NC} Toggle automatic safety scaler"
echo -e "      ${BOLD}sc0710-cli -pt${NC} or ${BOLD}--procedural-timings${NC} Toggle timing calculation mode"
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
