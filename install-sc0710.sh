#!/bin/bash
#
# SC0710 Driver Installer
#
# Usage: ./install.sh [--force] [--noconfirm]

# --- Auto-elevate to root ---
if [[ $EUID -ne 0 ]]; then
    # Check if we're running from an actual file or piped input
    if [[ -f "$0" ]]; then
        exec sudo bash "$(realpath "$0")" "$@"
    else
        echo "Please run with: sudo bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/install-sc0710.sh)\""
        exit 1
    fi
fi

# --- Ensure sbin paths are in PATH (Debian root fix) ---
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

# --- Safety & Strict Mode ---
set -euo pipefail
IFS=$'\n\t'

# --- Configuration ---
REPO_URL="https://github.com/Nakildias/sc0710.git"
VERSION_URL="https://raw.githubusercontent.com/Nakildias/sc0710/main/version"
DRV_NAME="sc0710"
# Fetch from local version file if exists, otherwise fetch from GitHub
if [[ -f "version" ]]; then
    DRV_VERSION="$(cat version | tr -d '[:space:]')"
else
    DRV_VERSION=$(curl -fsSL "$VERSION_URL" | tr -d '[:space:]')
fi
SRC_DEST="/usr/src/${DRV_NAME}-${DRV_VERSION}"
KERNEL_VER="$(uname -r)"

# --- Logging ---
LOG_TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
LOG_FILE="/var/log/sc0710-install_${LOG_TIMESTAMP}.log"

# --- Visual Definition (Pacman Style) ---
BOLD='\033[1m'
BLUE='\033[1;34m'
GREEN='\033[1;32m'
RED='\033[1;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

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

# --- Verification Functions ---
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

check_module_blacklist() {
    if grep -rq "blacklist.*$DRV_NAME" /etc/modprobe.d/ 2>/dev/null; then
        warning "Module $DRV_NAME is blacklisted in /etc/modprobe.d/"
        warning "Remove blacklist entry before loading"
        return 1
    fi
    return 0
}

check_kernel_consistency() {
    msg2 "Verifying kernel consistency..."
    local running_ver=$(uname -r)

    # CHECK 1: The "Arch Linux" Trap
    # If the headers for the RUNNING kernel are gone, we literally cannot build.
    if [[ ! -d "/lib/modules/${running_ver}/build" ]]; then
        echo ""
        error "CRITICAL: Headers for running kernel ($running_ver) are missing."
        printf " ${YELLOW}->${NC} This indicates a kernel update occurred, but the system has not rebooted.\n"
        printf " ${YELLOW}->${NC} The installer cannot build for a kernel that has been removed from disk.\n"
        echo ""
        printf "${BOLD}ACTION REQUIRED:${NC} Please ${RED}REBOOT${NC} your system and try again.\n"
        exit 1
    fi

    # CHECK 2: The "Pending Update" Trap
    # If the user is on 6.5 but 6.6 is installed, building for 6.5 is pointless.
    # We find the newest folder in /lib/modules/
    local newest_ver=$(ls -1 /lib/modules/ | sort -V | tail -n 1)

    # Only warn if the versions are different AND the new version looks valid
    if [[ "$running_ver" != "$newest_ver" ]] && [[ -d "/lib/modules/${newest_ver}/build" ]]; then
        warning "Kernel update detected."
        printf "    Running Kernel:   ${BOLD}$running_ver${NC}\n"
        printf "    Newest Installed: ${BOLD}$newest_ver${NC}\n"
        echo ""
        warning "You are installing a driver for an OLD kernel."
        printf " ${YELLOW}->${NC} This driver will likely stop working as soon as you reboot.\n"

        if ! confirm "Abort and Reboot? (Recommended)" "Y"; then
            msg2 "Proceeding anyway (Not recommended)..."
        else
            msg "Aborted. Please reboot to load the new kernel."
            exit 0
        fi
    fi
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        die "This script must be run as root."
    fi
}

check_video_group() {
    # Check if users are in the video group to access the capture card
    local users
    # Get users with UID >= 1000 (usually human users)
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

# Set trap AFTER root check to avoid permission issues during cleanup
setup_trap() {
    trap cleanup EXIT INT TERM
}

# --- Prompt Function ---
confirm() {
    local prompt_text="$1"
    local default_ans="$2" # Y or N

    if [[ "$NOCONFIRM" == "true" ]]; then
        return 0
    fi

    local brackets
    if [[ "$default_ans" == "Y" ]]; then brackets="[Y/n]"; else brackets="[y/N]"; fi

    printf "${BLUE}::${NC} ${BOLD}%s %s${NC} " "$prompt_text" "$brackets"
    read -r -n 1 response
    echo "" # Newline

    if [[ -z "$response" ]]; then response="$default_ans"; fi
    if [[ ! "$response" =~ ^[yY]$ ]]; then return 1; fi
    return 0
}

# --- Argument Parsing ---
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--force) FORCE_INSTALL=true; shift ;;
        --noconfirm) NOCONFIRM=true; shift ;;
        *) shift ;;
    esac
done

# ==============================================================================
# MAIN EXECUTION
# ==============================================================================

setup_trap
log "=== SC0710 Driver Installation Started ==="
log "Version: $DRV_VERSION | Kernel: $KERNEL_VER"
msg "Initializing SC0710 Driver Installer..."

