#!/bin/bash
# Install nv-sensors-extra via DKMS (dkms install).
# Usage: sudo ./nv-sensors-extra-dkms-install.sh [kernel-version]
set -euo pipefail

PKG_NAME="nv-sensors-extra"
PKG_VERSION="1.2"
MOD_NAME="nv-sensors-extra"
KERNEL="${1:-$(uname -r)}"

[[ $(id -u) -eq 0 ]] || { echo "ERROR: run as root (sudo)" >&2; exit 1; }

# Capture status before grepping: `grep -q` exits on first match, so piping
# dkms straight into it kills dkms with SIGPIPE and pipefail reports 141.
# dkms status (3.x) format: "<name>/<ver>, <kernel>, <arch>: built"
DKMS_STATUS="$(dkms status || true)"
if ! grep -q "^${PKG_NAME}/${PKG_VERSION}" <<<"${DKMS_STATUS}"; then
    echo "ERROR: ${PKG_NAME}/${PKG_VERSION} not added. Run the build script first." >&2
    exit 1
fi
if ! grep -q "^${PKG_NAME}/${PKG_VERSION}, ${KERNEL}," <<<"${DKMS_STATUS}"; then
    echo "ERROR: ${PKG_NAME}/${PKG_VERSION} not built for ${KERNEL}. Run the build script for that kernel first." >&2
    exit 1
fi

dkms install "${PKG_NAME}/${PKG_VERSION}" -k "${KERNEL}"

echo ">> Installed:"
ls -l "/lib/modules/${KERNEL}/updates/dkms/${MOD_NAME}.ko"*
