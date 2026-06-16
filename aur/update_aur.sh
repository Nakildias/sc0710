#!/bin/bash

# This script is used only by me (nakildias) to automate the AUR update process.

# Exit immediately if a command exits with a non-zero status
set -e

# Define variables
AUR_REPO_URL="ssh://aur@aur.archlinux.org/sc0710-dkms-git.git"
MAIN_REPO_URL="https://github.com/Nakildias/sc0710.git"
WORK_DIR="/tmp/sc0710_deploy"
MAIN_DIR="$WORK_DIR/main"
AUR_DIR="$WORK_DIR/aur"

# Set up a trap to ensure cleanup happens no matter what
cleanup() {
    echo "Cleaning up workspace and SSH agent..."
    rm -rf "$WORK_DIR"
    eval $(ssh-agent -k) > /dev/null 2>&1
}
trap cleanup EXIT

echo "Starting deployment process..."

# 1. Start the SSH agent and add key
eval $(ssh-agent -s) > /dev/null 2>&1
ssh-add ~/.ssh/aur_nakildias

# 2. Create fresh working directories
mkdir -p "$WORK_DIR"

# 3. Clone the main repository to get the latest PKGBUILD
echo "Cloning main repository..."
git clone "$MAIN_REPO_URL" "$MAIN_DIR"

# 4. Clone the AUR repository
echo "Cloning AUR repository..."
git clone "$AUR_REPO_URL" "$AUR_DIR"

# 5. Copy packaging files from main to AUR
echo "Transferring PKGBUILD and install hook..."
cp "$MAIN_DIR/aur/PKGBUILD" "$MAIN_DIR/aur/sc0710.install" "$AUR_DIR/"

# 6. Update the PKGBUILD version using makepkg (The Arch Linux standard)
echo "Updating pkgver..."
cd "$AUR_DIR"
# The -o flag downloads sources, -d skips dependency checks.
# This forces the execution of the pkgver() function to update the PKGBUILD.
makepkg -od

# 7. Generate a fresh .SRCINFO based on the updated PKGBUILD
echo "Generating .SRCINFO..."
makepkg --printsrcinfo > .SRCINFO

# 8. Extract the correct newly generated version for the commit message
NEW_VERSION=$(grep -m1 '^	pkgver =' .SRCINFO | awk '{print $3}')
NEW_REL=$(grep -m1 '^	pkgrel =' .SRCINFO | awk '{print $3}')
FULL_VERSION="${NEW_VERSION}-${NEW_REL}"

# 9. Commit and push the updates directly to AUR
echo "Pushing to AUR..."
git add PKGBUILD sc0710.install .SRCINFO

# Check if there are actually changes to commit before attempting
if git diff-index --quiet HEAD --; then
    echo "No changes to commit. The AUR is already up to date with version $FULL_VERSION."
else
    git commit -m "chore: update version to $FULL_VERSION"
    git push origin master
    echo "Success: AUR package updated to $FULL_VERSION."
fi