# 1. Permission Check
check_video_group

# 2. Dependency Check
msg2 "Checking system dependencies..."

# Detect Package Manager Strategy
PKG_MANAGER=""
OS_ID=""
OS_ID_LIKE=""

if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="$ID"
    OS_ID_LIKE="${ID_LIKE:-}"
fi

# Check if it's a Fedora-based distro (check both ID and ID_LIKE)
if [[ "$OS_ID" =~ ^(fedora|rhel|centos|almalinux|rocky|ol)$ ]] || \
   [[ "$OS_ID_LIKE" =~ (fedora|rhel) ]] || \
   command -v dnf >/dev/null 2>&1; then
    PKG_MANAGER="dnf"
elif [[ "$OS_ID" =~ ^(arch|manjaro|endeavouros)$ ]] || command -v pacman >/dev/null 2>&1; then
    PKG_MANAGER="pacman"
elif [[ "$OS_ID" =~ ^(debian|ubuntu|pop|linuxmint|kali|raspbian)$ ]] || command -v apt-get >/dev/null 2>&1; then
    PKG_MANAGER="apt"
fi

# 2. Execute Installation based on Strategy
case "$PKG_MANAGER" in
    pacman)
        # Arch / Pacman based (including Manjaro, EndeavourOS, CachyOS)
        msg2 "Installing missing dependencies (pacman)..."
        
        # Determine correct headers package
        HEADERS_PKG="linux-headers"
        if [[ "$OS_ID" == "manjaro" ]]; then
            # Manjaro uses versioned kernel packages like linux618-headers
            # Extract major.minor from kernel version (e.g., 6.18.3 -> 618)
            KERNEL_MAJOR=$(echo "$KERNEL_VER" | cut -d. -f1)
            KERNEL_MINOR=$(echo "$KERNEL_VER" | cut -d. -f2)
            MANJARO_HEADERS="linux${KERNEL_MAJOR}${KERNEL_MINOR}-headers"
            
            # Check if the Manjaro-specific package exists
            if pacman -Si "$MANJARO_HEADERS" >/dev/null 2>&1; then
                HEADERS_PKG="$MANJARO_HEADERS"
                log "Manjaro detected: using $HEADERS_PKG"
            else
                warning "Could not find $MANJARO_HEADERS, trying generic linux-headers"
            fi
        fi
        
        # Install dependencies
        pacman -S --needed --noconfirm base-devel "$HEADERS_PKG" git dkms >/dev/null 2>&1 || true
        
        # Verify headers are actually installed
        if [[ ! -d "/lib/modules/$KERNEL_VER/build" ]]; then
            error "Kernel headers for $KERNEL_VER are still missing after install attempt."
            if [[ "$OS_ID" == "manjaro" ]]; then
                echo -e "  ${YELLOW}Manjaro users:${NC} Try: ${BOLD}sudo pacman -S linux${KERNEL_MAJOR}${KERNEL_MINOR}-headers${NC}"
            fi
            exit 1
        fi

        # Auto-detect Clang-built kernels (CachyOS, etc.) and install LLVM toolchain
        if grep -qs '^CONFIG_CC_IS_CLANG=y' "/lib/modules/$KERNEL_VER/build/.config" 2>/dev/null; then
            msg2 "Clang-built kernel detected, installing LLVM toolchain..."
            pacman -S --needed --noconfirm clang lld >/dev/null 2>&1 || true
            log "Installed clang/lld for Clang-built kernel"
        fi
        ;;
    apt)
        # Debian / Apt based
        msg2 "Installing dependencies (apt)..."
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq 2>&1 | tee -a "$LOG_FILE"
        if ! apt-get install -y build-essential linux-headers-"$(uname -r)" git dkms 2>&1 | tee -a "$LOG_FILE"; then
            error "Failed to install dependencies via apt"
            exit 1
        fi
        ;;
    dnf)
        # RedHat / Dnf based
        msg2 "Installing dependencies (dnf)..."
        
        # Fedora splits kernel modules into separate packages
        # Ensure the full kernel-modules package is installed (not just -core)
        KERNEL_MODULES_PKG="kernel-modules-$(uname -r)"
        if ! rpm -q "$KERNEL_MODULES_PKG" >/dev/null 2>&1; then
            msg2 "Installing missing kernel modules package..."
            if ! dnf install -y "$KERNEL_MODULES_PKG" 2>&1 | tee -a "$LOG_FILE"; then
                warning "Could not install $KERNEL_MODULES_PKG - some features may not work"
            fi
        fi
        
        if ! dnf install -y kernel-devel kernel-headers gcc make git dkms 2>&1 | tee -a "$LOG_FILE"; then
            error "Failed to install dependencies via dnf"
            exit 1
        fi
        ;;
    *)
        warning "Could not detect a supported package manager (apt/pacman/dnf). Assuming dependencies are met."
        ;;
esac

# Verify critical tools are available
if ! command -v dkms >/dev/null 2>&1; then
    error "DKMS is not installed. Please install it manually:"
    echo -e "  Debian/Ubuntu: ${BOLD}sudo apt install dkms${NC}"
    echo -e "  Fedora/RHEL:   ${BOLD}sudo dnf install dkms${NC}"
    echo -e "  Arch:          ${BOLD}sudo pacman -S dkms${NC}"
    exit 1
