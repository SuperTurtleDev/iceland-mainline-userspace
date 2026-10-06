#!/usr/bin/env bash
# Host-side entry point for the sm8850 root filesystem image build.
#
# Produces Ubuntu (26.04 / resolute) arm64 root filesystem images: a bare
# ext4 image (no partition table) of a bootable base system -- systemd as
# PID 1 plus the initramfs tooling (update-initramfs / mkinitramfs /
# initramfs-tools / busybox-initramfs / kmod ...) and the network debugging
# stack (iproute2, network-manager/nmtui, iw, rfkill) -- plus an Android
# sparse copy for fastboot-style flashing. NO kernel: kernel image, modules
# and firmware come from the separate kernel build. How the image is booted
# (first-boot bootstrap) is handled elsewhere.
#
# The whole build runs ON THE HOST under a single sudo elevation (it is
# root-filesystem work, not a compile job): debootstrap --foreign + chroot
# second stage, with arm64 code executing through the host qemu-aarch64
# binfmt registration. Only actual cross-compilation steps (e.g. the
# fastcharge deb builder) use a one-shot podman container.
#
# Presets -- extra content layered on top of the base -- are selected at
# build time with --addition-presents (repeatable, comma-separated ok):
#
#   ./build.sh --addition-presents debug
#
# A preset lives in presets/<name>/ and may provide packages.list (extra
# packages), packages-recommends.list (installed with recommends),
# apt-preferences/ (apt pins/masks), tree/ (overlay copied verbatim into
# the rootfs), hook.sh (run inside the target chroot), image.conf
# (ROOTFS_SIZE override), debs.list + build-deb.sh (local debs built into
# WORK/debs and installed into the rootfs) and modules.list (EREs of kernel
# modules the preset ships, checked by verify).
# Artifacts are named rootfs-<present1>-<present2>...img (and .sparse.img);
# with no preset selected they stay rootfs.img / rootfs.sparse.img.
#
# Stages (in order, always executed; logs in ${OUT}/userspace/logs):
#   scripts/build-tree.sh   debootstrap + base + presets -> tree/rootfs.tar
#   scripts/make-image.sh   mkfs.ext4 -d tree -> rootfs[-p1-p2...].img
#                           + Android sparse rootfs[-p1-p2...].sparse.img
#   scripts/buildinfo.sh    buildinfo.txt (inputs, package versions)
#   scripts/verify.sh       artifact + content checks, SHA256SUMS
#
# Usage: sudo ./build.sh [BUILD_DIR] [--addition-presents <p>[,<p>...]]
#   (a plain ./build.sh re-execs itself under sudo; or OUT=... ./build.sh)
#   BUILD_DIR defaults to ../../build; images and all intermediates
#   (logs/staging/apt-cache/debs) land under BUILD_DIR/userspace/.
#
# Knobs (environment):
#   SUITE          debootstrap suite            (default: resolute = 26.04)
#   MIRROR         apt mirror, arm64 ports      (default: the official
#                  ports.ubuntu.com; point MIRROR=... at a local mirror for
#                  faster builds, e.g. mirrors.tuna.tsinghua.edu.cn)
#   ROOTFS_SIZE    size argument for mkfs.ext4  (default: 2g, or the largest
#                  ROOTFS_SIZE declared by a selected presets/<p>/image.conf;
#                  an explicit ROOTFS_SIZE here wins)
#   ROOTFS_LABEL   ext4 volume label            (default: rootfs)
#   The ext4 UUID and dir-hash seed are derived deterministically from the
#   suite + size + label + packages.list + the content of every selected
#   preset.
set -euo pipefail

SCRIPTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
META="${SCRIPTDIR}"
OUT="${OUT:-${SCRIPTDIR}/../../build}"
SUITE="${SUITE:-resolute}"
MIRROR="${MIRROR:-http://ports.ubuntu.com/ubuntu-ports}"
ROOTFS_SIZE="${ROOTFS_SIZE:-}"
ROOTFS_LABEL="${ROOTFS_LABEL:-rootfs}"

# ---- single sudo elevation for the whole build --------------------------------
if [ "$(id -u)" -ne 0 ]; then
    exec sudo env \
        "OUT=${OUT}" "SUITE=${SUITE}" "MIRROR=${MIRROR}" \
        "ROOTFS_SIZE=${ROOTFS_SIZE}" "ROOTFS_LABEL=${ROOTFS_LABEL}" \
        "PATH=/usr/sbin:/usr/bin:/sbin:/bin" \
        bash "${BASH_SOURCE[0]}" "$@"
