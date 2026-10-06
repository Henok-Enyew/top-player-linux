#!/usr/bin/env bash
# Builds the Top Player RPM on Fedora or openSUSE. Run from the repository root:
#
#   VERSION=1.0.6 packaging/rpm/build-rpm.sh
#
# As root (as in CI) this installs the build dependencies first; otherwise
# install the BuildRequires from packaging/rpm/top-player.spec yourself.
set -euo pipefail

ROOT="$(pwd)"
VERSION="${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}"
VERSION="${VERSION#v}"
# RPM versions cannot contain '-'; '~' sorts before the release.
RPM_VERSION="${VERSION//-/\~}"

. /etc/os-release
# openSUSE leaves %{dist} empty; give the file names a distro tag so
# Tumbleweed and Leap packages can sit side by side in one release.
DIST_DEFINE=()
if [[ -z "$(rpm --eval '%{?dist}')" ]]; then
    case "$ID" in
        opensuse-tumbleweed) DIST_DEFINE=(--define "dist .tw") ;;
        opensuse-leap)       DIST_DEFINE=(--define "dist .lp${VERSION_ID//./}") ;;
        *)                   DIST_DEFINE=(--define "dist .${ID//-/_}") ;;
    esac
fi

TOPDIR="${TOPDIR:-$ROOT/rpmbuild}"
mkdir -p "$TOPDIR/SOURCES" "$TOPDIR/SPECS"
SPEC="$TOPDIR/SPECS/top-player.spec"
sed "s/^Version:.*/Version:        ${RPM_VERSION}/" "$ROOT/packaging/rpm/top-player.spec" > "$SPEC"

STAGE="$TOPDIR/stage"
mkdir -p "$STAGE/top-player-${RPM_VERSION}"
git -c safe.directory="$ROOT" -C "$ROOT" archive --format=tar HEAD | tar -x -C "$STAGE/top-player-${RPM_VERSION}"
# Pass the optional OpenSubtitles key through the untracked file CMake reads.
if [[ -n "${OPENSUBTITLES_API_KEY:-}" ]]; then
    printf '%s\n' "$OPENSUBTITLES_API_KEY" > "$STAGE/top-player-${RPM_VERSION}/opensubtitles-api-key.txt"
fi
tar -czf "$TOPDIR/SOURCES/top-player-${RPM_VERSION}.tar.gz" -C "$STAGE" "top-player-${RPM_VERSION}"

if [[ "$(id -u)" == 0 ]]; then
    if command -v dnf >/dev/null; then
        dnf builddep -y "$SPEC"
    elif command -v zypper >/dev/null; then
        # zypper has no builddep: install the BuildRequires lines.
        mapfile -t deps < <(rpmspec -q --buildrequires "${DIST_DEFINE[@]}" "$SPEC" | sed 's/ .*//')
        zypper --non-interactive install --no-recommends "${deps[@]}"
    fi
fi

rpmbuild -ba --define "_topdir $TOPDIR" "${DIST_DEFINE[@]}" "$SPEC"

mkdir -p "$ROOT/dist"
cp "$TOPDIR"/RPMS/*/top-player-[0-9]*.rpm "$ROOT/dist/"
if [[ "${INCLUDE_SRPM:-0}" == 1 ]]; then
    cp "$TOPDIR"/SRPMS/*.src.rpm "$ROOT/dist/"
fi
ls -l "$ROOT"/dist/*.rpm
