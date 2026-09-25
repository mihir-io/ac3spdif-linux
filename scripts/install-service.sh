#!/bin/bash
# Install a systemd user unit so ac3spdif streams from login with no UI.
#
# A user unit rather than a system service: the sink switch and the output
# device belong to the logged-in user's PipeWire session, and root would have
# no business holding them. Any arguments are passed to ac3spdif, so
#   ./scripts/install-service.sh --codec dts --latency low
# runs exactly that at every login. The tray app's "Run as Background Service"
# does the same with the settings currently selected there.

set -euo pipefail
cd "$(dirname "$0")/.."

case "${1:-}" in
  uninstall)
    systemctl --user disable --now ac3spdif.service 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/ac3spdif.service"
    systemctl --user daemon-reload
    echo "removed ac3spdif.service"
    exit 0
    ;;
esac

BINARY="$(command -v ac3spdif || true)"
[ -x "$PWD/build/ac3spdif" ] && BINARY="$PWD/build/ac3spdif"
[ -n "$BINARY" ] || { echo "build first: ./scripts/build.sh" >&2; exit 1; }

quoted=""
for arg in "$@"; do quoted="$quoted \"${arg//\"/\\\"}\""; done

UNIT="$HOME/.config/systemd/user/ac3spdif.service"
mkdir -p "$(dirname "$UNIT")"
cat > "$UNIT" <<UNITEOF
[Unit]
Description=AC3SPDIF surround bitstream encoder
After=pipewire.service wireplumber.service pipewire-pulse.service
Wants=pipewire.service

[Service]
ExecStart="$BINARY" --quiet --status 0$quoted
Restart=on-failure
RestartSec=15

[Install]
WantedBy=default.target
UNITEOF

systemctl --user daemon-reload
systemctl --user enable --now ac3spdif.service
echo "installed ac3spdif.service"
echo "  status:    systemctl --user status ac3spdif"
echo "  log:       journalctl --user -u ac3spdif -f"
echo "  stop:      systemctl --user stop ac3spdif"
echo "  uninstall: $0 uninstall"