fi
SUDOUID="${SUDO_UID:-$(id -u)}"

STAGES=(build-tree make-image)

log() { printf '[build.sh] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

PRESENTS_RAW=""
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        --addition-presents)
            [ $# -ge 2 ] || die "--addition-presents needs a preset name"
            PRESENTS_RAW="${PRESENTS_RAW:+${PRESENTS_RAW},}$2"
            shift
            ;;
        --addition-presents=*)
            PRESENTS_RAW="${PRESENTS_RAW:+${PRESENTS_RAW},}${1#*=}"
            ;;
        *) OUT="$1" ;;
    esac
    shift
done
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
WORK="${OUT}/userspace"

# ---- preset selection: validate, dedupe (keep order) ------------------------
PRESENTS=()
seen=" "
for n in $(echo "${PRESENTS_RAW}" | tr ',' ' '); do
    [ -n "${n}" ] || continue
    case "${n}" in
        *[!A-Za-z0-9_-]*) die "invalid preset name: ${n}" ;;
    esac
    [ -d "${META}/presets/${n}" ] || die "unknown preset: ${n} (looked in ${META}/presets)"
    case "${seen}" in *" ${n} "*) continue ;; esac
    PRESENTS+=("${n}")
    seen="${seen}${n} "
done
ROOTFS_NAME="rootfs"
if [ "${#PRESENTS[@]}" -gt 0 ]; then
    ROOTFS_NAME="rootfs-$(IFS=-; echo "${PRESENTS[*]}")"
fi
PRESENTS_STR="${PRESENTS[*]:-}"

# ---- preset image knobs (presets/<name>/image.conf, KEY=VALUE) ---------------
size_bytes() {
    case "$1" in
        *[!0-9kKmMgGtT]*) return 1 ;;
    esac
    if [ "$1" -gt 0 ] 2>/dev/null; then printf '%s\n' "$1"; return 0; fi
    local v="${1%[kKmMgGtT]}" u="${1: -1}"
    case "${v}" in ''|*[!0-9]*) return 1 ;; esac
    case "${u}" in
        k|K) echo $((v * 1024)) ;;
        m|M) echo $((v * 1048576)) ;;
        g|G) echo $((v * 1073741824)) ;;
        t|T) echo $((v * 1099511627776)) ;;
        *) return 1 ;;
    esac
}
PRESET_SIZE=""
PRESET_SIZE_B=0
for p in ${PRESENTS_STR}; do
    cfg="${META}/presets/${p}/image.conf"
    [ -f "${cfg}" ] || continue
    v="$(grep -E '^ROOTFS_SIZE=' "${cfg}" | tail -1 | cut -d= -f2- | tr -d '[:space:]')"
    [ -n "${v}" ] || continue
    b="$(size_bytes "${v}")" || die "invalid ROOTFS_SIZE in ${cfg}: ${v}"
    if [ "${b}" -gt "${PRESET_SIZE_B}" ]; then
        PRESET_SIZE_B="${b}"
        PRESET_SIZE="${v}"
    fi
done
if [ -z "${ROOTFS_SIZE}" ]; then
    if [ "${PRESET_SIZE_B}" -gt 0 ]; then ROOTFS_SIZE="${PRESET_SIZE}"; else ROOTFS_SIZE="2g"; fi
fi

[ -f "${META}/packages.list" ] || die "missing ${META}/packages.list"
for s in "${STAGES[@]}"; do
    [ -f "${META}/scripts/${s}.sh" ] || die "missing ${META}/scripts/${s}.sh"
done

# ---- host dependencies ---------------------------------------------------------
need_pkgs=""
dpkg -s debootstrap >/dev/null 2>&1 || need_pkgs="${need_pkgs} debootstrap"
dpkg -s ubuntu-keyring >/dev/null 2>&1 || need_pkgs="${need_pkgs} ubuntu-keyring"
dpkg -s android-sdk-libsparse-utils >/dev/null 2>&1 || need_pkgs="${need_pkgs} android-sdk-libsparse-utils"
dpkg -s e2fsprogs >/dev/null 2>&1 || need_pkgs="${need_pkgs} e2fsprogs"
if [ -n "${need_pkgs}" ]; then
    log "[check] installing host packages:${need_pkgs}"
    apt-get update -qq || true
    apt-get install -y --no-install-recommends ${need_pkgs} >/dev/null
fi
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] \
    || die "binfmt qemu-aarch64 not registered on the host (install qemu-user-static)"

