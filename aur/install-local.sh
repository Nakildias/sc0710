#!/usr/bin/env bash
# Build and install sc0710-dkms-git from this repository (local AUR test install).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKGNAME="sc0710-dkms-git"
BRANCH=""
BUILD_DIR=""
DO_INSTALL=1
DO_CLEAN=0
DO_VERIFY=0
DO_WORKING_TREE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build sc0710-dkms-git from the local git tree and install it with pacman.
Use this to test AUR packaging before pushing to GitHub or the AUR.

Options:
  -b, --branch BRANCH   Git branch to build (default: current branch)
  -c, --clean           Remove an existing ${PKGNAME} install first
  -n, --no-install      Build the package only (do not run pacman -U)
  -v, --verify          Run basic post-install checks
  -w, --working-tree    Overlay uncommitted scripts/ into the build (for local testing)
  -h, --help            Show this help

Examples:
  ./aur/install-local.sh
  ./aur/install-local.sh --working-tree --verify
  ./aur/install-local.sh --clean --verify
  ./aur/install-local.sh -b fix/aur-dkms-build-and-cli -n

Note: makepkg clones from git. Uncommitted script changes are not included unless
      you commit them first, or pass --working-tree to overlay your local files.
EOF
}

required_kernel_headers_pkg() {
    local kver
    kver="$(uname -r)"
    case "$kver" in
        *-zen*) printf '%s' 'linux-zen-headers' ;;
        *-lts*) printf '%s' 'linux-lts-headers' ;;
        *-hardened*) printf '%s' 'linux-hardened-headers' ;;
        *) printf '%s' 'linux-headers' ;;
    esac
}

msg() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

detect_card_name() {
    local fw_lib="/usr/lib/sc0710/sc0710-firmware-lib.sh"

    if [[ -f "$fw_lib" ]]; then
        # shellcheck source=/dev/null
        source "$fw_lib"
        sc0710_detect_card_name
        return $?
    fi
    return 1
}

print_next_steps() {
    local card_name="" fw_lib="/usr/lib/sc0710/sc0710-firmware-lib.sh"

    if card_name=$(detect_card_name); then
        printf '\nDetected card: %s\n\n' "$card_name"
    fi

    if [[ -f "$fw_lib" ]]; then
        # shellcheck source=/dev/null
        source "$fw_lib"
        if sc0710_is_4k_pro; then
            cat <<'EOF'
4K Pro — the driver programs the ECP5 FPGA at module load and refuses to
bind the card if it can't (missing firmware file, upload failure).

Next steps:
  sc0710-cli --status
  sudo sc0710-cli --restart    # if the card didn't bind (no /dev/video*)
  sudo sc0710-cli --remove     # uninstall via yay/paru
EOF
            return
        fi
    fi

    cat <<'EOF'
Next steps:
  sudo modprobe sc0710
  sc0710-cli --status
  sudo sc0710-cli --remove     # uninstall via yay/paru
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--branch)
            [[ $# -ge 2 ]] || die "missing value for $1"
            BRANCH="$2"
            shift 2
            ;;
        -c|--clean) DO_CLEAN=1; shift ;;
        -n|--no-install) DO_INSTALL=0; shift ;;
        -v|--verify) DO_VERIFY=1; shift ;;
        -w|--working-tree) DO_WORKING_TREE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

command -v git >/dev/null || die "git is required"
command -v makepkg >/dev/null || die "makepkg is required (install base-devel)"
command -v pacman >/dev/null || die "pacman is required"

git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || die "not a git repository: $REPO_ROOT"

[[ -n "$BRANCH" ]] || BRANCH="$(git -C "$REPO_ROOT" branch --show-current)"
[[ -n "$BRANCH" ]] || die "could not determine git branch (detached HEAD? use --branch)"

if ! git -C "$REPO_ROOT" diff-index --quiet HEAD --; then
    printf 'warning: uncommitted changes in %s will not be included in the package\n' "$REPO_ROOT"
    if [[ "$DO_WORKING_TREE" -eq 0 ]]; then
        printf '         commit your changes first, or rerun with --working-tree\n\n'
    else
        printf '         using --working-tree to overlay local scripts/\n\n'
    fi
fi

