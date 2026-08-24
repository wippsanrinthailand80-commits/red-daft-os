#!/usr/bin/env bash
# branding.sh — shared Red Daft OS branding (crimson #FF0033 / matte black).
# Source this from any script that renders the logo or themed output.

# Truecolor crimson + matte black
RD_RED=$'\033[38;2;255;0;51m'
RD_RED_DIM=$'\033[38;2;160;0;32m'
RD_BLACK_BG=$'\033[48;2;12;12;12m'
RD_BOLD=$'\033[1m'
RD_DIM=$'\033[2m'
RD_RESET=$'\033[0m'

RD_LOGO_FILE="${RD_LOGO_FILE:-$(dirname "${BASH_SOURCE[0]}")/assets/logo.txt}"

# Print the block banner in crimson on matte black.
rd_logo() {
  if [[ -r "$RD_LOGO_FILE" ]]; then
    while IFS= read -r line; do
      printf '%s%s%s\n' "$RD_RED" "$line" "$RD_RESET"
    done < "$RD_LOGO_FILE"
  else
    printf '%sRED DAFT OS%s\n' "$RD_RED" "$RD_RESET"
  fi
}

# Print themed text: rd_echo "message"
rd_echo() {
  printf '%s%s%s%s\n' "$RD_BLACK_BG" "$RD_RED" "$*" "$RD_RESET"
}

# A crimson horizontal rule sized to the terminal width.
rd_rule() {
  local w="${COLUMNS:-$(tput cols 2>/dev/null || echo 60)}"
  printf '%s%*s%s\n' "$RD_RED" "$w" "" "$RD_RESET" | tr ' ' '='
}
