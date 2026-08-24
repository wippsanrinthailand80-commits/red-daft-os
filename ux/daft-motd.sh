#!/usr/bin/env bash
# daft-motd.sh — dynamic Message of the Day for Red Daft OS terminals.
# Shows: crimson logo, kernel, local Ollama status, RAM usage, quick commands.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=branding.sh
source "$HERE/branding.sh"

ollama_status() {
  if command -v ollama >/dev/null 2>&1 && pgrep -x ollama >/dev/null 2>&1; then
    local m; m="$(ollama list 2>/dev/null | awk 'NR==2{print $1}')"
    echo "ONLINE (${m:-qwen2.5:3b})"
  else
    echo "offline — start: ollama serve"
  fi
}

ram_usage() {
  free -h 2>/dev/null | awk '/^Mem:/{printf "%s / %s", $3, $2}' || echo "n/a"
}

clear
printf '%s' "$RD_BLACK_BG"
rd_logo
printf '%s' "$RD_RESET"
rd_rule
printf '%sKernel %s  |  Ollama: %s  |  RAM: %s%s\n' \
  "$RD_RED" "$(uname -r)" "$(ollama_status)" "$(ram_usage)" "$RD_RESET"
rd_rule
printf '%sQuick commands:%s\n' "$RD_BOLD$RD_RED" "$RD_RESET"
printf '  %sdaft-shell%s   in-RAM C/Python execution\n' "$RD_RED" "$RD_RESET"
printf '  %sdaft-pkg%s     native package manager\n' "$RD_RED" "$RD_RESET"
printf '  %sdaft-deb%s     install Ubuntu .deb (deps resolved)\n' "$RD_RED" "$RD_RESET"
printf '  %sollama-setup scale 7b%s  bump local model\n' "$RD_RED" "$RD_RESET"
printf '  %sdaft-ai-guard%s  defensive AI content filter\n' "$RD_RED" "$RD_RESET"
rd_rule