overlay_working_tree() {
    local src="$BUILD_DIR/src/sc0710"
    [[ -d "$src/scripts" ]] || die "expected source tree at $src/scripts (run makepkg -od first)"

    msg "Overlaying local scripts from ${REPO_ROOT}/scripts/"
    for f in sc0710-firmware-lib.sh sc0710-cli.sh extract-firmware.sh; do
        [[ -f "$REPO_ROOT/scripts/$f" ]] || die "missing $REPO_ROOT/scripts/$f"
        cp "$REPO_ROOT/scripts/$f" "$src/scripts/$f"
    done
}

HEADERS_PKG="$(required_kernel_headers_pkg)"
MISSING_PKGS=()
pacman -Q base-devel >/dev/null 2>&1 || MISSING_PKGS+=(base-devel)
pacman -Q dkms >/dev/null 2>&1 || MISSING_PKGS+=(dkms)
pacman -Q git >/dev/null 2>&1 || MISSING_PKGS+=(git)
pacman -Q "$HEADERS_PKG" >/dev/null 2>&1 || MISSING_PKGS+=("$HEADERS_PKG")

if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
    msg "Installing missing dependencies: ${MISSING_PKGS[*]}"
    sudo pacman -S --needed --noconfirm "${MISSING_PKGS[@]}"
fi

KVER="$(uname -r)"
if [[ ! -d "/lib/modules/${KVER}/build" ]]; then
    die "kernel headers for ${KVER} are missing (install ${HEADERS_PKG})"
fi

if [[ "$DO_CLEAN" -eq 1 ]] && pacman -Q "$PKGNAME" >/dev/null 2>&1; then
    msg "Removing existing ${PKGNAME} install"
    if command -v yay >/dev/null 2>&1; then
        yay -R "$PKGNAME"
    elif command -v paru >/dev/null 2>&1; then
        paru -R "$PKGNAME"
    else
        sudo pacman -R "$PKGNAME"
    fi
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sc0710-aur-local.XXXXXX")"
cleanup() { rm -rf "$BUILD_DIR"; }
trap cleanup EXIT

msg "Preparing local build in ${BUILD_DIR}"
cp "$SCRIPT_DIR/PKGBUILD" "$SCRIPT_DIR/sc0710.install" "$BUILD_DIR/"

LOCAL_SOURCE="git+file://${REPO_ROOT}#branch=${BRANCH}"
sed -i "s|^source=(\"git+https://github.com/Nakildias/sc0710.git\")|source=(\"${LOCAL_SOURCE}\")|" \
    "$BUILD_DIR/PKGBUILD"

msg "Building branch ${BRANCH} from ${REPO_ROOT}"
cd "$BUILD_DIR"

if [[ "$DO_WORKING_TREE" -eq 1 ]]; then
    makepkg -od --noconfirm
    overlay_working_tree
    if [[ "$DO_INSTALL" -eq 1 ]]; then
        makepkg -sei --noconfirm
    else
        makepkg -se --noconfirm
    fi
elif [[ "$DO_INSTALL" -eq 1 ]]; then
    makepkg -si --noconfirm
else
    makepkg -s --noconfirm
fi

if [[ "$DO_INSTALL" -eq 0 ]]; then
    # The EXIT trap removes BUILD_DIR so move it
    pkgfile=("$BUILD_DIR/${PKGNAME}"-*.pkg.tar.zst)
    cp "${pkgfile[@]}" "$SCRIPT_DIR/"
    msg "Package built: ${SCRIPT_DIR}/$(basename "${pkgfile[0]}")"
fi

if [[ "$DO_VERIFY" -eq 1 ]]; then
  msg "Running post-install checks"
  if [[ "$DO_INSTALL" -eq 1 ]]; then
      pacman -Q "$PKGNAME"
      command -v sc0710-cli
      ls -l /usr/bin/sc0710-cli /usr/lib/sc0710/ 2>/dev/null || true
      dkms status sc0710 2>/dev/null || true
      sc0710-cli -s || sudo sc0710-cli -s
      if source /usr/lib/sc0710/sc0710-firmware-lib.sh 2>/dev/null && sc0710_is_4k_pro; then
          sc0710_card_bound && msg "Card bound — ECP5 FPGA programmed" || msg "Card not bound — check dmesg (sudo dmesg | grep sc0710)"
      fi
  else
      msg "Skipping runtime checks (--no-install)"
  fi
fi

msg "Done"
if [[ "$DO_INSTALL" -eq 1 ]]; then
    print_next_steps
fi
