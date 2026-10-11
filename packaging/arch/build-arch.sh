#!/usr/bin/env bash
# Builds top-player-<version>-<rel>-<arch>.pkg.tar.zst on Arch Linux.
# Run from the repository root:
#
#   VERSION=1.0.8 packaging/arch/build-arch.sh
#
# makepkg refuses to run as root. As root (as in CI) this script installs the
# build dependencies and runs makepkg as an unprivileged "builder" user.
set -euo pipefail

ROOT="$(pwd)"
VERSION="${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}"
VERSION="${VERSION#v}"
# pkgver cannot contain '-'.
PKGVER="${VERSION//-/_}"

WORK="${WORK:-$ROOT/build-arch}"
mkdir -p "$WORK"
cp "$ROOT/packaging/arch/PKGBUILD" "$WORK/PKGBUILD"
sed -i "s/^pkgver=.*/pkgver=${PKGVER}/" "$WORK/PKGBUILD"

STAGE="$WORK/stage/top-player-${PKGVER}"
mkdir -p "$STAGE"
git -c safe.directory="$ROOT" -C "$ROOT" archive --format=tar HEAD | tar -x -C "$STAGE"
# Pass the optional OpenSubtitles key through the untracked file CMake reads.
if [[ -n "${OPENSUBTITLES_API_KEY:-}" ]]; then
    printf '%s\n' "$OPENSUBTITLES_API_KEY" > "$STAGE/opensubtitles-api-key.txt"
fi
tar -czf "$WORK/top-player-${PKGVER}.tar.gz" -C "$WORK/stage" "top-player-${PKGVER}"

cd "$WORK"
if [[ "$(id -u)" == 0 ]]; then
    pacman -Syu --noconfirm --needed base-devel sudo
    id builder >/dev/null 2>&1 || useradd -m builder
    echo 'builder ALL=(ALL) NOPASSWD: /usr/bin/pacman' > /etc/sudoers.d/builder
    chown -R builder "$WORK"
    sudo -u builder makepkg --syncdeps --noconfirm --cleanbuild --force
else
    makepkg --syncdeps --noconfirm --cleanbuild --force
fi

mkdir -p "$ROOT/dist"
cp "$WORK"/top-player-"${PKGVER}"-*.pkg.tar.zst "$ROOT/dist/"
ls -l "$ROOT"/dist/*.pkg.tar.zst
