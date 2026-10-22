#!/usr/bin/env bash
# Installs each distro's own package from dist/ (see build-deb.sh) into a
# fresh container of that distro and runs packaging/smoke-test.sh in it.
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

for id in "${TARGET_IDS[@]}"; do
    img="${TARGET_IMAGES[$id]}"
    PKG=$(ls dist/flocate_*_"${id}"_*.deb 2>/dev/null | head -1)
    if [ -z "$PKG" ]; then
        echo "no package for $id in dist/ - run packaging/build-deb.sh first" >&2
        exit 1
    fi
    echo "=========================================================="
    echo "== $id ($img) <- $PKG"
    echo "=========================================================="
    "$RUNTIME" run --rm \
        -v "$PWD/$PKG:/pkg/$(basename "$PKG"):Z,ro" \
        -v "$PWD/packaging/smoke-test.sh:/smoke-test.sh:Z,ro" \
        "$img" \
        bash /smoke-test.sh "/pkg/$(basename "$PKG")"
    echo "== $id: PASS"
    echo
done

echo "All targets passed."
