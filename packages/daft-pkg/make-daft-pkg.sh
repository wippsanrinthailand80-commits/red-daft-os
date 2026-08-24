#!/usr/bin/env bash
# make-daft-pkg.sh — generic .daft builder used by all package specs.
# Produces: <name>-<version>.daft  = tar.zst(./rootfs + manifest.json)
set -euo pipefail

PKGDIR="${1:?usage: make-daft-pkg.sh <pkgdir> <name> <version> [out.daft]}"
NAME="${2:?}"
VERSION="${3:?}"
OUT="${4:-$NAME-$VERSION.daft}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cp -a "$PKGDIR/rootfs" "$TMP/rootfs" 2>/dev/null || mkdir -p "$TMP/rootfs"
files=$(cd "$PKGDIR/rootfs" 2>/dev/null && find . -type f | sed 's#^\./##') || files=""
joined=$(printf '%s\n' "$files" | sed 's/.*/"&"/' | paste -sd, -)

cat > "$TMP/manifest.json" <<JSON
{
  "name": "$NAME",
  "version": "$VERSION",
  "files": [ $joined ]
}
JSON

tar --zstd -cf "$OUT" -C "$TMP" rootfs manifest.json
echo "built: $OUT ($(du -h "$OUT" | cut -f1))"
