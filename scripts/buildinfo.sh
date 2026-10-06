#!/usr/bin/env bash
# buildinfo.sh -- record how WORK/rootfs[-p1-p2...].img was built.
#
# Usage (invoked by ../build.sh with WORK/OUT/META/BASE_IMAGE/ROOTFS_* in
# the env); host-side, file/git-level work only. Writes WORK/buildinfo.txt.
set -euo pipefail

WORK="${WORK:?set by build.sh}"
OUT="${OUT:?set by build.sh}"
META="${META:?set by build.sh}"
SUITE="${SUITE:?set by build.sh}"
MIRROR="${MIRROR:?set by build.sh}"
ROOTFS_SIZE="${ROOTFS_SIZE:?set by build.sh}"
ROOTFS_LABEL="${ROOTFS_LABEL:?set by build.sh}"
ROOTFS_UUID="${ROOTFS_UUID:?set by build.sh}"
ROOTFS_NAME="${ROOTFS_NAME:-rootfs}"
PRESENTS="${PRESENTS:-}"
IMG="${WORK}/${ROOTFS_NAME}.img"
SIMG="${WORK}/${ROOTFS_NAME}.sparse.img"

[ -s "${IMG}" ] || { echo "buildinfo.sh: ${IMG} missing" >&2; exit 1; }

# Identity of this source tree: git HEAD (+ dirty suffix) when it is a
# repo, otherwise a hash of the source files themselves (the tree may not
# be a git repo yet).
repo_identity() {
    local head status_hash
    if ! git -C "${META}" rev-parse --git-dir >/dev/null 2>&1; then
        head="$(cd "${META}" && find . -type f ! -path './.git/*' -print0 \
            | LC_ALL=C sort -z | xargs -0 -r sha256sum | sha256sum | cut -d' ' -f1)"
        printf 'content-%s\n' "${head:0:12}"
        return
    fi
    head="$(git -C "${META}" rev-parse HEAD)"
    if [ -n "$(git -C "${META}" status --porcelain 2>/dev/null)" ]; then
        status_hash="$(git -C "${META}" status --porcelain 2>/dev/null | LC_ALL=C sort \
            | sha256sum | cut -c1-12)"
        printf '%s-dirty-%s\n' "${head}" "${status_hash}"
    else
        printf '%s\n' "${head}"
    fi
}

DIGEST="$(debootstrap --version 2>/dev/null || printf 'unknown')"
TREE_ARCH="$(cat "${WORK}/staging/tree-arch.txt" 2>/dev/null || printf 'unknown')"
HOST_ARCH="$(uname -m)"
EMU="chroot via qemu-aarch64 (binfmt_misc)"

{
    echo "build-time-utc: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo
    echo "rootfs-image: ${IMG}"
    echo "rootfs-bytes: $(stat -c%s "${IMG}")"
    echo "rootfs-bytes-on-disk: $(du -B1 "${IMG}" | cut -f1)"
    echo "rootfs-sha256: $(sha256sum "${IMG}" | cut -d' ' -f1)"
    echo "rootfs-size-arg: ${ROOTFS_SIZE}"
    echo "rootfs-label: ${ROOTFS_LABEL}"
    echo "rootfs-name: ${ROOTFS_NAME}"
    echo "presents: ${PRESENTS:-none}"
    echo "rootfs-uuid: ${ROOTFS_UUID}"
    if [ -f "${SIMG}" ]; then
        echo "sparse-image: ${SIMG}"
        echo "sparse-bytes: $(stat -c%s "${SIMG}")"
        echo "sparse-sha256: $(sha256sum "${SIMG}" | cut -d' ' -f1)"
        echo "sparse-format: Android sparse (img2simg, zero blocks -> don't-care)"
    fi
    echo
    echo "suite: ${SUITE}"
    echo "mirror: ${MIRROR}"
    echo "bootstrap: debootstrap ${DIGEST} (--foreign + chroot second stage)"
    echo "tree-arch: ${TREE_ARCH}"
    echo "host-arch: ${HOST_ARCH}"
    echo "arm64-execution: ${EMU}"
    echo
    echo "packages-list: ${META}/packages.list"
    echo "packages-list-sha256: $(sha256sum "${META}/packages.list" | cut -d' ' -f1)"
    echo "userspace-source: $(repo_identity)"
    echo "podman: $(podman --version)"
    echo
    echo "# installed packages (dpkg-query -W from the arm64 build container)"
    cat "${WORK}/staging/dpkg-packages.txt"
} > "${WORK}/buildinfo.txt"

echo "[buildinfo] wrote ${WORK}/buildinfo.txt" >&2
