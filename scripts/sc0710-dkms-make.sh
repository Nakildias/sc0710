#!/bin/bash
# DKMS build wrapper for sc0710.
#
# DKMS rewrites MAKE[0]="make ..." into "make -j$(nproc) KERNELRELEASE=...",
# which can hit EMFILE in the kernel tree on high-core systems. Using a script
# as MAKE[0] bypasses that rewrite. We also raise the open-file limit because
# some distros give sudo/dkms a very low ulimit.
set -euo pipefail

kernelver="$1"
shift || true

ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || ulimit -n 4096 2>/dev/null || true

# Older sc0710 builds used MO= and could leave bogus cmd files in the kernel
# headers tree; remove them so kbuild does not try to "compile" Makefile.build.
for kdir in "/lib/modules/${kernelver}/build" "/usr/lib/modules/${kernelver}/build"; do
    [[ -d "${kdir}/scripts" ]] || continue
    rm -f "${kdir}/scripts/Makefile.build.mod" \
          "${kdir}/scripts/.Makefile.build.mod.cmd"
done

exec make -j1 KVERSION="$kernelver" KERNELRELEASE="$kernelver" "$@"
