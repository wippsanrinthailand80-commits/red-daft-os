#!/usr/bin/env bash
# deb-adapter — install Ubuntu .deb packages on Red Daft OS
# Satisfies shared-library (.so) dependencies from the Ubuntu pool.
set -euo pipefail

UBUNTU_CODENAME="${UBUNTU_CODENAME:-noble}"
ARCH="${ARCH:-$(dpkg --print-architecture 2>/dev/null || echo amd64)}"
MIRROR="${DAFT_DEB_MIRROR:-http://archive.ubuntu.com/ubuntu}"

usage() { echo "daft-deb <install|resolve> <pkg.deb|pkg-name>"; }

resolve_deps() {
  local deb="$1"
  echo "Resolving dependencies for $deb ..."
  # Use apt-get in a controlled way to fetch + satisfy .so deps
  apt-get update -o Dir::Etc::sourcelist="sources.daft.list" >/dev/null 2>&1 || true
  apt-get install -y --no-install-recommends \
    -o Dpkg::Options::="--force-confdef" \
    "$(dpkg-deb -f "$deb" Depends | tr ',' ' ')" 2>&1 | tail -n 20
}

install_deb() {
  local deb="$1"
  [[ -f "$deb" ]] || { echo "not found: $deb" >&2; exit 1; }
  resolve_deps "$deb"
  dpkg -i "$deb" || apt-get install -fy
  echo "installed (with .so deps resolved): $deb"
}

cmd="${1:-}"; shift || true
case "$cmd" in
  install) install_deb "${1:?missing .deb}";;
  resolve) resolve_deps "${1:?missing .deb}";;
  *) usage;;
esac
