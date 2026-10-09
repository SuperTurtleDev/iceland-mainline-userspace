#!/usr/bin/env bash
# keymaster preset deb builder: km-init, from source, automatically.
#
# Called by ../../build.sh (DEB_OUT=WORK/debs) before the rootfs stages.
# Just delegates to the component's build.sh (one-shot stock ubuntu:26.04
# arm64 container): compiles km-init against the pinned quic-teec
# (libqcomtee) sources, self-tests the shipped values.conf, and packages
# the deb with the stock keymaster TA image.
#
# KM_FW_DIR (default <repo>/repos/iceland-fw) selects the keymaster.img
# source, pinned by SHA256 inside the component build.sh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEB_OUT="${DEB_OUT:?set by build.sh}"

exec "${HERE}/../../keymaster/build.sh" "${DEB_OUT}"
