#!/bin/bash
#
# SC0710 Driver Installer
#
# Usage: ./install.sh [--force] [--noconfirm]

# --- Safety & Strict Mode ---
set -euo pipefail
IFS=$'\n\t'

# --- Configuration ---
REPO_URL="https://github.com/Nakildias/sc0710.git"
DRV_NAME="sc0710"
DRV_VERSION="1.0.2"
SRC_DEST="/usr/src/${DRV_NAME}-${DRV_VERSION}"

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

# --- Helper Functions ---

msg() {
    printf "${BLUE}::${NC} ${BOLD}%s${NC}\n" "$1"
}

msg2() {
    printf " ${BLUE}->${NC} ${BOLD}%s${NC}\n" "$1"
}

warning() {
    printf "${YELLOW}warning:${NC} %s\n" "$1"
}

error() {
    printf "${RED}error:${NC} %s\n" "$1"
}

die() {
    error "$1"
    exit 1
}

cleanup() {
    if [[ -d "$TEMP_DIR" ]]; then
        rm -rf "$TEMP_DIR"
    fi
}
trap cleanup EXIT INT TERM

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

check_root
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
        echo -e "${RED}rmmod: ERROR: Module sc0710 is in use${NC}"

        # ASK USER TO FORCE
        if confirm "Force unload module (Kill processes)? " "N"; then
            msg2 "Attempting forced removal..."

            # Try 1: Kill processes accessing video devices (common culprit)
            # This is a guess, but often accurate for capture cards
            fuser -k /dev/video* >/dev/null 2>&1 || true
            sleep 1

            # Try 2: Force flag
            if ! rmmod -f "$DRV_NAME" 2>/dev/null; then
                error "Force unload failed. Please close OBS/Zoom and try again."
                exit 1
            else
                msg2 "Module unloaded successfully."
            fi
        else
            die "Cannot proceed while module is in use."
        fi
    else
        msg2 "Module unloaded."
    fi
fi

# 3. Source Setup
if [ -d "$SRC_DEST" ]; then rm -rf "$SRC_DEST"; fi
mkdir -p "$SRC_DEST"

msg2 "Downloading source..."
TEMP_DIR=$(mktemp -d)
if ! git clone --depth 1 "$REPO_URL" "$TEMP_DIR" >/dev/null 2>&1; then
    die "Git clone failed."
