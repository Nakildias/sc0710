#!/bin/bash
# DKMS build wrapper for sc0710.
#
# DKMS always rewrites "make ..." into "make -j$(nproc) KERNELRELEASE=... ...",
# which can hit EMFILE in the kernel tree on high-core systems. Using a script
# as MAKE[0] bypasses that rewrite. We also raise the open-file limit because
# some distros give sudo/dkms a very low ulimit.
set -euo pipefail

kernelver="$1"
shift || true
while [[ $# -gt 0 && "$1" == *"="* ]]; do
    shift
done

ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || ulimit -n 4096 2>/dev/null || true
export MAKEFLAGS=

exec make -j1 KVERSION="$kernelver" KERNELRELEASE="$kernelver"
