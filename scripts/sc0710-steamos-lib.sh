#!/usr/bin/env bash
# Copyright (C) 2025-2026 Nakildias <nakildiaspro@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared sc0710 helpers for SteamOS (and SteamOS-derived images).
#
# SteamOS is immutable like Bazzite, but immutable in a different way:
#
#   * "/" is a read-only btrfs subvolume, unlocked with `steamos-readonly
#     disable`. /etc and /usr both live on it, so *every* write to /etc,
#     /usr/local or /lib/modules needs the unlock first.
#   * Updates are A/B: the whole rootfs partition is replaced, so anything
#     written under / (pacman packages, kernel headers, /usr/local/bin, and
#     sometimes even units in /etc/systemd/system) is gone after an update.
#   * /var is a 256 MB A/B partition that is rsynced onto the new side — too
#     small to hold a driver build tree, and not a persistence guarantee.
#   * /home is a single shared partition that is never touched by an update.
#
# So the persistent driver tree lives in /home/sc0710, and /var/lib/sc0710 is
# a symlink to it — that keeps every existing /var/lib/sc0710 path in the
# driver's scripts working unchanged. The boot service re-creates the symlink,
# re-installs the kernel headers and re-installs the CLI on every boot when an
# OS update has wiped them.
#
# Sourced by install-sc0710.sh, build-and-load.sh and sc0710-cli.sh — do not
# execute directly.

[[ -n "${SC0710_STEAMOS_LIB_LOADED:-}" ]] && return 0
SC0710_STEAMOS_LIB_LOADED=1

# Persistent tree (survives SteamOS A/B updates) and the compatibility symlink
# every other sc0710 script already points at.
SC0710_STEAMOS_HOME="${SC0710_STEAMOS_HOME:-/home/sc0710}"
SC0710_STEAMOS_COMPAT_LINK="${SC0710_STEAMOS_COMPAT_LINK:-/var/lib/sc0710}"

# Original rootfs lock state, remembered on the first unlock of this process.
SC0710_STEAMOS_ORIG_LOCKED=""

sc0710_steamos_log() {
    if [[ -n "${SC0710_STEAMOS_LOG_FILE:-}" ]]; then
        echo "$*" >> "$SC0710_STEAMOS_LOG_FILE"
    fi
    echo "$*"
}

# --- Detection -------------------------------------------------------------

# True on SteamOS 3 and SteamOS-derived images (HoloISO, ChimeraOS-style
# builds) — anything with Valve's read-only-rootfs tooling.
sc0710_is_steamos() {
    [[ "${SC0710_FORCE_STEAMOS:-0}" == "1" ]] && return 0

    local id id_like variant
    if [[ -r /etc/os-release ]]; then
        # Subshell: never clobber a caller's ID/ID_LIKE/VARIANT_ID.
        id=$(. /etc/os-release 2>/dev/null; printf '%s' "${ID:-}")
        id_like=$(. /etc/os-release 2>/dev/null; printf '%s' "${ID_LIKE:-}")
        variant=$(. /etc/os-release 2>/dev/null; printf '%s' "${VARIANT_ID:-}")
        [[ "$id" == "steamos" ]] && return 0
        [[ "$id_like" == *steamos* ]] && return 0
        [[ "$variant" == "steamdeck" ]] && return 0
    fi

    command -v steamos-readonly >/dev/null 2>&1 && return 0
    return 1
}

# Pretty name for messages: "SteamOS 3.8.16" / "SteamOS".
sc0710_steamos_name() {
    local name version
    if [[ -r /etc/os-release ]]; then
        name=$(. /etc/os-release 2>/dev/null; printf '%s' "${NAME:-}")
        version=$(. /etc/os-release 2>/dev/null; printf '%s' "${VERSION_ID:-}")
    fi
    [[ -z "$name" ]] && name="SteamOS"
    if [[ -n "$version" ]]; then
        printf '%s %s' "$name" "$version"
    else
        printf '%s' "$name"
    fi
}

# --- Read-only rootfs ------------------------------------------------------

