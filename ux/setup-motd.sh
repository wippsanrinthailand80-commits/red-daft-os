#!/usr/bin/env bash
# setup-motd.sh — install the Red Daft MOTD for login shells + daft-shell.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${1:-/}"

install -Dm755 "$HERE/daft-motd.sh"        "$ROOT/usr/local/bin/daft-motd"
install -Dm755 "$HERE/branding.sh"         "$ROOT/usr/local/lib/daft/branding.sh"
install -Dm644 "$HERE/assets/logo.txt"     "$ROOT/usr/local/lib/daft/logo.txt"

# Login MOTD via update-motd.d (requires the daemon to run on SSH/tty login)
install -Dm755 /dev/stdin "$ROOT/etc/update-motd.d/30-daft" <<'EOF'
#!/usr/bin/env bash
RD_LOGO_FILE=/usr/local/lib/daft/logo.txt exec /usr/local/bin/daft-motd
EOF

# Bash login banner
install -Dm755 /dev/stdin "$ROOT/etc/profile.d/daft-motd.sh" <<'EOF'
# Red Daft OS MOTD on interactive login
if [[ $- == *i* ]] && [[ -x /usr/local/bin/daft-motd ]]; then
  /usr/local/bin/daft-motd
fi
EOF

echo "Red Daft MOTD installed into $ROOT"
