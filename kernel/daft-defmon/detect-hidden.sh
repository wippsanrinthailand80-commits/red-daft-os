#!/usr/bin/env bash
# detect-hidden.sh — userspace companion to daft-defmon.ko
# Diffs the authoritative kernel task list against /proc to surface processes
# an attacker attempted to hide via DKOM/unlinking. Detection only.
set -euo pipefail

PROC="/proc/daft_defmon"
if [[ ! -r "$PROC" ]]; then
	echo "[-] $PROC not found. Load the module first:" >&2
	echo "    insmod daft-defmon.ko" >&2
	exit 1
fi

mapfile -t kernel < <(awk '{print $1}' "$PROC" | sort -n)
mapfile -t procfs < <(ls /proc | grep -E '^[0-9]+$' | sort -n)

hidden=$(comm -23 <(printf '%s\n' "${kernel[@]}") <(printf '%s\n' "${procfs[@]}"))

if [[ -n "$hidden" ]]; then
	echo "[!] Processes present in kernel task list but MISSING from /proc (possible DKOM hiding):"
	while read -r p; do
		[[ -z "$p" ]] && continue
		comm=$(awk -v pid="$p" '$1==pid{print $2}' "$PROC")
		echo "    PID $p  ->  ${comm:-<unknown>}"
	done <<< "$hidden"
	exit 2
else
	echo "[+] No hidden processes detected (kernel task list == /proc)."
fi
