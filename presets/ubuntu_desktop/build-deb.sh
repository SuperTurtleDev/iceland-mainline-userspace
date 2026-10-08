#!/usr/bin/env bash
# ubuntu_desktop preset deb builder: SENSORS, from source, automatically.
#
# Called by ../../build.sh (DEB_OUT=WORK/debs) before the rootfs stages.
# Replays the sensor workspace's on-device build (sensor-pkgs/build-all.sh)
# inside a one-shot stock ubuntu:26.04 arm64 container instead:
#   1. verify the pinned inputs (upstream/ + foreign/ SHA256SUMS)
#   2. install build deps incl. the pinned foreign debs (libssc, fastrpc)
#   3. dpkg-buildpackage iceland-sensors (native arm64)
#   4. dpkg-source -x the pinned iio-sensor-proxy .dsc, apply the ssc
#      claim-race patch, the single-line Build-Depends rewrite, compat 13
#      and the 3.9-1iceland1 changelog entry (verbatim ports), build
#   5. collect iceland-sensors_*.deb + iio-sensor-proxy_*iceland*.deb and
#      the runtime foreign debs into DEB_OUT
#
# Source of truth: the sensor workspace, SENSOR_SRC (default
# /home/wyb/Documents/sensor/sensor-pkgs). It lives outside this repo on
# purpose -- point SENSOR_SRC=... elsewhere to move it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEB_OUT="${DEB_OUT:?set by build.sh}"
SENSOR_SRC="${SENSOR_SRC:-/home/wyb/Documents/sensor/sensor-pkgs}"
BASE="${BASE_IMAGE:-docker.io/library/ubuntu:26.04}"

