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

# --- Safety & Strict Mode ---
set -euo pipefail
IFS=$'\n\t'

# --- Configuration ---
REPO_URL="https://github.com/Nakildias/sc0710.git"
VERSION_URL="https://raw.githubusercontent.com/Nakildias/sc0710/main/version"
DRV_NAME="sc0710"
DRV_VERSION="01.11.26-1"
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

# 1. Dependency Check
msg2 "Checking system dependencies..."

# 1. Detect Package Manager Strategy
PKG_MANAGER=""
OS_ID=""

# Try to read ID from file
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="$ID"
fi

# Logic: Check ID matches OR check if command exists
if [[ "$OS_ID" =~ ^(arch|manjaro|endeavouros)$ ]] || command -v pacman >/dev/null 2>&1; then
    PKG_MANAGER="pacman"
elif [[ "$OS_ID" =~ ^(debian|ubuntu|pop|linuxmint|kali|raspbian)$ ]] || command -v apt-get >/dev/null 2>&1; then
    PKG_MANAGER="apt"
elif [[ "$OS_ID" =~ ^(fedora|rhel|centos|almalinux)$ ]] || command -v dnf >/dev/null 2>&1; then
    PKG_MANAGER="dnf"
fi

# 2. Execute Installation based on Strategy
case "$PKG_MANAGER" in
    pacman)
        # Arch / Pacman based
        if ! pacman -Qi base-devel linux-headers git dkms >/dev/null 2>&1; then
            msg2 "Installing missing dependencies (pacman)..."
            # CHANGED: -S --needed (Prevents system breakage)
            pacman -S --needed --noconfirm base-devel linux-headers git dkms >/dev/null
        fi
        ;;
    apt)
        # Debian / Apt based
        msg2 "Installing dependencies (apt)..."
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq build-essential linux-headers-"$(uname -r)" git dkms >/dev/null
        ;;
    dnf)
        # RedHat / Dnf based
        msg2 "Installing dependencies (dnf)..."
        dnf install -y -q kernel-devel kernel-headers gcc make git dkms >/dev/null
        ;;
    *)
        warning "Could not detect a supported package manager (apt/pacman/dnf). Assuming dependencies are met."
        ;;
esac

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
            # Remove all versions found
            for ver in $(dkms status | grep "$DRV_NAME" | sed 's/.*\/\([^,]*\),.*/\1/'); do
                dkms remove -m "$DRV_NAME" -v "$ver" --all >/dev/null 2>&1 || true
            done
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

    # Optional: Add softdeps if needed for stability
    cat > "/etc/modprobe.d/${DRV_NAME}.conf" <<EOF
softdep $DRV_NAME pre: videodev videobuf2-v4l2 videobuf2-vmalloc
EOF
else
    msg2 "Autostart disabled. Load manually with 'modprobe $DRV_NAME'."
    rm -f "/etc/modules-load.d/${DRV_NAME}.conf"
fi

# 7. Final Load
msg2 "Loading module..."
modprobe "$DRV_NAME"

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

# --- Version Check Function ---
check_version() {
    local REMOTE_VERSION
    REMOTE_VERSION=\$(curl -fsSL "\$VERSION_URL" 2>/dev/null | tr -d '[:space:]')
    
    if [[ -n "\$REMOTE_VERSION" && "\$REMOTE_VERSION" != "\$CURRENT_VERSION" ]]; then
        echo ""
        echo -e "\${YELLOW}╔═══════════════════════════════════════════════════════════╗\${NC}"
        echo -e "\${YELLOW}║              UPDATE AVAILABLE                             ║\${NC}"
        echo -e "\${YELLOW}╠═══════════════════════════════════════════════════════════╣\${NC}"
        echo -e "\${YELLOW}║\${NC}  Current: \${RED}\${CURRENT_VERSION}\${NC}"
        printf "\${YELLOW}║\${NC}  Latest:  \${GREEN}%-47s\${NC}\n" "\$REMOTE_VERSION"
        echo -e "\${YELLOW}╠═══════════════════════════════════════════════════════════╣\${NC}"
        echo -e "\${YELLOW}║\${NC}  Run \${BOLD}sc0710-cli -U\${NC} or \${BOLD}sc0710-cli --update\${NC} to update"
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
    echo -e "    \${BOLD}-r, --restart\${NC}    Restart the driver module"
    echo -e "    \${BOLD}-s, --status\${NC}     Show DKMS and module status"
    echo -e "    \${BOLD}-U, --update\${NC}     Check for updates and reinstall"
    echo -e "    \${BOLD}-R, --remove\${NC}     Completely uninstall driver and CLI"
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
    -r|--restart)
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
            lsmod | grep \$DRV_NAME | awk '{print "   Size: " \$2 " bytes, Used by: " \$3 " processes"}'
        else
            echo -e "   \${RED}○\${NC} Module is not loaded"
        fi
        ;;
    -U|--update)
        echo -e "\${BLUE}::\${NC} Checking for updates..."
        echo -e "\${BLUE}::\${NC} Re-running installer from GitHub..."
        exec bash -c "\$(curl -fsSL https://raw.githubusercontent.com/Nakildias/sc0710/main/install-sc0710.sh)"
        ;;
    -R|--remove)
        echo -e "\${BLUE}::\${NC} Uninstalling driver and utility..."
        dkms remove -m \$DRV_NAME --all >/dev/null 2>&1 || true
        rm -f "/etc/modules-load.d/\${DRV_NAME}.conf"
        rm -f "/etc/modprobe.d/\${DRV_NAME}.conf"
        rm -f "/usr/local/bin/sc0710-cli"
        echo -e "\${GREEN}[OK]\${NC} Driver and CLI tool removed."
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
echo -e "      ${BOLD}sc0710-cli -r${NC}  or  ${BOLD}--restart${NC}  Reload driver"
echo -e "      ${BOLD}sc0710-cli -U${NC}  or  ${BOLD}--update${NC}   Pull latest & rebuild"
echo -e "      ${BOLD}sc0710-cli -R${NC}  or  ${BOLD}--remove${NC}   Complete uninstall"
echo -e "      ${BOLD}sc0710-cli -h${NC}  or  ${BOLD}--help${NC}     Show all options"
echo ""
echo -e " ${BLUE}->${NC} Installation log available at: ${BOLD}$LOG_FILE${NC}"
echo ""