# 0 = rootfs is read-only (locked), 1 = writable.
sc0710_steamos_rootfs_locked() {
    local prop probe

    if command -v btrfs >/dev/null 2>&1; then
        prop=$(btrfs property get -ts / ro 2>/dev/null || true)
        [[ "$prop" == *"ro=true"* ]] && return 0
        [[ "$prop" == *"ro=false"* ]] && return 1
    fi

    # Not btrfs (or btrfs-progs unavailable): probe /usr directly. /etc can be
    # a separate mount on some images, /usr never is.
    probe="/usr/.sc0710-write-test.$$"
    if (: > "$probe") 2>/dev/null; then
        rm -f "$probe" 2>/dev/null || true
        return 1
    fi
    return 0
}

# Make / writable. Remembers the state seen on the first call so
# sc0710_steamos_relock() can put the system back exactly as it was.
sc0710_steamos_unlock() {
    if [[ -z "$SC0710_STEAMOS_ORIG_LOCKED" ]]; then
        if sc0710_steamos_rootfs_locked; then
            SC0710_STEAMOS_ORIG_LOCKED=1
        else
            SC0710_STEAMOS_ORIG_LOCKED=0
        fi
    fi

    sc0710_steamos_rootfs_locked || return 0

    if command -v steamos-readonly >/dev/null 2>&1; then
        steamos-readonly disable >/dev/null 2>&1 || true
    fi
    if sc0710_steamos_rootfs_locked && command -v btrfs >/dev/null 2>&1; then
        btrfs property set -ts / ro false >/dev/null 2>&1 || true
    fi

    if sc0710_steamos_rootfs_locked; then
        sc0710_steamos_log "ERROR: could not unlock the read-only rootfs (steamos-readonly disable failed)."
        return 1
    fi
    return 0
}

# Restore the lock state from before the first sc0710_steamos_unlock().
# Never re-locks a system the user had already unlocked themselves.
sc0710_steamos_relock() {
    [[ "$SC0710_STEAMOS_ORIG_LOCKED" == "1" ]] || return 0
    sc0710_steamos_rootfs_locked && return 0

    sync 2>/dev/null || true
    if command -v steamos-readonly >/dev/null 2>&1; then
        steamos-readonly enable >/dev/null 2>&1 || true
    fi
    if ! sc0710_steamos_rootfs_locked && command -v btrfs >/dev/null 2>&1; then
        btrfs property set -ts / ro true >/dev/null 2>&1 || true
    fi
    sc0710_steamos_rootfs_locked || \
        sc0710_steamos_log "WARNING: rootfs left writable — re-lock it with: sudo steamos-readonly enable"
    return 0
}

# --- Persistent source tree ------------------------------------------------

# Create /home/sc0710 and point /var/lib/sc0710 at it. Safe to re-run; it is
# how the layout heals itself after an update reset /var.
sc0710_steamos_ensure_layout() {
    local link="$SC0710_STEAMOS_COMPAT_LINK" target="$SC0710_STEAMOS_HOME"

    mkdir -p "$target" || return 1

    if [[ -L "$link" ]]; then
        [[ "$(readlink -f "$link")" == "$target" ]] && return 0
        rm -f "$link"
    elif [[ -d "$link" ]]; then
        # A pre-existing real directory (e.g. an older install, or the atomic
        # layout): move its contents into the persistent tree, then replace it.
        cp -a "$link/." "$target/" 2>/dev/null || true
        rm -rf "$link"
    elif [[ -e "$link" ]]; then
        rm -f "$link"
    fi

    mkdir -p "$(dirname "$link")" || return 1
    ln -sfn "$target" "$link" || return 1
    return 0
}

# Warn when /home is not its own filesystem: the tree would then be wiped by
# the next OS update just like everything else under /.
sc0710_steamos_home_is_persistent() {
    local home_src root_src
    home_src=$(findmnt -no SOURCE --target "$SC0710_STEAMOS_HOME" 2>/dev/null || true)
    root_src=$(findmnt -no SOURCE --target / 2>/dev/null || true)
    [[ -n "$home_src" && -n "$root_src" && "$home_src" != "$root_src" ]]
}

# --- Packages --------------------------------------------------------------

