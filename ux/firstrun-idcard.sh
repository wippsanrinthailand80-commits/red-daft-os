#!/usr/bin/env bash
# firstrun-idcard.sh — Red Daft "Classified ID Card" TUI (first boot).
# Cosmetic only: crimson ASCII logo + randomized masked network pattern.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=branding.sh
source "$HERE/branding.sh"

rand_octet() { echo $((RANDOM % 254 + 1)); }

gen_handle() {
  local adj=(shadow cipher neon cobalt ghost voltaic)
  local noun=(daemon proxy sentinel wisp relay specter)
  echo "${adj[$((RANDOM % ${#adj[@]}))]}-${noun[$((RANDOM % ${#noun[@]}))]}-$((RANDOM % 9000 + 1000))"
}

# Randomized masked local-network pattern (10.x.x.x OR 192.168.x.x only).
masked_ip() {
  if (( RANDOM % 2 )); then
    echo "10.$(rand_octet).$(rand_octet).x"
  else
    echo "192.168.$(rand_octet).x"
  fi
}

clear
rd_black_and_logo() { :; }
printf '%s' "$RD_BLACK_BG"
rd_logo
printf '%s' "$RD_RESET"
rd_rule

cat <<EOF
${RD_BOLD}${RD_RED}    R E D   D A F T   O S  —  CLASSIFIED ID CARD${RD_RESET}
${RD_RED}--------------------------------------------------------------${RD_RESET}
  AGENT HANDLE : $(gen_handle)
  CLEARANCE    : DEVELOPER / RED-TEAM (AUTHORIZED SCOPE)
  NODE ID      : RD-$(printf '%04X' $RANDOM)-$(printf '%04X' $RANDOM)
  LOCAL MASK   : $(masked_ip)
  AI COPILOT   : reddaft (qwen2.5:3b, local)
  STATUS       : OPERATIONAL — offensive tooling authorization-gated
EOF

rd_rule
printf '%sWelcome, agent.%s Type %sdaft-shell%s to begin. Stay legal.\n' \
  "$RD_RED" "$RD_RESET" "$RD_BOLD" "$RD_RESET"
