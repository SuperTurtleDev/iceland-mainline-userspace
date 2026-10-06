#!/usr/bin/env bash
# make-image.sh -- stage 2: pack WORK/tree into WORK/rootfs[-p1-p2...].img
# (+ Android sparse .sparse.img).
#
# Usage (invoked by ../build.sh, as root, with WORK/ROOTFS_* in the env).
#
# Runs ON THE HOST: plain filesystem work, no cross-compilation involved.
# The tree is root-owned (debootstrap output), so mkfs.ext4 -d records the
# true per-file ownership straight into the ext4 inodes. Produces, directly
# in WORK (= BUILD_DIR/userspace):
#   ${ROOTFS_NAME}.img        bare ext4 filesystem image (no partition
#                             table), sparse on disk, deterministic UUID
#                             and dir-hash seed (derived by build.sh)
#   ${ROOTFS_NAME}.sparse.img Android sparse image of the same filesystem
#                             (img2simg, zero blocks -> dont-care chunks)
#                             for fastboot-style flashing; verified by
#                             simg2img round-trip against the raw image
set -euo pipefail

WORK="${WORK:?set by build.sh}"
ROOTFS_SIZE="${ROOTFS_SIZE:?set by build.sh}"
ROOTFS_LABEL="${ROOTFS_LABEL:?set by build.sh}"
ROOTFS_UUID="${ROOTFS_UUID:?set by build.sh}"
ROOTFS_NAME="${ROOTFS_NAME:-rootfs}"
SUDOUID="${SUDO_UID:-1000}"
IMG="${WORK}/${ROOTFS_NAME}.img"
SIMG="${WORK}/${ROOTFS_NAME}.sparse.img"

log() { printf '[make-image] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root (build.sh re-execs under sudo)"
[ -d "${WORK}/tree" ] || die "${WORK}/tree missing (build-tree stage did not run?)"
[ -f "${WORK}/tree/usr/sbin/update-initramfs" ] || die "update-initramfs missing from tree"
for c in mkfs.ext4 dumpe2fs debugfs img2simg simg2img; do
    command -v "$c" >/dev/null 2>&1 || die "$c not found on host"
done

log "mkfs.ext4 -> ${IMG} (size=${ROOTFS_SIZE} label=${ROOTFS_LABEL} uuid=${ROOTFS_UUID})"
rm -f "${IMG}" "${SIMG}"
mkfs.ext4 -q -F \
    -L "${ROOTFS_LABEL}" \
    -U "${ROOTFS_UUID}" \
    -E "hash_seed=${ROOTFS_UUID}" \
    -m 1 \
    -d "${WORK}/tree" \
    "${IMG}" "${ROOTFS_SIZE}"
# best effort: turn unallocated ranges back into holes
fallocate --dig-holes "${IMG}" 2>/dev/null || true

echo "--- dumpe2fs -h"
dumpe2fs -h "${IMG}" 2>/dev/null

echo "--- debugfs: /usr/lib/os-release (/etc/os-release is a symlink to it)"
debugfs -R "cat /usr/lib/os-release" "${IMG}" 2>/dev/null

echo "--- debugfs: initramfs tooling visible in the image"
# on Ubuntu 26.04 both live in /usr/sbin
debugfs -R "ls -l /usr/sbin" "${IMG}" 2>/dev/null | grep -q update-initramfs \
    || { echo "ERROR: /usr/sbin/update-initramfs not found in image" >&2; exit 1; }
debugfs -R "ls -l /usr/sbin" "${IMG}" 2>/dev/null | grep -q mkinitramfs \
    || { echo "ERROR: /usr/sbin/mkinitramfs not found in image" >&2; exit 1; }

echo "--- debugfs: /boot and /usr/lib/modules (both must be empty: no kernel)"
debugfs -R "ls /boot" "${IMG}" 2>/dev/null
debugfs -R "ls /usr/lib/modules" "${IMG}" 2>/dev/null

echo "--- Android sparse image (zero blocks -> dont-care chunks)"
img2simg "${IMG}" "${SIMG}"
# round-trip check: the unsparsified image must be byte-identical
TMP="${WORK}/.${ROOTFS_NAME}.sparse-check.tmp"
simg2img "${SIMG}" "${TMP}"
if ! cmp "${IMG}" "${TMP}"; then
    rm -f "${TMP}"
    die "sparse round-trip mismatch"
fi
rm -f "${TMP}"

chown "${SUDOUID}" "${IMG}" "${SIMG}"
log "done: ${IMG} + ${SIMG}"
du -h --apparent-size "${IMG}" "${SIMG}"
du -h "${IMG}" "${SIMG}"