fi
cp -r "$TEMP_DIR"/* "$SRC_DEST/"

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

    # Clean old DKMS
    if dkms status | grep -q "$DRV_NAME"; then
        dkms remove -m "$DRV_NAME" -v "$DRV_VERSION" --all >/dev/null 2>&1 || true
    fi

    msg2 "Building and Installing via DKMS..."
    dkms add -m "$DRV_NAME" -v "$DRV_VERSION" >/dev/null
    if ! dkms build -m "$DRV_NAME" -v "$DRV_VERSION" >/dev/null; then
        die "DKMS Build failed. Check make.log."
    fi
    dkms install -m "$DRV_NAME" -v "$DRV_VERSION" --force >/dev/null

else
    # --- MANUAL PATH ---
    msg2 "Compiling driver manually..."
    cd "$SRC_DEST"
    make >/dev/null

    msg2 "Installing .ko file..."
    # Usually make install works, but let's be explicit to be safe
    INSTALL_MOD_PATH="/lib/modules/$(uname -r)/kernel/drivers/media/pci/"
    mkdir -p "$INSTALL_MOD_PATH"
    cp "${DRV_NAME}.ko" "$INSTALL_MOD_PATH"
    depmod -a
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

# Colors for CLI output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

# Ensure the script is run as root
if [[ \$EUID -ne 0 ]]; then
   echo -e "\${RED}[ERROR] Please run with sudo (sudo sc0710-cli \$1)\${NC}"
   exit 1
fi

case "\$1" in
    start)
        echo "Loading driver..."
        modprobe $DRV_NAME && echo -e "\${GREEN}[OK] Driver started.\${NC}"
        ;;
    stop)
        echo "Stopping driver..."
        if ! rmmod $DRV_NAME 2>/dev/null; then
            echo -e "\${YELLOW}[BUSY] Standard stop failed. Attempting Hard Force...\${NC}"
            fuser -k /dev/video* >/dev/null 2>&1 || true
            sleep 0.5
            if rmmod -f $DRV_NAME 2>/dev/null; then
                echo -e "\${GREEN}[OK] Driver force-stopped successfully.\${NC}"
            else
                echo -e "\${RED}[ERROR] Kernel refused to force unload. A reboot may be required.\${NC}"
            fi
        else
            echo -e "\${GREEN}[OK] Driver stopped.\${NC}"
        fi
        ;;
    restart)
        \$0 stop
        sleep 1
        \$0 start
        ;;
    status)
        echo -e "--- \${GREEN}DKMS Status\${NC} ---"
        dkms status $DRV_NAME
        echo ""
        echo -e "--- \${GREEN}Kernel Module\${NC} ---"
        lsmod | grep $DRV_NAME || echo "Module not currently loaded in kernel."
        ;;
    update)
        echo "Checking for updates..."
        # 1. Clear old DKMS build
        dkms remove -m $DRV_NAME -v $DRV_VERSION --all >/dev/null 2>&1 || true

        # 2. Pull fresh source
        echo "Downloading latest source from GitHub..."
        TEMP=\$(mktemp -d)
        if git clone --depth 1 $REPO_URL "\$TEMP"; then
            rm -rf "$SRC_DEST"/*
            cp -r "\$TEMP"/* "$SRC_DEST/"
            rm -rf "\$TEMP"

            # 3. RE-GENERATE DKMS.CONF (The missing piece)
            cat > "$SRC_DEST/dkms.conf" <<EOD
PACKAGE_NAME="$DRV_NAME"
PACKAGE_VERSION="$DRV_VERSION"
BUILT_MODULE_NAME[0]="$DRV_NAME"
DEST_MODULE_LOCATION[0]="/kernel/drivers/media/pci/"
AUTOINSTALL="yes"
MAKE[0]="make KVERSION=\\\$kernelver"
EOD

            # 4. Rebuild
            echo "Rebuilding and installing driver..."
            if dkms install -m $DRV_NAME -v $DRV_VERSION --force; then
                \$0 restart
                echo -e "\${GREEN}[SUCCESS] Update complete.\${NC}"
            else
                echo -e "\${RED}[ERROR] DKMS build failed.\${NC}"
            fi
        else
            echo -e "\${RED}[ERROR] Download failed. Update aborted.\${NC}"
            rm -rf "\$TEMP"
        fi
        ;;
    remove)
        echo "Uninstalling driver and utility..."
        dkms remove -m $DRV_NAME -v $DRV_VERSION --all >/dev/null 2>&1 || true
        rm -f "/etc/modules-load.d/${DRV_NAME}.conf"
        rm -f "/etc/modprobe.d/${DRV_NAME}.conf"
        rm -f "/usr/local/bin/sc0710-cli"
        echo -e "\${GREEN}[OK] Driver and CLI tool removed.\${NC}"
        ;;
    *)
        echo "Usage: sc0710-cli {start|stop|restart|status|update|remove}"
        exit 1
        ;;
esac
EOF
chmod +x /usr/local/bin/sc0710-cli

# --- Final Success Message ---
echo ""
echo -e "${BOLD}${GREEN}::${NC} ${BOLD}Installation Complete.${NC}"
echo ""
echo -e " ${BLUE}->${NC} New command available: ${BOLD}sc0710-cli${NC}"
echo -e "    Usage:"
echo -e "      ${BOLD}sc0710-cli status${NC}  - Check driver health"
echo -e "      ${BOLD}sc0710-cli start${NC}   - Load driver"
echo -e "      ${BOLD}sc0710-cli stop${NC}    - Unload driver (with Force option)"
echo -e "      ${BOLD}sc0710-cli restart${NC} - Reload driver"
echo -e "      ${BOLD}sc0710-cli update${NC}  - Pull latest code & rebuild"
echo -e "      ${BOLD}sc0710-cli remove${NC}  - Complete uninstall"
echo ""