# Headers package matching the running Neptune kernel, e.g.
#   6.16.12-valve24.4-1-neptune-616-gfe14... -> linux-neptune-616-headers
sc0710_steamos_headers_pkg() {
    local kver="${1:-$(uname -r)}" num
    num=$(printf '%s' "$kver" | sed -n 's/.*neptune-\([0-9][0-9]*\).*/\1/p')
    if [[ -n "$num" ]]; then
        printf 'linux-neptune-%s-headers' "$num"
        return 0
    fi
    printf 'linux-headers'
    return 0
}

sc0710_steamos_have_headers() {
    [[ -d "/lib/modules/${1:-$(uname -r)}/build" ]]
}

# SteamOS ships an empty keyring on a fresh image; without this pacman rejects
# every package as "required key missing from keyring".
sc0710_steamos_init_keyring() {
    command -v pacman-key >/dev/null 2>&1 || return 0

    if [[ ! -s /etc/pacman.d/gnupg/trustdb.gpg ]]; then
        sc0710_steamos_log "Initialising the pacman keyring..."
        pacman-key --init >/dev/null 2>&1 || true
    fi
    pacman-key --populate archlinux >/dev/null 2>&1 || true
    pacman-key --populate holo >/dev/null 2>&1 || true
    return 0
}

# pacman with a package cache on /home: /var is a 256 MB partition on SteamOS
# and a kernel-headers download can fill it.
sc0710_steamos_pacman() {
    local cachedir="${SC0710_STEAMOS_HOME}/pkgcache"
    mkdir -p "$cachedir" 2>/dev/null || cachedir="/var/cache/pacman/pkg"
    pacman --cachedir "$cachedir" "$@"
}

sc0710_steamos_clean_pkgcache() {
    rm -rf "${SC0710_STEAMOS_HOME}/pkgcache" 2>/dev/null || true
    return 0
}

# Install compiler + git if the image (or an update) left us without them.
# Caller must have unlocked the rootfs.
# gcc from base-devel is present on a fresh SteamOS image, but the userspace
# C headers that normally ship inside glibc (stdio.h, glob.h, ...) and
# linux-api-headers (asm/*.h, e.g. sys/ioctl.h's asm/ioctls.h) are stripped
# from Valve's base squashfs to save space — so gcc runs but #include fails.
# This is separate from the kernel headers sc0710_steamos_ensure_headers
# installs (those are versioned per-kernel; these are the generic userspace
# ones every C program compiles against).
sc0710_steamos_have_c_headers() {
    [[ -f /usr/include/stdio.h && -f /usr/include/asm/ioctls.h ]]
}