mkdir -p "${WORK}/logs" "${WORK}/staging" "${WORK}/debs" \
    "${WORK}/apt-cache/archives/partial" "${WORK}/apt-cache/lists/partial"

# deterministic ext4 UUID / dir-hash seed from the build inputs
ROOTFS_UUID="$(
    {
        printf 'suite=%s\nmirror=%s\nsize=%s\nlabel=%s\n' \
            "${SUITE}" "${MIRROR}" "${ROOTFS_SIZE}" "${ROOTFS_LABEL}"
        cat "${META}/packages.list"
        for p in ${PRESENTS_STR}; do
            printf 'present=%s\n' "${p}"
            ( cd "${META}/presets/${p}" && find . -type f -print0 | LC_ALL=C sort -z \
                | xargs -0 -r sha256sum | sha256sum )
        done
    } | sha256sum | cut -c1-32
)"
ROOTFS_UUID="${ROOTFS_UUID:0:8}-${ROOTFS_UUID:8:4}-${ROOTFS_UUID:12:4}-${ROOTFS_UUID:16:4}-${ROOTFS_UUID:20:12}"
export ROOTFS_UUID

START="$(date +%s)"
printf 'start-utc %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "${WORK}/staging/last-run.txt"

# ---- preset-provided deb builders --------------------------------------------
for p in ${PRESENTS_STR}; do
    if [ -f "${META}/presets/${p}/build-deb.sh" ]; then
        t0="$(date +%s)"
        log "[${p}] building debs (log: ${WORK}/logs/build-deb-${p}.log)"
        DEB_OUT="${WORK}/debs" bash "${META}/presets/${p}/build-deb.sh" \
            2>&1 | tee "${WORK}/logs/build-deb-${p}.log"
        printf 'build-deb-%s %ss\n' "${p}" "$(( $(date +%s) - t0 ))" >> "${WORK}/staging/last-run.txt"
    fi
done

for stage in "${STAGES[@]}"; do
    t0="$(date +%s)"
    log "[${stage}] RUN (log: ${WORK}/logs/${stage}.log)"
    WORK="${WORK}" OUT="${OUT}" META="${META}" SUITE="${SUITE}" MIRROR="${MIRROR}" \
    ROOTFS_SIZE="${ROOTFS_SIZE}" ROOTFS_LABEL="${ROOTFS_LABEL}" ROOTFS_UUID="${ROOTFS_UUID}" \
    ROOTFS_NAME="${ROOTFS_NAME}" PRESENTS="${PRESENTS_STR}" \
        bash "${META}/scripts/${stage}.sh" 2>&1 | tee "${WORK}/logs/${stage}.log"
    printf '%s %ss\n' "${stage}" "$(( $(date +%s) - t0 ))" >> "${WORK}/staging/last-run.txt"
done

t0="$(date +%s)"
log "generating buildinfo (log: ${WORK}/logs/buildinfo.log)"
WORK="${WORK}" OUT="${OUT}" META="${META}" SUITE="${SUITE}" MIRROR="${MIRROR}" \
ROOTFS_SIZE="${ROOTFS_SIZE}" ROOTFS_LABEL="${ROOTFS_LABEL}" ROOTFS_UUID="${ROOTFS_UUID}" \
ROOTFS_NAME="${ROOTFS_NAME}" PRESENTS="${PRESENTS_STR}" \
    bash "${META}/scripts/buildinfo.sh" > "${WORK}/logs/buildinfo.log" 2>&1
printf 'buildinfo %ss\n' "$(( $(date +%s) - t0 ))" >> "${WORK}/staging/last-run.txt"

t0="$(date +%s)"
log "verifying artifacts (log: ${WORK}/logs/verify.log)"
WORK="${WORK}" OUT="${OUT}" META="${META}" \
ROOTFS_NAME="${ROOTFS_NAME}" PRESENTS="${PRESENTS_STR}" \
    bash "${META}/scripts/verify.sh" 2>&1 | tee "${WORK}/logs/verify.log"
printf 'verify %ss\n' "$(( $(date +%s) - t0 ))" >> "${WORK}/staging/last-run.txt"

log "total wall time: $(( $(date +%s) - START )) s"

log "done. artifacts in ${WORK}:"
ls -l "${WORK}/${ROOTFS_NAME}.img" "${WORK}/${ROOTFS_NAME}.sparse.img" \
    "${WORK}/SHA256SUMS" "${WORK}/buildinfo.txt" >&2
