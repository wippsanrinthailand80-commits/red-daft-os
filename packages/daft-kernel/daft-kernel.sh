#!/usr/bin/env bash
# daft-kernel — switch the default boot kernel on Red Daft OS (GRUB)
# Works on live ISO (grub-reboot for next boot only) and installed systems (set-default).
# Pairs with the Demo Box 'kernel switch/reboot' commands.
set -euo pipefail

usage(){
  cat <<'EOF'
daft-kernel — Red Daft OS kernel switcher (GRUB)

  daft-kernel status              show current default + available entries
  daft-kernel list                list GRUB menuentry titles + ids
  daft-kernel set <name>          set default for all future boots (persistent)
  daft-kernel next <name>         set next boot only (one-shot, via grub-reboot)
  daft-kernel reboot [name]       set next (if given) and reboot now
  daft-kernel help

Names: aik | ai-kernel | demo  ->  "Red Daft AI-Kernel ..."
       linux | daft | daft-kernel  ->  "Red Daft OS ... (Live)"
       linux-safe | safe        ->  "Red Daft OS ... (safe graphics)"
You can also pass the full GRUB title or numeric index.

Needs root for set/next/reboot.
EOF
}

# normalize short names to a grep pattern for the GRUB title
normalize(){
  case "$1" in
    aik|ai-kernel|demo|ai) echo "AI-Kernel" ;;
    linux|daft|daft-kernel) echo "Red Daft OS 0.2 — kernel" ;;
    linux-safe|safe) echo "safe graphics" ;;
    *) echo "$1" ;;
  esac
}

grub_list(){
  # list from /boot/grub/grub.cfg if present, else from ISO template
  local cfg="/boot/grub/grub.cfg"
  [[ -f "$cfg" ]] || cfg="/boot/grub/grub.cfg"  # keep as is
  if [[ -f "$cfg" ]]; then
    grep -n "^menuentry " "$cfg" | sed -E 's/.*menuentry \"([^\"]+)\".*/\1/'
  else
    echo "Red Daft OS 0.2 — kernel (Live)"
    echo "Red Daft OS 0.2 — kernel (safe graphics)"
    echo "Red Daft AI-Kernel 0.1 — experimental bare-metal HMM"
  fi
}

resolve_entry(){
  # arg is name/pattern -> returns exact menuentry title or numeric id
  local want="$1"
  local norm; norm=$(normalize "$want")
  # if numeric, return as-is
  if [[ "$norm" =~ ^[0-9]+$ ]]; then echo "$norm"; return 0; fi
  # search titles
  local titles; titles=$(grub_list)
  local match
  match=$(echo "$titles" | grep -m1 -F "$norm" || true)
  if [[ -z "$match" ]]; then
    # try case-insensitive substring
    match=$(echo "$titles" | grep -i -m1 -F "$want" || true)
  fi
  if [[ -z "$match" ]]; then
    echo "unknown kernel '$want'. Available:" >&2
    grub_list | sed 's/^/  /' >&2
    return 1
  fi
  echo "$match"
}

cmd="${1:-status}"; shift || true
case "$cmd" in
  status)
    echo "== daft-kernel status =="
    if command -v grub-editenv >/dev/null 2>&1; then
      echo -n "saved_entry: "; grub-editenv list 2>/dev/null | grep saved_entry || echo "(none, default=0)"
      echo -n "next_entry:  "; grub-editenv list 2>/dev/null | grep next_entry || echo "(none)"
    else
      echo "(grub-editenv not found)"
    fi
    echo "entries:"
    grub_list | cat -n | sed 's/^/  /'
    ;;
  list|ls)
    grub_list | cat -n
    ;;
  set)
    [[ $# -ge 1 ]] || { echo "usage: daft-kernel set <name>" >&2; exit 1; }
    [[ $EUID -eq 0 ]] || { echo "need root (sudo)" >&2; exit 1; }
    entry=$(resolve_entry "$1")
    echo "[daft-kernel] set default -> $entry"
    if command -v grub-set-default >/dev/null 2>&1; then
      grub-set-default "$entry"
    else
      grub-editenv /boot/grub/grubenv set saved_entry="$entry"
    fi
    echo "done. Reboot to use '$entry'. (Demo Box: 'reboot' will come back here.)"
    ;;
  next)
    [[ $# -ge 1 ]] || { echo "usage: daft-kernel next <name>" >&2; exit 1; }
    [[ $EUID -eq 0 ]] || { echo "need root (sudo)" >&2; exit 1; }
    entry=$(resolve_entry "$1")
    echo "[daft-kernel] set next boot -> $entry"
    if command -v grub-reboot >/dev/null 2>&1; then
      grub-reboot "$entry"
    else
      grub-editenv /boot/grub/grubenv set next_entry="$entry"
    fi
    echo "done. Next reboot will use '$entry' once."
    ;;
  reboot)
    if [[ $# -ge 1 ]]; then
      [[ $EUID -eq 0 ]] || { echo "need root (sudo)" >&2; exit 1; }
      entry=$(resolve_entry "$1")
      echo "[daft-kernel] next -> $entry + reboot"
      if command -v grub-reboot >/dev/null 2>&1; then grub-reboot "$entry"
      else grub-editenv /boot/grub/grubenv set next_entry="$entry"; fi
    fi
    echo "[daft-kernel] rebooting..."
    reboot
    ;;
  help|--help|-h) usage ;;
  *) echo "unknown: $cmd" >&2; usage; exit 1 ;;
esac
