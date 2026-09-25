#!/bin/bash
# Build everything into ./build. Re-running is cheap; ninja rebuilds only what
# changed. BUILD_TYPE=Debug for a debug build, BUILD_APP=OFF to skip the tray
# app and its GTK dependency.

set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
  -DBUILD_APP="${BUILD_APP:-ON}" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX:-$HOME/.local}"
cmake --build build
ctest --test-dir build --output-on-failure

echo
echo "Built:"
echo "  build/ac3spdif        the bitstreaming command line"
echo "  build/ac3spdif-probe  full PipeWire and ALSA device dump"
[ -x build/ac3spdif-app ] && echo "  build/ac3spdif-app    the tray app"
echo
echo "Try:  ./build/ac3spdif --list"
