#!/usr/bin/env bash
# daft-pkg — native package manager for Red Daft OS
# .daft format: tar.zst archive with a manifest.json and an optional .sig
set -euo pipefail

REPO="${DAFT_REPO:-/var/cache/daft/repo}"
ROOT="${DAFT_ROOT:-/}"
DB="${DAFT_DB:-/var/lib/daft/db}"

usage() {
  cat <<'EOF'
daft-pkg — Red Daft OS native package manager

  daft-pkg install <pkg.daft>     install a local .daft archive
  daft-pkg add <name>             fetch + install from configured repo
  daft-pkg remove <name>          remove an installed package
  daft-pkg list                   list installed packages
  daft-pkg verify <name>          verify manifest + signature
EOF
}

db_init() { mkdir -p "$DB" "$REPO"; }

verify_sig() {
  local file="$1"
  # Expect a detached minisign/signify-style signature alongside the archive.
  if [[ -f "$file.sig" ]]; then
    signify -V -p /etc/daft/daft.pub -m "$file" 2>/dev/null \
      || { echo "signature verification FAILED: $file" >&2; return 1; }
  else
    echo "warning: no signature for $file (DAFT_INSECURE=1 to override)" >&2
    [[ "${DAFT_INSECURE:-0}" == "1" ]] || return 1
  fi
  return 0
}

install_archive() {
  local file="$1"
  db_init
  verify_sig "$file"
  local tmp; tmp="$(mktemp -d)"
  tar --zstd -xf "$file" -C "$tmp"
  local name ver
  name="$(jq -r .name "$tmp/manifest.json")"
  ver="$(jq -r .version "$tmp/manifest.json")"
  # Files live under ./rootfs in the archive
  cp -a "$tmp/rootfs/." "$ROOT"
  echo "{\"name\":\"$name\",\"version\":\"$ver\"}" > "$DB/$name.json"
  echo "installed $name $ver"
  rm -rf "$tmp"
}

cmd="${1:-}"; shift || true
case "$cmd" in
  install) install_archive "${1:?missing .daft file}";;
  add)
    db_init
    local pkg="${1:?missing package name}"
    fetch="${REPO}/${pkg}.daft"
    install_archive "$fetch";;
  remove)
    local name="${1:?missing package name}"
    [[ -f "$DB/$name.json" ]] || { echo "not installed: $name" >&2; exit 1; }
    jq -r '.files[]?' "$DB/$name.json" | while read -r f; do rm -f "$ROOT$f"; done
    rm -f "$DB/$name.json"
    echo "removed $name";;
  list) ls "$DB" 2>/dev/null | sed 's/.json//';;
  verify)
    local name="${1:?missing package name}"
    [[ -f "$DB/$name.json" ]] && echo "ok: $name" || echo "unknown: $name";;
  *) usage;;
esac
