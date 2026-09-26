#!/bin/bash
# Build installable packages into ./dist, each in a clean environment so the
# result depends only on what the package declares:
#
#   ./scripts/package.sh rpm       Fedora .rpm, built in a Fedora container
#   ./scripts/package.sh deb       Ubuntu/Debian .deb, built in an Ubuntu container
#   ./scripts/package.sh flatpak   a single-file Flatpak bundle
#   ./scripts/package.sh all
#
# The RPM and the deb need only podman on the host; the Flatpak needs flatpak,
# and fetches the builder and the runtime from Flathub the first time. The RPM
# and deb declare their libraries (ffmpeg, PipeWire, ALSA, GLib, GTK, the
# appindicator) so dnf and apt install them alongside; the Flatpak carries
# them in its runtime. FEDORA=45 or UBUNTU=22.04 target another release.

set -euo pipefail
cd "$(dirname "$0")/.."

NAME=ac3spdif
VERSION=$(sed -n 's/.*AC3SPDIF_VERSION "\([0-9.]*\)".*/\1/p' src/engine/version.h)
APP_ID=$(sed -n 's/.*AC3SPDIF_APP_ID "\([^"]*\)".*/\1/p' src/engine/version.h)
FEDORA="${FEDORA:-44}"
UBUNTU="${UBUNTU:-24.04}"
mkdir -p dist

# The spec and the Debian changelog carry the version too; refuse to build a
# package whose metadata disagrees with the binaries.
check_version() {
  local spec deb
  spec=$(sed -n 's/^Version: *//p' packaging/fedora/ac3spdif.spec)
  deb=$(head -1 packaging/debian/changelog | sed 's/.*(\([^)]*\)).*/\1/')
  [ "$spec" = "$VERSION" ] || { echo "packaging/fedora/ac3spdif.spec says $spec, src/engine/version.h says $VERSION" >&2; exit 1; }
  [ "$deb" = "$VERSION" ] || { echo "packaging/debian/changelog says $deb, src/engine/version.h says $VERSION" >&2; exit 1; }
}

# The working tree as it is, uncommitted changes included, without build products.
tarball() {
  tar czf "dist/$NAME-$VERSION.tar.gz" --exclude-vcs \
    --exclude=./build --exclude='./build-*' --exclude=./dist --exclude=./.cache --exclude=./.flatpak-builder \
    --transform "s,^\./,$NAME-$VERSION/," .
}

container() {
  local image=$1; shift
  podman run --rm --security-opt label=disable \
    -v "$PWD/packaging:/packaging:ro" -v "$PWD/dist:/dist" \
    -e NAME="$NAME" -e VERSION="$VERSION" "$image" bash -euo pipefail -c "$@"
}

build_rpm() {
  check_version; tarball
  container "registry.fedoraproject.org/fedora:$FEDORA" '
    dnf -q install -y rpm-build rpmlint "dnf-command(builddep)"
    dnf -q builddep -y /packaging/fedora/ac3spdif.spec
    mkdir -p ~/rpmbuild/SOURCES
    cp "/dist/$NAME-$VERSION.tar.gz" ~/rpmbuild/SOURCES/
    rpmbuild -bb /packaging/fedora/ac3spdif.spec
    echo; echo "rpmlint:"; rpmlint ~/rpmbuild/RPMS/*/"$NAME"-[0-9]*.rpm || true
    find ~/rpmbuild/RPMS -name "$NAME-[0-9]*.rpm" ! -name "*debug*" -exec cp {} /dist/ \;'
  echo; echo "built:"; ls -1 dist/*.rpm
}

build_deb() {
  check_version; tarball
  container "docker.io/library/ubuntu:$UBUNTU" '
    export DEBIAN_FRONTEND=noninteractive
    apt-get -qq update
    apt-get -qq install -y --no-install-recommends build-essential debhelper devscripts lintian >/dev/null
    mkdir /build && cd /build
    tar xzf "/dist/$NAME-$VERSION.tar.gz" && cd "$NAME-$VERSION"
    cp -r /packaging/debian debian
    apt-get -qq build-dep -y ./ >/dev/null
    dpkg-buildpackage -us -uc -b
    echo; echo "lintian:"; lintian ../*.deb || true
    cp ../*.deb /dist/'
  echo; echo "built:"; ls -1 dist/*.deb
}

build_flatpak() {
  local builder
  if command -v flatpak-builder >/dev/null; then
    builder=(flatpak-builder)
  else
    flatpak info --user org.flatpak.Builder >/dev/null 2>&1 || flatpak info org.flatpak.Builder >/dev/null 2>&1 ||
      flatpak install --user -y flathub org.flatpak.Builder
    builder=(flatpak run org.flatpak.Builder)
  fi
  rm -rf dist/flatpak-build
  "${builder[@]}" --force-clean --user --install-deps-from=flathub --ccache \
    --state-dir=dist/flatpak-state --repo=dist/flatpak-repo \
    dist/flatpak-build "packaging/flatpak/$APP_ID.yml"
  flatpak build-bundle dist/flatpak-repo "dist/$APP_ID.flatpak" "$APP_ID"
  echo; echo "built: dist/$APP_ID.flatpak"
  echo "install with:  flatpak install --user dist/$APP_ID.flatpak"
}

case "${1:-}" in
  rpm) build_rpm ;;
  deb) build_deb ;;
  flatpak) build_flatpak ;;
  all) build_rpm; build_deb; build_flatpak ;;
  *) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