fi

check_kernel_consistency

# 2. Existing Driver Check & Smart Unload
if lsmod | grep -q "$DRV_NAME"; then
    warning "Module $DRV_NAME is currently loaded."

    # Attempt gentle unload
    if ! rmmod "$DRV_NAME" 2>/dev/null; then
        echo ""
        echo -e "${YELLOW}═══════════════════════════════════════════════════════════════${NC}"
        echo -e "${YELLOW}  The module is currently in use by the kernel.${NC}"
        echo -e "${YELLOW}═══════════════════════════════════════════════════════════════${NC}"
        echo -e "  Please close any applications using the capture card:"
        echo -e "    • OBS Studio"
        echo -e "    • Zoom / Discord / Teams"
        echo -e "    • Any video player or streaming software"
        echo ""
        echo -e "  You can check what's using it with: ${BOLD}lsof /dev/video*${NC}"
        echo -e "${YELLOW}═══════════════════════════════════════════════════════════════${NC}"
        echo ""
        
        if confirm "Have you closed all applications? Force unload now?" "N"; then
            msg2 "Attempting forced removal..."
            log "User confirmed force unload"
            
            # Force flag after user confirmation
            if ! rmmod -f "$DRV_NAME" 2>/dev/null; then
                error "Force unload failed. The kernel is still holding the module."
                error "A reboot may be required, or try closing more applications."
                log "Force unload failed"
                exit 1
            else
                msg2 "Module unloaded successfully."
                log "Module force-unloaded successfully"
            fi
        else
            die "Cannot proceed while module is in use. Close applications and try again."
        fi
    else
        msg2 "Module unloaded."
        log "Module unloaded normally"
    fi
fi

# 3. Source Setup
if [ -d "$SRC_DEST" ]; then rm -rf "$SRC_DEST"; fi
mkdir -p "$SRC_DEST"

# --- Local/Online Mode Detection ---
LOCAL_MODE=false
if [[ -f "./Makefile" && -f "./sc0710.h" ]]; then
    msg "Local source detected in current directory."
    if confirm "Use local source instead of downloading?" "Y"; then
        LOCAL_MODE=true
    fi
fi

