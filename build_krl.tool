#!/bin/bash
# build_krl.tool: build KernReserveLow.efi (DEBUG + RELEASE, X64) with EDK2, the way acidanthera builds OpenCorePkg.
# macOS: Xcode command line tools (XCODE5 toolchain); nasm, iasl and mtoc must be on PATH (efibuild.sh otherwise
#        offers to install prebuilt copies into /usr/local/bin with sudo).
# Linux: gcc or clang, nasm, iasl, python3, uuid-dev; GCC/CLANGPDB toolchains.
# Output: UDK/Build/KernReserveLowPkg/<TARGET>_<TOOLCHAIN>/X64/KernReserveLow.efi, copied to Binaries/<TARGET>/.
# no "set -e": efibuild.sh relies on commands that may return non-zero (e.g. its CRLF grep)
cd "$(dirname "$0")" || exit 1

package() {
  # called by efibuild.sh from this folder after the build: $1 = build output dir, $2 = target (or toolchain-target)
  mkdir -p "Binaries/$2"
  cp "$1/KernReserveLow.efi" "Binaries/$2/KernReserveLow.efi"
}

export SELFPKG=KernReserveLowPkg
export ARCHS=(X64)
export TARGETS=(DEBUG RELEASE)
export RTARGETS=(DEBUG RELEASE)
export NO_ARCHIVES=1

# Pinned copy of acidanthera/ocbuild efibuild.sh (fetched 2026-10-24, reviewed; sha256
# 4b8fae283dccce16594e01c20f19f2c4d4823e21c7d45015710764025a1c4e05). Not fetched from master at build time.
EFIBUILD=tools/efibuild.sh
[ "$(shasum -a 256 "$EFIBUILD" 2>/dev/null | cut -d' ' -f1)" = 4b8fae283dccce16594e01c20f19f2c4d4823e21c7d45015710764025a1c4e05 ] \
  || { echo "tools/efibuild.sh does not match the pinned sha256"; exit 1; }
. "$EFIBUILD" || exit 1
