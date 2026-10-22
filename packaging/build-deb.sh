#!/usr/bin/env bash
# Builds one .deb per target distro, natively in that distro's own container,
# into dist/. Each package declares the glibc/libzstd/liburing versions of the
# distro it was built on, which is why there is one per release rather than a
# single "Debian-based" package.
set -euo pipefail
cd "$(dirname "$0")/.."

RUNTIME=docker
command -v docker >/dev/null 2>&1 || RUNTIME=podman

TARGET_IDS=(debian12 debian13 ubuntu2404 ubuntu2604)
declare -A TARGET_IMAGES=(
    [debian12]=debian:12-slim
    [debian13]=debian:13-slim
    [ubuntu2404]=ubuntu:24.04
    [ubuntu2604]=ubuntu:26.04
)

mkdir -p dist
rm -f dist/flocate_*.deb

for id in "${TARGET_IDS[@]}"; do
    img="${TARGET_IMAGES[$id]}"
    echo "=========================================================="
    echo "== Building for $id ($img)"
    echo "=========================================================="
    "$RUNTIME" run --rm \
        -v "$PWD:/work:Z" \
        -w /work \
        -e INSTALL_DEPS=1 \
        -e FLOCATE_RELEASE="${FLOCATE_RELEASE:-0}" \
        -e DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-}" \
        "$img" \
        bash -euc 'git config --global --add safe.directory /work; packaging/build-deb-native.sh'
done

echo
echo "Built packages:"
ls -la dist/