log() { printf '[sensors-build] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

command -v podman >/dev/null 2>&1 || die "podman not found"
[ -d "${SENSOR_SRC}/iceland-sensors" ] || die "sensor sources not found at ${SENSOR_SRC} (SENSOR_SRC=... to override)"
[ -f "${SENSOR_SRC}/iio-sensor-proxy-overlay/0001-ssc-claim-race.patch" ] || die "ssc patch missing"
[ -f "${SENSOR_SRC}/upstream/SHA256SUMS" ] || die "upstream pins missing"
[ -f "${SENSOR_SRC}/foreign/SHA256SUMS" ] || die "foreign pins missing"

# pin verification happens on the host: the container only ever sees the
# exact files that passed the sums
( cd "${SENSOR_SRC}/upstream" && sha256sum -c SHA256SUMS >/dev/null ) || die "upstream pin mismatch"
( cd "${SENSOR_SRC}/foreign" && sha256sum -c SHA256SUMS >/dev/null ) || die "foreign pin mismatch"
log "pins verified (upstream + foreign)"

STAGE="$(dirname "${DEB_OUT}")/sensors-build/staging"
CACHE="$(dirname "${DEB_OUT}")/sensors-build/apt-cache"
mkdir -p "${STAGE}" "${CACHE}/archives/partial" "${CACHE}/lists/partial" "${DEB_OUT}"

log "building in a one-shot stock ${BASE} (linux/arm64) container"
podman run --rm \
    --platform linux/arm64 \
    -v "${SENSOR_SRC}:/src:ro" \
    -v "${STAGE}:/stage" \
    -v "${CACHE}/archives:/var/cache/apt/archives" \
    -v "${CACHE}/lists:/var/lib/apt/lists" \
    --entrypoint /bin/bash \
    "${BASE}" -euo pipefail -c '
# ---- inside the one-shot arm64 container -----------------------------------
[ "$(uname -m)" = aarch64 ] || { echo "not aarch64" >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
B=/src; S=/stage

apt-get update
apt-get install -y --no-install-recommends build-essential fakeroot \
    debhelper dpkg-dev meson ninja-build pkg-config libglib2.0-dev \
    libgudev-1.0-dev libpolkit-gobject-1-dev libudev-dev systemd-dev \
    /src/foreign/libssc-dev_*_arm64.deb /src/foreign/gir1.2-ssc-2_*_arm64.deb \
    /src/foreign/libssc2_*_arm64.deb /src/foreign/libssc-bin_*_arm64.deb \
    /src/foreign/fastrpc-support_*_arm64.deb \
    /src/foreign/libfastrpc1_*_arm64.deb >/dev/null

echo "== build iceland-sensors (native arm64)"
rm -rf /tmp/iceland-sensors
cp -a "${B}/iceland-sensors" /tmp/iceland-sensors
cd /tmp/iceland-sensors
dpkg-buildpackage -us -uc -b 2>&1 | tail -2
mv /tmp/iceland-sensors_*_all.deb "${S}/"

echo "== unpack + patch + build iio-sensor-proxy 3.9-1iceland1"
cd /tmp
rm -rf isp
dpkg-source -x "${B}/upstream/iio-sensor-proxy_3.9-1.dsc" isp >/dev/null
mkdir -p isp/debian/patches
cp "${B}/iio-sensor-proxy-overlay/0001-ssc-claim-race.patch" isp/debian/patches/
grep -qx 0001-ssc-claim-race.patch isp/debian/patches/series \
    || echo 0001-ssc-claim-race.patch >> isp/debian/patches/series
# relax Debian pins for an Ubuntu 26.04 native build; single-line stanza
# without build profiles (multiline trips dpkg-checkbuilddeps here)
python3 - <<PY
import re
p = "isp/debian/control"
s = open(p).read()
new = ("Build-Depends: debhelper (>= 13), libgudev-1.0-dev, meson, "
       "ninja-build, pkg-config, libglib2.0-dev, libpolkit-gobject-1-dev, "
       "libssc-dev (>= 0.4.0)")
s = re.sub(r"Build-Depends:.*?(?=\n\S)", new, s, count=1, flags=re.S)
open(p, "w").write(s)
PY
echo 13 > isp/debian/compat
grep -q "3.9-1iceland1" isp/debian/changelog || \
sed -i "1i iio-sensor-proxy (3.9-1iceland1) UNRELEASED; urgency=medium\n\n  * Add ssc-claim-race.patch from piano-sensors: start polling for\n    clients that claimed while an SSC driver was still opening.\n\n -- Iceland mainline <dev@localhost>  Wed, 07 Oct 2026 09:00:00 +0000\n" isp/debian/changelog
cd isp
dpkg-checkbuilddeps
dpkg-buildpackage -us -uc -b 2>&1 | tail -2
mv /tmp/iio-sensor-proxy_*_arm64.deb "${S}/"
echo "== container build done"
'

# runtime deb set: the two freshly built ones + the pinned foreign runtime
# debs (libssc-dev / gir are build-time only and stay out of the image)
rm -f "${DEB_OUT}"/iceland-sensors_*.deb "${DEB_OUT}"/iio-sensor-proxy_*iceland*.deb \
      "${DEB_OUT}"/libssc2_* "${DEB_OUT}"/libssc-bin_* "${DEB_OUT}"/fastrpc-support_* "${DEB_OUT}"/libfastrpc1_*
cp -f "${STAGE}"/iceland-sensors_*_all.deb "${STAGE}"/iio-sensor-proxy_*iceland*_arm64.deb "${DEB_OUT}/"
cp -f "${SENSOR_SRC}"/foreign/libssc2_*_arm64.deb "${SENSOR_SRC}"/foreign/libssc-bin_*_arm64.deb \
      "${SENSOR_SRC}"/foreign/fastrpc-support_*_arm64.deb "${SENSOR_SRC}"/foreign/libfastrpc1_*_arm64.deb \
      "${DEB_OUT}/"
( cd "${DEB_OUT}" && sha256sum iceland-sensors_*.deb iio-sensor-proxy_*iceland*.deb \
    libssc2_* libssc-bin_* fastrpc-support_* libfastrpc1_* | tee sensors-debs.SHA256SUMS )
log "done: $(ls "${DEB_OUT}" | grep -cE "iceland-sensors_|iio-sensor-proxy_|libssc|fastrpc") debs in ${DEB_OUT}"
