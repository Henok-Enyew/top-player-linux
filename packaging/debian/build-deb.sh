#!/usr/bin/env bash
# Builds top-player_<version>+<distro>_<arch>.deb for the Debian or Ubuntu
# release it runs on. Run from the repository root:
#
#   sudo apt-get install -y git devscripts equivs
#   VERSION=1.0.8 packaging/debian/build-deb.sh
#
# Installs the build dependencies with mk-build-deps when run as root
# (as in CI); otherwise install them first, see packaging/debian/control.
set -euo pipefail

ROOT="$(pwd)"
VERSION="${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}"
VERSION="${VERSION#v}"
# Native Debian versions cannot contain '-'; '~' sorts before the release.
DEB_VERSION="${VERSION//-/\~}"

# Tag the version with the distro, so one release can carry a .deb for each
# (a package built against one release's Qt and libmpv needs those versions).
. /etc/os-release
DISTRO="${ID}${VERSION_ID:-}"
DEB_VERSION="${DEB_VERSION}+${DISTRO}"
CODENAME="${VERSION_CODENAME:-unstable}"

WORK="${WORK:-$ROOT/build-deb}"
SRC="$WORK/top-player-${DEB_VERSION}"
rm -rf "$WORK"
mkdir -p "$SRC"
git -c safe.directory="$ROOT" -C "$ROOT" archive --format=tar HEAD | tar -x -C "$SRC"
cp -r "$ROOT/packaging/debian" "$SRC/debian"
rm -f "$SRC/debian/build-deb.sh"
# Pass the optional OpenSubtitles key through the untracked file CMake reads.
if [[ -n "${OPENSUBTITLES_API_KEY:-}" ]]; then
    printf '%s\n' "$OPENSUBTITLES_API_KEY" > "$SRC/opensubtitles-api-key.txt"
fi

cat > "$SRC/debian/changelog" <<CHANGELOG
top-player (${DEB_VERSION}) ${CODENAME}; urgency=medium

  * Top Player ${VERSION} for ${PRETTY_NAME:-$DISTRO}.
    See https://github.com/Henok-Enyew/top-player-linux/releases

 -- Henok Enyew Andargie <noreply@github.com>  $(date -R)
CHANGELOG

cd "$SRC"
if [[ "$(id -u)" == 0 ]] && command -v mk-build-deps >/dev/null; then
    mk-build-deps --install --remove \
        --tool 'apt-get -y --no-install-recommends' debian/control
fi
dpkg-buildpackage -b -us -uc

mkdir -p "$ROOT/dist"
cp "$WORK"/top-player_"${DEB_VERSION}"_*.deb "$ROOT/dist/"
ls -l "$ROOT"/dist/*.deb
