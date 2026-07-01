#!/bin/bash
# Shared DKMS helpers for sc0710 packaging, CLI, and installer scripts.

SC0710_DRV_NAME="${SC0710_DRV_NAME:-sc0710}"

# Convert version file format (YYYY.MM.DD-REV) to DKMS format (YYYY.MM.DD.REV).
sc0710_version_to_dkms() {
    local ver="${1//[[:space:]]/}"
    if [[ "$ver" == *-* ]]; then
        printf '%s.%s' "${ver%-*}" "${ver##*-}"
    else
        printf '%s' "$ver"
    fi
}

sc0710_dkms_cleanup() {
    local ver_item drv="${SC0710_DRV_NAME}"

    if lsmod | grep -q "^${drv} "; then
        rmmod "$drv" 2>/dev/null || true
    fi

    for ver_item in $(dkms status 2>/dev/null | awk -F'[:,]' "/^${drv}/ {print \$1}" | tr -d ' '); do
        dkms remove "$ver_item" --all >/dev/null 2>&1 || \
            rm -rf "/var/lib/dkms/$(echo "$ver_item" | tr ',' '/')" 2>/dev/null
    done
    rmdir "/var/lib/dkms/${drv}" 2>/dev/null || true
    rm -rf "/usr/src/${drv}-"*

    find /usr/lib/modules -path "*/updates/dkms/${drv}.ko*" -delete 2>/dev/null || true
}

sc0710_dkms_installed_src_version() {
    local dir drv="${SC0710_DRV_NAME}"

    for dir in /usr/src/${drv}-*; do
        [[ -d "$dir" ]] || continue
        basename "$dir"
        return 0
    done
    return 1
}

sc0710_dkms_ensure_installed() {
    local src_name dkms_ver kver dir drv="${SC0710_DRV_NAME}"

    src_name=$(sc0710_dkms_installed_src_version) || return 0
    dkms_ver="${src_name#${drv}-}"

    for dir in /usr/lib/modules/*/; do
        [[ -d "$dir" ]] || continue
        kver=$(basename "$dir")
        [[ -f "${dir}build/Makefile" ]] || continue

        if ! dkms status "${drv}/${dkms_ver}" 2>/dev/null | grep -q ", ${kver}, .*installed"; then
            dkms install -m "$drv" -v "$dkms_ver" -k "$kver" --force >/dev/null 2>&1 || true
        fi
    done

    depmod -a >/dev/null 2>&1 || true
}
