#!/usr/bin/env bash
# fastcharge/build.sh -- build the fastcharge arm64 deb.
#
# Contents of the deb (userspace ONLY -- the charge_boost_lite module is
# shipped by the kernel build's modules package; bundling it here would
# conflict with it):
#   /usr/sbin/fastcharged                     the C daemon
#   /etc/fastcharge/fastcharge.conf           profile (conffile)
#   /usr/lib/systemd/system/fastcharged.service + enable symlink
#   /etc/modprobe.d + /etc/modules-load.d     load charge_boost_lite (apply=0)
#
# Everything compiles inside a one-shot stock docker.io/library/ubuntu:26.04
# linux/arm64 container (qemu on x86_64 hosts); apt cache under
# ../fastcharge/apt-cache.
#
# Usage: ./build.sh [DEB_OUT_DIR]   (or OUT=... ./build.sh)
#   DEB_OUT_DIR defaults to ../../build/userspace/debs; the deb lands there
#   as fastcharge_<version>_arm64.deb.
set -euo pipefail

SCRIPTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
META="${SCRIPTDIR}"
OUT="${OUT:-${SCRIPTDIR}/../../../build/userspace/debs}"

BASE="${BASE_IMAGE:-docker.io/library/ubuntu:26.04}"

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) OUT="$1" ;;
    esac
    shift
done
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

log() { printf '[fastcharge-build] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

command -v podman >/dev/null 2>&1 || die "podman not found"
[ -f "${META}/src/fastcharged.c" ] || die "missing src/fastcharged.c"
[ -f "${META}/debian/control" ] || die "missing debian/control"

VER="$(awk '/^Version:/{print $2}' "${META}/debian/control")"
[ -n "${VER}" ] || die "cannot parse Version from debian/control"
DEB_NAME="fastcharge_${VER}_arm64.deb"

CACHE="${SCRIPTDIR}/../../../build/userspace/fastcharge/apt-cache"
STAGE="${SCRIPTDIR}/../../../build/userspace/fastcharge/staging"
mkdir -p "${CACHE}/archives/partial" "${CACHE}/lists/partial" "${STAGE}"

log "building in a one-shot stock ${BASE} (linux/arm64) container"
podman run --rm \
    --platform linux/arm64 \
    -e DEB_NAME="${DEB_NAME}" \
    -v "${META}:/src:ro" \
    -v "${STAGE}:/stage" \
    -v "${CACHE}/archives:/var/cache/apt/archives" \
    -v "${CACHE}/lists:/var/lib/apt/lists" \
    --entrypoint /bin/bash \
    "${BASE}" -euo pipefail -c '
# ---- inside the one-shot arm64 container ---------------------------------
[ "$(uname -m)" = aarch64 ] || { echo "not aarch64" >&2; exit 1; }
DEBIAN_FRONTEND=noninteractive
export DEBIAN_FRONTEND LC_ALL=C TZ=UTC

apt-get update
apt-get install -y --no-install-recommends gcc libc6-dev binutils

# ---- compile + self-test the daemon ---------------------------------------
rm -rf /stage/pkg
mkdir -p /stage/pkg/usr/sbin
cc -O2 -Wall -Wextra -o /stage/pkg/usr/sbin/fastcharged /src/src/fastcharged.c
strip /stage/pkg/usr/sbin/fastcharged

# acceptance: the shipped profile must parse and cover the whole domain
/stage/pkg/usr/sbin/fastcharged -t -c /src/rootfs/etc/fastcharge/fastcharge.conf \
    > /stage/profile-dump.txt
echo "[fastcharge-build] profile self-test ok:"
sed "s/^/    /" /stage/profile-dump.txt

# ---- assemble the package ---------------------------------------------------
cp -a /src/rootfs/. /stage/pkg/
mkdir -p /stage/pkg/etc/systemd/system/multi-user.target.wants
ln -sfn ../../../usr/lib/systemd/system/fastcharged.service \
    /stage/pkg/etc/systemd/system/multi-user.target.wants/fastcharged.service

mkdir -p /stage/pkg/DEBIAN
cp /src/debian/control /stage/pkg/DEBIAN/control
cp /src/debian/conffiles /stage/pkg/DEBIAN/conffiles
chmod 0644 /stage/pkg/DEBIAN/control /stage/pkg/DEBIAN/conffiles
chmod 0755 /stage/pkg/usr/sbin/fastcharged

dpkg-deb --root-owner-group --build /stage/pkg "/stage/${DEB_NAME}"
dpkg-deb -I "/stage/${DEB_NAME}" | head -15
'

cp -f "${STAGE}/${DEB_NAME}" "${OUT}/${DEB_NAME}"
log "built ${OUT}/${DEB_NAME}"
sha256sum "${OUT}/${DEB_NAME}" | tee "${OUT}/${DEB_NAME}.sha256"