if [ "$LOCAL_MODE" = true ]; then
    msg2 "Copying local source..."
    cp -r ./* "$SRC_DEST/"
    log "Copied local source to $SRC_DEST"
else
    msg2 "Downloading source..."
    TEMP_DIR=$(mktemp -d -t sc0710.XXXXXX) || die "Failed to create temp directory"
    log "Created temp directory: $TEMP_DIR"
    
    if ! git clone --depth 1 "$REPO_URL" "$TEMP_DIR" >/dev/null 2>&1; then
        die "Git clone failed. Check your internet connection."
    fi
    log "Git clone successful"
    cp -r "$TEMP_DIR"/* "$SRC_DEST/"
fi

# Verify essential files are present
msg2 "Verifying source integrity..."
if ! verify_essential_files "$SRC_DEST"; then
    die "Source verification failed. The download may be corrupted."
fi
log "Source verification passed"

# 3.5 Firmware Extraction (4K Pro only)
# The Elgato 4K Pro (subsystem 1cfa:0012) requires ECP5 companion FPGA firmware
# at /lib/firmware/sc0710/SC0710.FWI.HEX. The driver uploads this on every boot.
# Only run if a 4K Pro card is detected in the system.
if lspci -nn 2>/dev/null | grep -qi "1cfa:0012"; then
    FIRMWARE_PATH="/lib/firmware/sc0710/SC0710.FWI.HEX"
    if [[ ! -f "$FIRMWARE_PATH" ]]; then
        msg "4K Pro detected — extracting ECP5 firmware..."
        EXT_SCRIPT="$SRC_DEST/extract-firmware.sh"
        if [[ ! -f "$EXT_SCRIPT" ]]; then
            # Mirror of 3.6 logic: handle missing script from source
            EXT_SCRIPT="./extract-firmware.sh"
        fi

        if [[ -f "$EXT_SCRIPT" ]]; then
            chmod +x "$EXT_SCRIPT"
            if bash "$EXT_SCRIPT"; then
                msg2 "Firmware extraction completed."
                log "4K Pro firmware extracted to $FIRMWARE_PATH"
            else
                warning "Firmware extraction failed."
                log "WARNING: Firmware extraction failed"
            fi
        else
            warning "extract-firmware.sh not found. Firmware must be installed manually."
        fi
    else
        msg2 "4K Pro firmware already present at $FIRMWARE_PATH"
    fi
else
    log "No 4K Pro card detected, skipping firmware extraction"
fi

# 3.6 Firmware Service (4K Pro only)
# Install a systemd service that ensures the ECP5 firmware file is present
# on every boot and triggers a driver reload if the FPGA wasn't programmed.
# This handles cold boots where the ECP5 SRAM is wiped.
if lspci -nn 2>/dev/null | grep -qi "1cfa:0012"; then
    if systemctl is-enabled sc0710-firmware.service >/dev/null 2>&1; then
        msg2 "4K Pro firmware service already installed and enabled"
    else
        msg "4K Pro detected — installing firmware service..."

        # Install the firmware service script
        FW_SERVICE_SCRIPT="/usr/local/libexec/sc0710-firmware.sh"
        FW_SERVICE_SCRIPT_SRC="$SRC_DEST/sc0710-firmware.sh"
        mkdir -p "$(dirname "$FW_SERVICE_SCRIPT")"
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
    fi
else
    log "No 4K Pro card detected, skipping firmware service installation"
fi

# 4. Auto-Update / DKMS Selection
USE_DKMS=false
echo ""
if confirm "Enable automatic updates (DKMS)?" "Y"; then
    USE_DKMS=true
else
    msg2 "Manual build selected. Driver will NOT update with kernel."
fi

# 5. Build Process
if [ "$USE_DKMS" = true ]; then
    # --- DKMS PATH ---
    # Create config
    cat > "$SRC_DEST/dkms.conf" <<EOF
PACKAGE_NAME="$DRV_NAME"
PACKAGE_VERSION="$DRV_VERSION"
BUILT_MODULE_NAME[0]="$DRV_NAME"
DEST_MODULE_LOCATION[0]="/kernel/drivers/media/pci/"
AUTOINSTALL="yes"
MAKE[0]="make KVERSION=\$kernelver"
EOF

    # Check for existing DKMS installation (any version)
    msg2 "Checking for existing DKMS installation..."
    DKMS_STATUS=$(dkms status 2>/dev/null || true)
    if echo "$DKMS_STATUS" | grep -q "$DRV_NAME"; then
        warning "DKMS already has $DRV_NAME installed."
        if confirm "Remove existing and reinstall?" "Y"; then
            msg2 "Removing all existing DKMS versions..."
            for ver_item in $(dkms status 2>/dev/null | awk -F'[:,]' '/^sc0710/ {print $1}' | tr -d ' '); do
                if ! dkms remove "$ver_item" --all >/dev/null 2>&1; then
                    cleanup_path=$(echo "$ver_item" | tr ',' '/')
                    msg2 "Cleaning stale DKMS entry: $cleanup_path"
                    rm -rf "/var/lib/dkms/$cleanup_path" 2>/dev/null || true
                fi
            done
            # Remove any leftover top-level DKMS directory if empty
            rmdir "/var/lib/dkms/${DRV_NAME}" 2>/dev/null || true
        else
            msg2 "Skipping DKMS rebuild. Keeping existing installation."
            USE_DKMS=false
        fi
    fi

    if [ "$USE_DKMS" = true ]; then
        msg2 "Building and Installing via DKMS..."
        dkms add -m "$DRV_NAME" -v "$DRV_VERSION" >/dev/null 2>&1 || true
        log "DKMS add completed"
        
        if ! dkms build -m "$DRV_NAME" -v "$DRV_VERSION" -k "$KERNEL_VER" 2>&1 | tee -a "$LOG_FILE"; then
            error "DKMS Build failed. Check the log at: $LOG_FILE"
            exit 1
        fi
        log "DKMS build completed"
        
        if ! dkms install -m "$DRV_NAME" -v "$DRV_VERSION" -k "$KERNEL_VER" --force 2>&1 | tee -a "$LOG_FILE"; then
            error "DKMS Install failed. Check the log at: $LOG_FILE"
            exit 1
        fi
        log "DKMS install completed"
    fi

else
    # --- MANUAL PATH ---
    msg2 "Compiling driver manually..."
    cd "$SRC_DEST"
    if ! make 2>&1 | tee -a "$LOG_FILE"; then
        error "Build failed. Check the log at: $LOG_FILE"
        exit 1
    fi
    log "Manual build completed"

    msg2 "Installing .ko file..."
    INSTALL_MOD_PATH="/lib/modules/$KERNEL_VER/kernel/drivers/media/pci/"
    mkdir -p "$INSTALL_MOD_PATH"
    cp "${DRV_NAME}.ko" "$INSTALL_MOD_PATH"
    depmod -a
    log "Module installed to $INSTALL_MOD_PATH"
fi

# 6. Autostart Selection
echo ""
if confirm "Load driver automatically on boot?" "Y"; then
    msg2 "Enabling autostart..."
    echo "$DRV_NAME" > "/etc/modules-load.d/${DRV_NAME}.conf"

    # Add softdeps for all required modules (V4L2 + ALSA)
    cat > "/etc/modprobe.d/${DRV_NAME}.conf" <<EOF
softdep $DRV_NAME pre: videodev videobuf2-v4l2 videobuf2-vmalloc videobuf2-common snd-pcm
EOF
else
    msg2 "Autostart disabled. Load manually with 'modprobe $DRV_NAME'."
    rm -f "/etc/modules-load.d/${DRV_NAME}.conf"
fi

# 7. Final Load
msg2 "Loading module..."

# Try to load dependency modules and track failures
FAILED_DEPS=()
DEP_ERRORS=""

load_dep() {
    local mod="$1"
    local modname="${mod//-/_}"  # modprobe uses - but lsmod uses _
    
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

# Show error if dependencies failed
if [[ ${#FAILED_DEPS[@]} -gt 0 ]]; then
    echo ""
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${RED}  KERNEL MODULE ISSUE DETECTED${NC}"
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "  The following required kernel modules failed to load:"
    echo ""
    echo -e "${YELLOW}${DEP_ERRORS}${NC}"
    echo -e "  This indicates a problem with your kernel package, not the driver."
    echo -e "  Possible solutions:"
    
    # Show distro-specific reinstall command
    case "$PKG_MANAGER" in
        pacman)
            echo -e "    1. Reinstall kernel: ${BOLD}sudo pacman -S linux linux-headers${NC}"
            ;;
        apt)
            echo -e "    1. Reinstall kernel modules: ${BOLD}sudo apt reinstall linux-modules-$(uname -r)${NC}"
            ;;
        dnf)
            echo -e "    1. Reinstall kernel modules: ${BOLD}sudo dnf reinstall kernel-modules-$(uname -r)${NC}"
            ;;
        *)
            echo -e "    1. Reinstall your kernel and kernel modules package"
            ;;
    esac
    
    echo -e "    2. Downgrade to a working kernel version"
    echo -e "    3. Wait for a kernel update from your distribution"
    echo ""
    echo -e "${BOLD}Recent kernel messages:${NC}"
    dmesg | tail -10 | sed 's/^/  /'
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
    log "ERROR: Failed to load kernel modules: ${FAILED_DEPS[*]}"
fi

# Load the driver
if ! DRIVER_ERR=$(modprobe "$DRV_NAME" 2>&1); then
    echo ""
    error "Failed to load $DRV_NAME module."
    echo -e "  ${YELLOW}Error: ${DRIVER_ERR}${NC}"
    echo ""
    echo -e "${BOLD}Recent kernel messages:${NC}"
    dmesg | tail -10 | sed 's/^/  /'
    echo ""
    log "ERROR: modprobe $DRV_NAME failed: $DRIVER_ERR"
    warning "The driver was installed but could not be loaded."
    warning "It may work after a reboot, or there may be a kernel compatibility issue."
else
    msg2 "Driver loaded successfully!"
fi

# 8. Install CLI Tool
msg2 "Installing CLI utility..."
cat > "/usr/local/bin/sc0710-cli" <<EOF
#!/bin/bash
# SC0710 Control Utility

# --- Configuration ---
CURRENT_VERSION="$DRV_VERSION"
VERSION_URL="$VERSION_URL"
DRV_NAME="$DRV_NAME"

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
    # Read current values - check both possible parameter names
    local dbg=0
    if [[ -f /sys/module/sc0710/parameters/sc0710_debug_mode ]]; then
        dbg=\$(cat /sys/module/sc0710/parameters/sc0710_debug_mode 2>/dev/null || echo 0)
    elif [[ -f /sys/module/sc0710/parameters/debug ]]; then
        dbg=\$(cat /sys/module/sc0710/parameters/debug 2>/dev/null || echo 0)
    fi
    local img=\$(cat /sys/module/sc0710/parameters/use_status_images 2>/dev/null || echo 1)
    local smode=0
    if [[ -f /sys/module/sc0710/parameters/scaler_mode ]]; then
        smode=\$(cat /sys/module/sc0710/parameters/scaler_mode 2>/dev/null || echo 0)
    fi
    
    # Write to modprobe config using the C variable name (what module_param exposes)
    echo "options sc0710 sc0710_debug_mode=\$dbg use_status_images=\$img scaler_mode=\$smode" > /etc/modprobe.d/sc0710-params.conf
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
    echo -e "\${BOLD}SC0710\${NC} Driver Control Utility v\${CURRENT_VERSION}"
    echo ""
    echo -e "\${BOLD}USAGE:\${NC}"
    echo -e "    sc0710-cli [OPTION]"
    echo ""
    echo -e "\${BOLD}OPTIONS:\${NC}"
    echo -e "    \${BOLD}-l, --load\${NC}       Load the driver module"
    echo -e "    \${BOLD}-u, --unload\${NC}     Unload the driver module"
    echo -e "    \${BOLD}--restart\${NC}        Restart the driver module"
    echo -e "    \${BOLD}-s, --status\${NC}     Show DKMS and module status"
    echo -e "    \${BOLD}-d, --debug\${NC}      Toggle debug mode on/off"
    echo -e "    \${BOLD}-it, --image-toggle\${NC} Toggle status images on/off"
    echo -e "    \${BOLD}-ss, --software-scaler\${NC} Toggle software scaler modes (MK.2 only)"
    echo -e "    \${BOLD}-U, --update\${NC}     Check for updates and reinstall"
    echo -e "    \${BOLD}-r, -R, --remove\${NC} Completely uninstall driver and CLI"
    echo -e "    \${BOLD}-v, --version\${NC}    Show version information"
    echo -e "    \${BOLD}-h, --help\${NC}       Show this help message"
    echo ""
}

# --- No Arguments Handler ---
if [[ \$# -eq 0 ]]; then
    echo -e "\${BOLD}SC0710\${NC} Driver Control Utility"
    echo -e "Use \${BOLD}-h\${NC} or \${BOLD}--help\${NC} for usage information."
    exit 0
fi

# --- Command Handler ---
case "\$1" in
    -l|--load)
        echo -e "\${BLUE}::\${NC} Loading driver..."
        modprobe \$DRV_NAME && echo -e "\${GREEN}[OK]\${NC} Driver loaded successfully."
        ;;
    -u|--unload)
        echo -e "\${BLUE}::\${NC} Unloading driver..."
        if ! rmmod \$DRV_NAME 2>/dev/null; then
            echo -e "\${YELLOW}[BUSY]\${NC} Standard unload failed. Attempting force..."
            fuser -k /dev/video* >/dev/null 2>&1 || true
            sleep 0.5
            if rmmod -f \$DRV_NAME 2>/dev/null; then
                echo -e "\${GREEN}[OK]\${NC} Driver force-unloaded successfully."
            else
                echo -e "\${RED}[ERROR]\${NC} Kernel refused to force unload. A reboot may be required."
            fi
        else
            echo -e "\${GREEN}[OK]\${NC} Driver unloaded successfully."
        fi
        ;;
    --restart)
        \$0 --unload
        sleep 1
        \$0 --load
        ;;
    -s|--status)
        check_version
        echo -e "\${BLUE}::\${NC} \${BOLD}DKMS Status\${NC}"
        dkms status \$DRV_NAME 2>/dev/null || echo "   DKMS not configured for this driver."
        echo ""
        echo -e "\${BLUE}::\${NC} \${BOLD}Kernel Module\${NC}"
        if lsmod | grep -q \$DRV_NAME; then
            echo -e "   \${GREEN}●\${NC} Module is loaded"
            MOD_INFO=\$(lsmod | grep \$DRV_NAME | head -1)
            MOD_SIZE=\$(echo "\$MOD_INFO" | awk '{print \$2}')
            MOD_USED=\$(echo "\$MOD_INFO" | awk '{print \$3}')
            echo "   Size: \$MOD_SIZE bytes, Reference count: \$MOD_USED"
            if [[ "\$MOD_USED" -gt 0 ]]; then
                # Find processes using video devices or ALSA sound devices
                PIDS=""
                for vdev in /dev/video*; do
                    if [[ -e "\$vdev" ]]; then
                        DEVPIDS=\$(fuser "\$vdev" 2>/dev/null | tr -s ' ')
                        if [[ -n "\$DEVPIDS" ]]; then
                            PIDS="\$PIDS \$DEVPIDS"
                        fi
                    fi
                done
                # Check all ALSA sound devices (control, PCM, timer, etc.)
                for sdev in /dev/snd/*; do
                    if [[ -e "\$sdev" ]]; then
                        DEVPIDS=\$(fuser "\$sdev" 2>/dev/null | tr -s ' ')
                        if [[ -n "\$DEVPIDS" ]]; then
                            PIDS="\$PIDS \$DEVPIDS"
                        fi
                    fi
                done
                # Deduplicate and resolve PID to process name
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
                    
                    # Try to parse exact board name from dmesg
                    BOARD_NAME=\$(dmesg 2>/dev/null | grep -E "sc0710.*subsystem: \${SUBVEN}:\${SUBDEV}.*board:" | tail -1 | sed 's/.*board: \([^\[]*\).*/\1/' | sed 's/ *$//')
                    
                    if [[ -z "\$BOARD_NAME" ]]; then
                        # Fallbacks
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
            # Parse the state proc file for signal info
            PROC_INFO=\$(cat /proc/sc0710-state 2>/dev/null)
            # Check for HDMI line - if it shows "no signal" or actual format
            HDMI_LINE=\$(echo "\$PROC_INFO" | grep "HDMI:" | head -1)
            if [[ -n "\$HDMI_LINE" ]]; then
                if echo "\$HDMI_LINE" | grep -q "no signal"; then
                    echo -e "   \${YELLOW}○\${NC} No signal detected"
                else
                    # Extract format info from HDMI line
                    # Format: "        HDMI: 1920x1080p60 -- 1920x1080p (2200x1125)"
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
                
                # Scaler Status (MK.2 only)
                SCALER_LINE=\$(echo "\$PROC_INFO" | grep "^      scaler:" | head -1)
                AUTO_SCALER_LINE=\$(echo "\$PROC_INFO" | grep "^ auto scaler:" | head -1)
                
                if [[ -n "\$SCALER_LINE" ]]; then
                    echo ""
                    echo -e "\${BLUE}::\${NC} \${BOLD}Scaler Information (MK.2)\${NC}"
                    SCALER_MODE=\$(echo "\$SCALER_LINE" | sed 's/.*scaler: \([^ ]*.*\)/\1/')
                    if [[ "\$SCALER_MODE" == "DISABLED" ]]; then
                        echo -e "   Software Scaler: \${YELLOW}DISABLED\${NC}"
                    else
                        echo -e "   Software Scaler: \${GREEN}\${SCALER_MODE}\${NC}"
                    fi

                    if [[ -n "\$AUTO_SCALER_LINE" ]]; then
                        AUTO_VAL=\$(echo "\$AUTO_SCALER_LINE" | sed 's/.*auto scaler: \(.*\)/\1/')
                        if echo "\$AUTO_VAL" | grep -q "ON"; then
                            echo -e "   Auto Scaler: \${GREEN}ON (Prevented Kernel Panic)\${NC}"
                            echo -e "     \${YELLOW}⚠ Please restart your streaming/recording software\${NC}"
                            echo -e "     \${YELLOW}  to restore low latency hardware capture.\${NC}"
                        else
                            echo -e "   Auto Scaler: \${BLUE}OFF\${NC}"
                        fi
                    fi
                fi
            else
                echo -e "   \${RED}○\${NC} Could not read HDMI status"
            fi
        else
            # Fallback to dmesg if proc not available
            LAST_FMT=\$(dmesg 2>/dev/null | grep -E "sc0710.*Detected timing|sc0710.*DTC created" | tail -1)
            if [[ -n "\$LAST_FMT" ]]; then
                # Clean up the message
                FMT_MSG=\$(echo "\$LAST_FMT" | sed 's/.*sc0710[^:]*: //')
                echo -e "   Last detected: \${FMT_MSG}"
            else
                echo -e "   \${RED}○\${NC} No signal info available (check dmesg)"
            fi
        fi
        echo ""
        # --- ECP5 Firmware Status (4K Pro only) ---
        # Check if any bound device is a 4K Pro (subsystem 1cfa:0012)
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
        # Fallback: check lspci if driver not loaded
        if [[ "\$IS_4KP" == "false" ]]; then
            if lspci -d ::0400 -nn 2>/dev/null | grep -qi "1cfa:0012"; then
                IS_4KP=true
            fi
        fi
        if [[ "\$IS_4KP" == "true" ]]; then
            echo -e "\${BLUE}::\${NC} \${BOLD}ECP5 Firmware\${NC}"
            # Check firmware file presence
            FW_FOUND=false
            FW_LOCATION=""
            if [[ -f "/lib/firmware/sc0710/SC0710.FWI.HEX" ]]; then
                FW_FOUND=true
                FW_LOCATION="/lib/firmware/sc0710/SC0710.FWI.HEX"
            elif [[ -f "/var/lib/sc0710/firmware/SC0710.FWI.HEX" ]]; then
                FW_FOUND=true
                FW_LOCATION="/var/lib/sc0710/firmware/SC0710.FWI.HEX"
            elif [[ -f "/etc/firmware/sc0710/SC0710.FWI.HEX" ]]; then
                FW_FOUND=true
                FW_LOCATION="/etc/firmware/sc0710/SC0710.FWI.HEX"
            fi
            if [[ "\$FW_FOUND" == "true" ]]; then
                echo -e "   \${GREEN}●\${NC} Firmware file present"
                echo -e "     Location: \${FW_LOCATION}"
            else
                echo -e "   \${RED}○\${NC} Firmware file missing"
                echo -e "     Run: \${BOLD}sudo bash /usr/local/libexec/sc0710-firmware.sh\${NC}"
            fi
            # Check ECP5 FPGA programming status from dmesg
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
            # Check firmware service status
            if systemctl is-enabled sc0710-firmware.service >/dev/null 2>&1; then
                echo -e "   \${GREEN}●\${NC} Firmware service enabled"
            else
                echo -e "   \${YELLOW}○\${NC} Firmware service not installed"
            fi
            echo ""
        fi
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
    -ss|--software-scaler)
        if [[ ! -f /sys/module/sc0710/parameters/scaler_mode ]]; then
            echo -e "\${RED}[ERROR]\${NC} Module not loaded. Load it first with: sc0710-cli --load"
            exit 1
        fi
        CURRENT=\$(cat /sys/module/sc0710/parameters/scaler_mode)
        if [[ "\$CURRENT" == "0" || -z "\$CURRENT" ]]; then
            echo 1 > /sys/module/sc0710/parameters/scaler_mode
            echo -e "\${GREEN}[OK]\${NC} Software Scaler enabled: Upscale Mode (to 4K)"
        elif [[ "\$CURRENT" == "1" ]]; then
            echo 2 > /sys/module/sc0710/parameters/scaler_mode
            echo -e "\${YELLOW}[OK]\${NC} Software Scaler changed: Downscale Mode (to 1080P)"
        else
            echo 0 > /sys/module/sc0710/parameters/scaler_mode
            echo -e "\${BLUE}[OK]\${NC} Software Scaler disabled"
        fi
        save_config
        ;;

    -U|--update)
        echo -e "\${BLUE}::\${NC} Checking for updates..."

        # Find the DKMS source directory
        DKMS_SRC=""
        for d in /usr/src/\${DRV_NAME}-*; do
            [[ -d "\$d" ]] && DKMS_SRC="\$d" && break
        done

        if [[ -z "\$DKMS_SRC" ]]; then
            echo -e "\${RED}[ERROR]\${NC} DKMS source not found. Re-run the full installer."
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
        if ! tar -xzf "\$TEMP_TAR" --strip-components=1 -C "\$DKMS_SRC"; then
            echo -e "\${RED}[ERROR]\${NC} Failed to extract update archive."
            rm -f "\$TEMP_TAR"
            exit 1
        fi
        rm -f "\$TEMP_TAR"

        # Read updated version
        NEW_VER="\$CURRENT_VERSION"
        if [[ -f "\$DKMS_SRC/version" ]]; then
            NEW_VER=\$(cat "\$DKMS_SRC/version" | tr -d '[:space:]')
        fi

        # Unload the current module
        if lsmod | grep -q \$DRV_NAME; then
            echo -e "\${BLUE}::\${NC} Unloading current module..."
            \$0 --unload
        fi

        # Rebuild via DKMS
        echo -e "\${BLUE}::\${NC} Rebuilding module via DKMS..."
        KVER=\$(uname -r)
        
        # Remove old DKMS build (handles anything starting with sc0710)
        for ver_item in \$(dkms status 2>/dev/null | awk -F'[:,]' '/^sc0710/ {print \$1}' | tr -d ' '); do
            if ! dkms remove "\$ver_item" --all >/dev/null 2>&1; then
                cleanup_path=\$(echo "\$ver_item" | tr ',' '/')
                rm -rf "/var/lib/dkms/\$cleanup_path" 2>/dev/null || true
            fi
        done
        rmdir "/var/lib/dkms/\${DRV_NAME}" 2>/dev/null || true
        if dkms install -m \$DRV_NAME -v "\$NEW_VER" -k "\$KVER" 2>&1; then
            echo -e "\${GREEN}[OK]\${NC} Module rebuilt successfully."
        else
            echo -e "\${RED}[ERROR]\${NC} DKMS rebuild failed."
            exit 1
        fi

        # Reload the module
        echo -e "\${BLUE}::\${NC} Loading updated module..."
        if modprobe \$DRV_NAME 2>/dev/null; then
            echo -e "\${GREEN}[OK]\${NC} Driver updated and loaded (v\${NEW_VER})."
        else
            echo -e "\${YELLOW}[WARNING]\${NC} Module rebuilt but failed to load. Try: sc0710-cli --load"
        fi
        ;;
    -r|-R|--remove)
        echo -e "\${BLUE}::\${NC} Uninstalling driver and utility..."
        for ver_item in \$(dkms status 2>/dev/null | awk -F'[:,]' '/^sc0710/ {print \$1}' | tr -d ' '); do
            if ! dkms remove "\$ver_item" --all >/dev/null 2>&1; then
                cleanup_path=\$(echo "\$ver_item" | tr ',' '/')
                rm -rf "/var/lib/dkms/\$cleanup_path" 2>/dev/null || true
            fi
        done
        rmdir "/var/lib/dkms/\${DRV_NAME}" 2>/dev/null || true
        rm -rf "/usr/src/\${DRV_NAME}-"*
        rm -f "/etc/modules-load.d/\${DRV_NAME}.conf"
        rm -f "/etc/modprobe.d/\${DRV_NAME}.conf"
        rm -f "/etc/modprobe.d/\${DRV_NAME}-params.conf"
        # Remove firmware service if installed
        systemctl stop sc0710-firmware.service 2>/dev/null || true
        systemctl disable sc0710-firmware.service 2>/dev/null || true
        rm -f /etc/systemd/system/sc0710-firmware.service
        rm -f /usr/local/libexec/sc0710-firmware.sh
        systemctl daemon-reload 2>/dev/null || true
        rm -f "/usr/local/bin/sc0710-cli"
        echo -e "\${GREEN}[OK]\${NC} Driver, firmware service, and CLI tool removed."
        ;;
    -v|--version)
        echo -e "\${BOLD}SC0710\${NC} Driver Control Utility"
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
log "=== Installation completed successfully ==="
echo ""
echo -e "${BOLD}${GREEN}::${NC} ${BOLD}Installation Complete.${NC}"
echo ""
echo -e " ${BLUE}->${NC} New command available: ${BOLD}sc0710-cli${NC}"
echo -e "    Usage:"
echo -e "      ${BOLD}sc0710-cli -s${NC}  or  ${BOLD}--status${NC}   Check driver health"
echo -e "      ${BOLD}sc0710-cli -l${NC}  or  ${BOLD}--load${NC}     Load driver"
echo -e "      ${BOLD}sc0710-cli -u${NC}  or  ${BOLD}--unload${NC}   Unload driver"
echo -e "      ${BOLD}sc0710-cli --restart${NC}        Reload driver"
echo -e "      ${BOLD}sc0710-cli -d${NC}  or  ${BOLD}--debug${NC}    Toggle debug output"
echo -e "      ${BOLD}sc0710-cli -it${NC} or  ${BOLD}--image-toggle${NC}  Toggle status images"
echo -e "      ${BOLD}sc0710-cli -ss${NC} or  ${BOLD}--software-scaler${NC} Toggle software scaler (MK.2)"
echo -e ""
echo -e "      ${BOLD}sc0710-cli -U${NC}  or  ${BOLD}--update${NC}   Pull latest & rebuild"
echo -e "      ${BOLD}sc0710-cli -r/R${NC} or ${BOLD}--remove${NC} Complete uninstall"
echo -e "      ${BOLD}sc0710-cli -h${NC}  or  ${BOLD}--help${NC}     Show all options"
echo ""
echo -e " ${BLUE}->${NC} Installation log available at: ${BOLD}$LOG_FILE${NC}"
echo ""

if [ "$VIDEO_GROUP_CHANGED" = true ]; then
    echo -e "${YELLOW}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${YELLOW}║                 IMPORTANT NOTICE                          ║${NC}"
    echo -e "${YELLOW}╠═══════════════════════════════════════════════════════════╣${NC}"
    echo -e "${YELLOW}║${NC}  User permissions have been updated.                      ${YELLOW}║${NC}"
    echo -e "${YELLOW}║${NC}  You ${BOLD}MUST REBOOT${NC} or ${BOLD}LOG OUT${NC} and back in for changes       ${YELLOW}║${NC}"
    echo -e "${YELLOW}║${NC}  to take effect. OBS will NOT detect the card otherwise.  ${YELLOW}║${NC}"
    echo -e "${YELLOW}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
fi
