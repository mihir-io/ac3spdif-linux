#!/bin/bash
# Install into a prefix (default ~/.local, no root needed): the three
# binaries, the icons, and the desktop entry. UNINSTALL=1 removes them.

set -euo pipefail
cd "$(dirname "$0")/.."
PREFIX="${PREFIX:-$HOME/.local}"

if [ "${UNINSTALL:-0}" = "1" ]; then
  rm -f "$PREFIX/bin/ac3spdif" "$PREFIX/bin/ac3spdif-app" "$PREFIX/bin/ac3spdif-probe"
  rm -rf "$PREFIX/share/ac3spdif"
  rm -f "$PREFIX/share/applications/org.ac3spdif.App.desktop" \
        "$PREFIX/share/icons/hicolor/scalable/apps/org.ac3spdif.App.svg" \
        "$PREFIX/share/metainfo/org.ac3spdif.App.metainfo.xml"
  systemctl --user disable --now ac3spdif.service 2>/dev/null || true
  rm -f "$HOME/.config/systemd/user/ac3spdif.service" \
        "$HOME/.config/autostart/org.ac3spdif.App.desktop" "$HOME/.config/autostart/ac3spdif.desktop"
  echo "removed from $PREFIX"
  exit 0
fi

[ -x build/ac3spdif ] || ./scripts/build.sh
cmake --install build --prefix "$PREFIX"
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q "$PREFIX/share/icons/hicolor" 2>/dev/null || true
command -v update-desktop-database >/dev/null && update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true

echo
echo "installed to $PREFIX"
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "note: $PREFIX/bin is not on your PATH" ;;
esac
echo
echo "  ac3spdif --list        what can carry a bitstream"
echo "  ac3spdif --selftest    a tone per channel to the best digital output"
echo "  ac3spdif-app           the tray app (also in your application menu as AC3SPDIF)"
