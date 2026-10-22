#!/usr/bin/env bash
# Builds the flocate .deb on the Debian/Ubuntu system this runs on and drops
# it into dist/ as flocate_<version>_<distro>_<arch>.deb (distro = debian12,
# ubuntu2404, ...). Used directly on a Debian host, by build-deb.sh inside
# each target's container, and by the GitHub workflow.
#
#   INSTALL_DEPS=1     install the build dependencies first (needs root)
#   FLOCATE_RELEASE=1  no snapshot suffix even when HEAD is not the release tag
#   DEB_BUILD_OPTIONS  passed through (e.g. nocheck to skip the test suite)
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD

if [ "${INSTALL_DEPS:-0}" = 1 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        build-essential dpkg-dev git ca-certificates lintian >/dev/null
    apt-get build-dep -y -qq "$REPO" >/dev/null
fi

. /etc/os-release
DISTRO_ID="${ID}${VERSION_ID//./}"
ARCH=$(dpkg --print-architecture)

# meson.build is authoritative for the version; debian/changelog must agree
# (Debian spells the pre-release suffix with "~" so that it sorts before the
# release).
VERSION=$(sed -n "s/^project(.*version: '\([^']*\)'.*/\1/p" meson.build)
DEB_VERSION=${VERSION//-/"~"}
CHANGELOG_VERSION=$(dpkg-parsechangelog -S Version)
if [ "$CHANGELOG_VERSION" != "$DEB_VERSION" ]; then
    echo "version mismatch: meson.build says $VERSION ($DEB_VERSION), debian/changelog says $CHANGELOG_VERSION" >&2
    exit 1
fi

# Anything that is not the tagged release commit gets a snapshot suffix.
SNAPSHOT=""
if [ "${FLOCATE_RELEASE:-0}" != 1 ] && git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then
    HEAD_SHA=$(git -C "$REPO" rev-parse --short=7 HEAD)
    if ! git -C "$REPO" describe --tags --exact-match HEAD 2>/dev/null | grep -qx "v$VERSION"; then
        SNAPSHOT="+git$(date -u +%Y%m%d).$HEAD_SHA"
    fi
fi

# Build from a copy, so dpkg-buildpackage's output lands next to the copy
# instead of next to the checkout, and the changelog edit stays out of it.
BUILD_ROOT=$(mktemp -d)
trap 'rm -rf "$BUILD_ROOT"' EXIT
SRC="$BUILD_ROOT/flocate-$DEB_VERSION"
mkdir "$SRC"
tar -C "$REPO" --exclude=./.git --exclude=./build --exclude=./dist -cf - . | tar -C "$SRC" -xf -

if [ -n "$SNAPSHOT" ]; then
    MAINT=$(dpkg-parsechangelog -l "$SRC/debian/changelog" -S Maintainer)
    {
        printf 'flocate (%s%s) UNRELEASED; urgency=medium\n\n' "$DEB_VERSION" "$SNAPSHOT"
        printf '  * Snapshot build of commit %s.\n\n' "$HEAD_SHA"
        printf ' -- %s  %s\n\n' "$MAINT" "$(date -R)"
        cat "$SRC/debian/changelog"
    } > "$SRC/debian/changelog.new"
    mv "$SRC/debian/changelog.new" "$SRC/debian/changelog"
fi

(cd "$SRC" && dpkg-buildpackage -us -uc -b -jauto)

if command -v lintian >/dev/null 2>&1; then
    lintian --fail-on error "$BUILD_ROOT"/flocate_*.changes
fi

# The file name uses the meson spelling of the version (1.2.1-dev, not
# 1.2.1~dev): GitHub rewrites "~" in release asset names, which would make
# the published names differ from SHA256SUMS. dpkg does not care about it.
mkdir -p "$REPO/dist"
OUT="$REPO/dist/flocate_${VERSION}${SNAPSHOT}_${DISTRO_ID}_${ARCH}.deb"
cp "$BUILD_ROOT"/flocate_*.deb "$OUT"
echo "-> $OUT"
