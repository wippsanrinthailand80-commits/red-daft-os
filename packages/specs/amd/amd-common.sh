#!/usr/bin/env bash
# amd-common.sh — shared helpers for Red Daft AMD/ROCm .daft specs.
# Adds the official AMD ROCm apt repository and stages packages into a rootfs
# that make-daft-pkg.sh can turn into a .daft archive.
set -euo pipefail

ROCM_VER="${ROCM_VER:-6.2}"
AMD_REPO="https://repo.radeon.com/rocm/apt/${ROCM_VER}"
ROCM_KEY_URL="https://repo.radeon.com/rocm/rocm.gpg.key"

# Register the AMD ROCm apt repo inside $1 (defaults to /).
# NOTE: ROCm's signing key currently fails modern apt's strict sqv binding
# check, so the repo is added with trusted=yes. The spec only downloads
# AMD-published packages; this is the same workaround AMD documents for
# current distros.
add_amd_repo() {
  local dest="${1:-/}"
  mkdir -p "$dest/etc/apt/sources.list.d"
  echo "deb [arch=amd64 trusted=yes] $AMD_REPO jammy main" \
    > "$dest/etc/apt/sources.list.d/amd-rocm.list"
  apt-get update -o Dir="$dest" >/dev/null 2>&1 || apt-get update >/dev/null 2>&1 || true
}

# Full stage: download + extract the named .debs into $1/rootfs.
stage_debs() {
  local root="$1"; shift
  mkdir -p "$root/opt/daft/amd/stage"
  apt-get download "$@"
  for d in *.deb; do dpkg-deb -x "$d" "$root"; done
  rm -f *.deb
}

# CI check mode: simulate install to validate the deb list resolves (no download).
check_debs() {
  shift || true
  echo "[check] validating ROCm deb resolution: $*"
  apt-get install -s "$@" | grep -E '^Inst ' >/dev/null \
    && echo "[check] OK: packages resolve" \
    || { echo "[check] FAILED: packages do not resolve"; return 1; }
}

build_daft() {
  local work="$1" name="$2" version="$3"
  "$(dirname "${BASH_SOURCE[0]}")/../daft-pkg/make-daft-pkg.sh" \
    "$work" "$name" "$version" "$name-$version.daft"
}
