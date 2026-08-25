#!/usr/bin/env bash
# nvidia-common.sh — shared helpers for Red Daft NVIDIA/CUDA .daft specs.
# Adds the official NVIDIA CUDA apt repository and validates packages.
set -euo pipefail

CUDA_VER="${CUDA_VER:-12.6}"
NVIDIA_REPO="https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64"
NVIDIA_KEY_URL="https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/3bf863cc.pub"

# Register the NVIDIA CUDA apt repo on the host so apt can resolve packages.
# NOTE: NVIDIA's signing key fails modern apt's sqv binding check, so
# trusted=yes is used — same workaround as AMD ROCm. The GPG key is
# still imported for end-user systems that use gpgv.
add_nvidia_repo() {
  local dest="${1:-/}"
  local line="deb [arch=amd64 trusted=yes] $NVIDIA_REPO /"
  mkdir -p /etc/apt/keyrings /etc/apt/sources.list.d
  if [ ! -f /etc/apt/keyrings/nvidia-cuda.gpg ]; then
    curl -fsSL "$NVIDIA_KEY_URL" \
      | gpg --dearmor -o /etc/apt/keyrings/nvidia-cuda.gpg 2>/dev/null \
      || echo "[!] nvidia GPG key import skipped"
  fi
  echo "$line" > /etc/apt/sources.list.d/nvidia-cuda.list
  if [[ "$dest" != "/" && -d "$dest" ]]; then
    mkdir -p "$dest/etc/apt/keyrings" "$dest/etc/apt/sources.list.d" 2>/dev/null || true
    cp /etc/apt/keyrings/nvidia-cuda.gpg "$dest/etc/apt/keyrings/nvidia-cuda.gpg" 2>/dev/null || true
    echo "$line" > "$dest/etc/apt/sources.list.d/nvidia-cuda.list" 2>/dev/null || true
  fi
  apt-get update -o Dir=/ 2>&1 | grep -Ev '^(Get|Hit|Ign|Reading|Fetched)' || true
}

# CI check mode: verify each package exists in the configured CUDA repo.
check_debs() {
  shift || true
  echo "[check] validating NVIDIA package existence: $*"
  local ok=0 bad=""
  for pkg in "$@"; do
    if apt-cache show "$pkg" >/dev/null 2>&1; then
      ok=$((ok+1))
    else
      bad="$bad $pkg"
    fi
  done
  echo "[check] $ok/$# packages found in repo"
  if [ -n "$bad" ]; then
    echo "[check] FAILED: missing packages:$bad"
    return 1
  fi
  echo "[check] OK: all packages resolve"
}

# Full stage: download + extract the named .debs into $1/rootfs.
stage_debs() {
  local root="$1"; shift
  mkdir -p "$root/opt/daft/nvidia/stage"
  apt-get download "$@"
  for d in *.deb; do dpkg-deb -x "$d" "$root"; done
  rm -f *.deb
}

build_daft() {
  local work="$1" name="$2" version="$3"
  "$(dirname "${BASH_SOURCE[0]}")/../daft-pkg/make-daft-pkg.sh" \
    "$work" "$name" "$version" "$name-$version.daft"
}
