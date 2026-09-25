#!/bin/bash
# Install the build and runtime dependencies on Fedora, Debian/Ubuntu or
# Arch/Manjaro. Everything comes from the standard repositories; the ffmpeg
# libraries shipped by each distribution include the AC-3 and DTS encoders and
# the IEC 61937 muxer this project needs.

set -euo pipefail

if [ -r /etc/os-release ]; then
  . /etc/os-release
fi
ID_LIKE="${ID_LIKE:-}"
family=""
case "${ID:-}" in
  fedora|rhel|centos|rocky|almalinux|nobara) family=fedora ;;
  debian|ubuntu|linuxmint|pop|elementary|zorin) family=debian ;;
  arch|manjaro|endeavouros|garuda|cachyos) family=arch ;;
esac
if [ -z "$family" ]; then
  case "$ID_LIKE" in
    *fedora*|*rhel*) family=fedora ;;
    *debian*|*ubuntu*) family=debian ;;
    *arch*) family=arch ;;
  esac
fi

case "$family" in
  fedora)
    sudo dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config \
      pipewire-devel alsa-lib-devel ffmpeg-free-devel glib2-devel \
      gtk3-devel libappindicator-gtk3-devel
    ;;
  debian)
    sudo apt-get install -y g++ cmake ninja-build pkg-config \
      libpipewire-0.3-dev libasound2-dev libavcodec-dev libavformat-dev \
      libavutil-dev libswresample-dev libglib2.0-dev libgtk-3-dev \
      libayatana-appindicator3-dev
    ;;
  arch)
    sudo pacman -S --needed gcc cmake ninja pkgconf pipewire alsa-lib ffmpeg \
      glib2 gtk3 libappindicator-gtk3
    ;;
  *)
    cat >&2 <<'MSG'
Unrecognised distribution. Install the equivalents of:
  a C++20 compiler, cmake, ninja, pkg-config
  libpipewire-0.3 (dev), alsa-lib (dev), ffmpeg's libavcodec/libavformat/
  libavutil/libswresample (dev), glib2/gio (dev), gtk3 (dev),
  libappindicator-gtk3 or libayatana-appindicator (dev)
MSG
    exit 1
    ;;
esac

cat <<'MSG'

Dependencies installed. Next:
  ./scripts/build.sh          # builds into ./build
  ./build/ac3spdif --list     # what can carry a bitstream
  ./build/ac3spdif --selftest # a tone per channel to the best digital output
  ./build/ac3spdif-app        # the tray app

GNOME needs the AppIndicator extension for a tray icon:
  Fedora: sudo dnf install gnome-shell-extension-appindicator
  Ubuntu: installed by default
  Arch:   the "appindicatorsupport@rgcjonas.gmail.com" extension from extensions.gnome.org
KDE Plasma, XFCE, MATE and Cinnamon need nothing extra.
MSG
