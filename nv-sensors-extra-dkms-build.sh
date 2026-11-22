#!/bin/bash
# Build nv-sensors-extra via DKMS (dkms add + dkms build).
# Usage: sudo ./nv-sensors-extra-dkms-build.sh [kernel-version]
set -euo pipefail

PKG_NAME="nv-sensors-extra"
PKG_VERSION="1.2"
MOD_NAME="nv-sensors-extra"
# Source directory: this repository by default; override with SRC_DIR=...
SRC_DIR="${SRC_DIR:-$(cd "$(dirname "$0")" && pwd)}"
KERNEL="${1:-$(uname -r)}"

[[ $(id -u) -eq 0 ]] || { echo "ERROR: run as root (sudo)" >&2; exit 1; }
[[ -f "${SRC_DIR}/dkms.conf" ]] || { echo "ERROR: no dkms.conf in ${SRC_DIR}" >&2; exit 1; }

# Capture status before grepping: `grep -q` exits on first match, so piping
# dkms straight into it kills dkms with SIGPIPE and pipefail reports 141.
# dkms status (3.x) format: "<name>/<ver>, <kernel>, <arch>: built"
DKMS_STATUS="$(dkms status || true)"
if grep -q "^${PKG_NAME}/${PKG_VERSION}" <<<"${DKMS_STATUS}"; then
    echo ">> ${PKG_NAME}/${PKG_VERSION} already added, skipping dkms add"
else
    dkms add "${SRC_DIR}"
fi

dkms build "${PKG_NAME}/${PKG_VERSION}" -k "${KERNEL}"

echo ">> Built for ${KERNEL}:"
find "/var/lib/dkms/${PKG_NAME}/${PKG_VERSION}" \( -name "${MOD_NAME}.ko" -o -name "${MOD_NAME}.ko.zst" \) -print 2>/dev/null || true
