#!/usr/bin/env bash
# Build the pen bridge deb in a stock Ubuntu ARM64 container.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${DEB_OUT:-${1:-$HERE/../../../build/userspace/debs}}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=${PENCIL_BUILD_DIR:-$(dirname "$OUT")/pencil-build}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

podman run --rm --platform linux/arm64 \
    -v "$HERE:/src:ro" -v "$WORK:/build" \
    --entrypoint /bin/bash "${BASE_IMAGE:-docker.io/library/ubuntu:26.04}" \
    -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
apt-get update
apt-get install -y --no-install-recommends build-essential debhelper \
    pkg-config libglib2.0-dev
test "$(dpkg --print-architecture)" = arm64
rm -rf /build/source
mkdir -p /build/source
cp -a /src/. /build/source/
cd /build/source
make clean
dpkg-buildpackage -us -uc -b
'
VERSION=$(dpkg-parsechangelog -l "$HERE/debian/changelog" -SVersion)
cp "$WORK/iceland-pencild_${VERSION}_arm64.deb" "$OUT/"
printf 'Built %s/iceland-pencild_%s_arm64.deb\n' "$OUT" "$VERSION"
