#!/usr/bin/env bash
# daft-shell.sh — terminal-first shell with in-RAM code execution.
# Embedded compilers (TCC for C, python for scripts) run entirely in /dev/shm.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../ux/branding.sh
source "$HERE/ux/branding.sh" 2>/dev/null || true
# shellcheck source=../ux/daft-motd.sh
source "$HERE/ux/daft-motd.sh" 2>/dev/null || true

SHM="/dev/shm/daft-$$"
mkdir -p "$SHM"

run_c() {
  local src="$SHM/inmem.c"
  cat > "$src"
  if command -v tcc >/dev/null 2>&1; then
    tcc -run "$src"
  else
    gcc -x c -o "$SHM/a.out" "$src" && "$SHM/a.out"
  fi
}

run_py() {
  python3 - "$@" <<'PY'
import sys
code = sys.stdin.read()
exec(compile(code, "<daft-mem>", "exec"))
PY
}

cleanup() { rm -rf "$SHM"; }
trap cleanup EXIT

case "${1:-interactive}" in
  c)  run_c;;
  py) shift; run_py "$@";;
  interactive)
    echo "daft-shell — type ':c' or ':py' then code, Ctrl-D to run; ':q' to quit"
    while IFS= read -r line; do
      case "$line" in
        ':q') exit 0;;
        ':c') run_c < /dev/stdin;;
        ':py') run_py < /dev/stdin;;
        *) echo "daft> $line";;
      esac
    done;;
  *) echo "usage: daft-shell [c|py|interactive]";;
esac