sc0710_steamos_ensure_build_tools() {
    local missing=()

    command -v gcc >/dev/null 2>&1 || missing+=("gcc")
    command -v make >/dev/null 2>&1 || missing+=("make")
    command -v git >/dev/null 2>&1 || missing+=("git")
    sc0710_steamos_have_c_headers || missing+=("glibc" "linux-api-headers")
    [[ ${#missing[@]} -eq 0 ]] && return 0

    sc0710_steamos_log "Installing build tools: ${missing[*]}"
    sc0710_steamos_init_keyring
    if ! sc0710_steamos_pacman -Sy --needed --noconfirm base-devel git >/dev/null 2>&1; then
        # base-devel is a group and can fail on a partially-synced image; try
        # the individual packages before giving up.
        sc0710_steamos_pacman -Sy --needed --noconfirm "${missing[@]}" >/dev/null 2>&1 || true
    fi
    # SteamOS strips /usr/include from the base image after glibc and
    # linux-api-headers are already marked installed in the pacman DB, so
    # --needed (used above) sees them as satisfied and skips them. Drop
    # --needed here to force pacman to actually re-extract the packages.
    if ! sc0710_steamos_have_c_headers; then
        sc0710_steamos_pacman -S --noconfirm glibc linux-api-headers >/dev/null 2>&1 || true
    fi
    sc0710_steamos_clean_pkgcache

    missing=()
    command -v gcc >/dev/null 2>&1 || missing+=("gcc")
    command -v make >/dev/null 2>&1 || missing+=("make")
    sc0710_steamos_have_c_headers || missing+=("glibc/linux-api-headers")
    if [[ ${#missing[@]} -gt 0 ]]; then
        sc0710_steamos_log "ERROR: build tools still missing: ${missing[*]}"
        return 1
    fi
    return 0
}

# Install the Neptune headers for the running kernel. Caller must have
# unlocked the rootfs. Returns 1 with an explanation when they cannot be had.
sc0710_steamos_ensure_headers() {
    local kver="${1:-$(uname -r)}" pkg

    sc0710_steamos_have_headers "$kver" && return 0

    pkg="$(sc0710_steamos_headers_pkg "$kver")"
    sc0710_steamos_log "Kernel headers for $kver are missing — installing $pkg"

    sc0710_steamos_init_keyring
    if ! sc0710_steamos_pacman -Sy --needed --noconfirm "$pkg" >/dev/null 2>&1; then
        sc0710_steamos_log "WARNING: could not install $pkg; trying linux-neptune-headers"
        sc0710_steamos_pacman -Sy --needed --noconfirm linux-neptune-headers >/dev/null 2>&1 || true
    fi
    sc0710_steamos_clean_pkgcache

    if sc0710_steamos_have_headers "$kver"; then
        sc0710_steamos_log "Kernel headers installed for $kver"
        return 0
    fi

    sc0710_steamos_log "ERROR: no kernel headers for $kver after installing $pkg."
    sc0710_steamos_log "       If SteamOS was just updated, reboot into the new kernel and retry."
    sc0710_steamos_log "       Manual fix: sudo steamos-readonly disable && sudo pacman -Sy $pkg"
    return 1
}

# Bounded wait for name resolution — the boot service may run before
# NetworkManager has a connection, and pacman needs one after an OS update.
sc0710_steamos_wait_online() {
    local timeout="${1:-90}" waited=0

    if command -v nm-online >/dev/null 2>&1; then
        nm-online -s -q --timeout="$timeout" >/dev/null 2>&1 && return 0
    fi

    while (( waited < timeout )); do
        if getent hosts steamdeck-packages.steamos.cloud >/dev/null 2>&1 || \
           getent hosts archlinux.org >/dev/null 2>&1; then
            return 0
        fi
        sleep 3
        waited=$(( waited + 3 ))
    done
    return 1
}

# --- Userspace tools -------------------------------------------------------

# Re-install the CLI and GUIs into /usr/local/bin from the persistent tree.
# An A/B update empties /usr/local, so the boot service calls this to heal it.
# Mirrors the "Install CLI Utility" step of install-sc0710.sh — keep both in
# sync when a new tool is added.
sc0710_steamos_install_tools() {
    local src="${1:-$SC0710_STEAMOS_HOME}"
    local scripts="$src/scripts"

    [[ -d "$scripts" ]] || return 1
    mkdir -p /usr/local/bin || return 1

    local tool
    if [[ -f "$scripts/sc0710-cli.sh" ]]; then
        install -m755 "$scripts/sc0710-cli.sh" /usr/local/bin/sc0710-cli || return 1
    fi
    for tool in sc0710-edid-config sc0710-hdr-config sc0710-gui; do
        if [[ -f "$scripts/$tool" ]]; then
            install -m755 "$scripts/$tool" "/usr/local/bin/$tool" || true
        fi
    done

    if [[ -f "$scripts/mk2-set-edid.c" && ! -x /usr/local/bin/mk2-set-edid ]]; then
        cc -O2 -o /usr/local/bin/mk2-set-edid "$scripts/mk2-set-edid.c" 2>/dev/null || \
        gcc -O2 -o /usr/local/bin/mk2-set-edid "$scripts/mk2-set-edid.c" 2>/dev/null || true
    fi
    if [[ -f "$scripts/mk2-set-tonemap.c" && ! -x /usr/local/bin/mk2-set-tonemap ]]; then
        cc -O2 -o /usr/local/bin/mk2-set-tonemap "$scripts/mk2-set-tonemap.c" -lm 2>/dev/null || \
        gcc -O2 -o /usr/local/bin/mk2-set-tonemap "$scripts/mk2-set-tonemap.c" -lm 2>/dev/null || true
    fi

    [[ -x /usr/local/bin/sc0710-cli ]]
}

sc0710_steamos_tools_missing() {
    [[ ! -x /usr/local/bin/sc0710-cli ]]
}
