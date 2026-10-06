#!/usr/bin/env bash
# verify.sh -- artifact checks for the rootfs image build (host-side).
#
# Usage (invoked by ../build.sh with WORK/ROOTFS_NAME/PRESENTS in the env);
# also writes WORK/SHA256SUMS for the WORK/rootfs[-p1-p2...].img artifacts.
#
# Checks:
#   1) rootfs.img exists and is non-empty
#   2) ext4 superblock magic (0x438 == 0xef53)
#   3) no kernel artifacts in the tree: no boot/vmlinuz*, no .ko under
#      lib/modules|usr/lib/modules (the dir itself stays empty by design)
#   4) os-release in the tree is Ubuntu 26.04
#   5) required packages installed (initramfs-tools chain, kmod, cpio)
#   6) NO linux-image/modules/headers packages installed
#   7) update-initramfs + mkinitramfs binaries present in the tree
#   8) rootfs.sparse.img exists with Android sparse magic (0x3AFF26ED)
#   9) SHA256SUMS written and sha256sum -c passes
set -euo pipefail

WORK="${WORK:?set by build.sh}"
META="${META:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ROOTFS_NAME="${ROOTFS_NAME:-rootfs}"
PRESENTS="${PRESENTS:-}"
IMG="${WORK}/${ROOTFS_NAME}.img"
TAR="${WORK}/rootfs.tar"
STAGE="${WORK}/staging"

PASS=0
FAIL=0
ok()  { printf 'PASS: %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf 'FAIL: %s\n' "$1"; FAIL=$((FAIL + 1)); }

# ---- 1) artifact exists ----------------------------------------------------
if [ -s "${IMG}" ]; then
    ok "${ROOTFS_NAME}.img exists ($(numfmt --to=iec "$(stat -c%s "${IMG}")" 2>/dev/null || stat -c%s "${IMG}"))"
else
    bad "${ROOTFS_NAME}.img missing or empty: ${IMG}"
fi

# ---- 2) ext4 magic ----------------------------------------------------------
if python3 - "${IMG}" <<'EOF'
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(0x438)
    sys.exit(0 if f.read(2) == b"\x53\xef" else 1)
EOF
then ok "ext4 superblock magic present"
else bad "ext4 superblock magic missing (not an ext filesystem?)"
fi

# ---- 3/4/7) tree content ----------------------------------------------------
if [ -f "${TAR}" ]; then
    tar -tf "${TAR}" > "${STAGE}/rootfs-tar-listing.txt"
    if grep -Eq '^(\./)?boot/vmlinuz' "${STAGE}/rootfs-tar-listing.txt"; then
        bad "kernel image(s) under /boot"
    else
        ok "no kernel image under /boot"
    fi
    # kernel modules are allowed only when a preset declares them
    # (presets/<p>/modules.list holds EREs matched against the listing)
    KOS="$(grep -E '^(\./)?(usr/)?lib/modules/.*\.ko(\.zst|\.xz)?$' \
        "${STAGE}/rootfs-tar-listing.txt" || true)"
    EXPECT_RE=""
    for p in ${PRESENTS}; do
        f="${META}/presets/${p}/modules.list"
        [ -f "${f}" ] || continue
        EXPECT_RE="${EXPECT_RE:+${EXPECT_RE}|}$(grep -E -v '^[[:space:]]*(#|$)' "${f}" | paste -sd'|')"
    done
    if [ -z "${KOS}" ]; then
        ok "/lib/modules empty (no kernel modules)"
    elif [ -z "${EXPECT_RE}" ]; then
        bad "kernel modules present but no preset declares them"
    elif grep -Ev "${EXPECT_RE}" <<<"${KOS}" | grep -q .; then
        bad "undeclared kernel modules: $(grep -Ev "${EXPECT_RE}" <<<"${KOS}" | head -2 | tr '\n' ' ')"
    else
        ok "kernel modules present, all declared by presets ($(wc -l <<<"${KOS}") files)"
    fi
    # merged-usr: the real file is /usr/sbin/init, /sbin is a symlink to it
    if grep -Eq '^(\./)?(usr/)?sbin/init$' "${STAGE}/rootfs-tar-listing.txt"; then
        ok "bootable: /sbin/init present (PID 1 = systemd)"
    else
        bad "/sbin/init missing -- rootfs cannot boot without systemd-sysv"
    fi
    # /etc/os-release is a symlink; the real file is /usr/lib/os-release
    OS_REL="$(tar -xOf "${TAR}" usr/lib/os-release ./usr/lib/os-release 2>/dev/null || true)"
    if grep -q 'VERSION_ID="26.04"' <<<"${OS_REL}"; then
        ok "os-release is Ubuntu 26.04"
    else
        bad "os-release not Ubuntu 26.04 (got: $(grep VERSION_ID= <<<"${OS_REL}" || echo nothing))"
    fi
    if grep -qx '/usr/sbin/update-initramfs' "${STAGE}/initramfs-files.txt" 2>/dev/null; then
        ok "update-initramfs present"
    else
        bad "update-initramfs not in initramfs-tools file list"
    fi
    if grep -qx '/usr/sbin/mkinitramfs' "${STAGE}/initramfs-files.txt" 2>/dev/null; then
        ok "mkinitramfs present"
    else
        bad "mkinitramfs not in initramfs-tools-core file list"
    fi
    # every preset overlay file must have made it into the tree
    for p in ${PRESENTS}; do
        tdir="${META}/presets/${p}/tree"
        [ -d "${tdir}" ] || continue
        while IFS= read -r f; do
            rel="${f#"${tdir}/"}"
            if grep -qx "${rel}" "${STAGE}/rootfs-tar-listing.txt" \
                || grep -qx "./${rel}" "${STAGE}/rootfs-tar-listing.txt"; then
                ok "preset ${p}: ${rel}"
            else
                bad "preset ${p}: ${rel} missing from tree"
            fi
        done < <(find "${tdir}" \( -type f -o -type l \) | LC_ALL=C sort)
    done
else
    bad "rootfs.tar missing: ${TAR}"
fi

# ---- 5/6) package database ---------------------------------------------------
PKGS="${STAGE}/dpkg-packages.txt"
for p in initramfs-tools initramfs-tools-core busybox-initramfs kmod cpio; do
    if grep -Eq "^${p}[[:space:]]" "${PKGS}" 2>/dev/null; then
        ok "package installed: ${p}"
    else
        bad "package missing: ${p}"
    fi
done
if grep -Eq '^linux-(image|modules|headers)' "${PKGS}" 2>/dev/null; then
    bad "kernel packages installed (linux-image/modules/headers)"
else
    ok "no kernel packages installed"
fi

# ---- 8) Android sparse image -------------------------------------------------
SIMG="${WORK}/${ROOTFS_NAME}.sparse.img"
if [ -s "${SIMG}" ]; then
    ok "${ROOTFS_NAME}.sparse.img exists ($(numfmt --to=iec "$(stat -c%s "${SIMG}")" 2>/dev/null || stat -c%s "${SIMG}"))"
else
    bad "${ROOTFS_NAME}.sparse.img missing or empty: ${SIMG}"
fi
if python3 - "${SIMG}" <<'EOF'
import sys
with open(sys.argv[1], "rb") as f:
    sys.exit(0 if f.read(4) == b"\x3a\xff\x26\xed" else 1)
EOF
then ok "Android sparse image magic present"
else bad "Android sparse image magic missing"
fi

# ---- 9) checksums --------------------------------------------------------------
( cd "${WORK}" && sha256sum "${ROOTFS_NAME}.img" "${ROOTFS_NAME}.sparse.img" ) > "${WORK}/SHA256SUMS"
if ( cd "${WORK}" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ); then
    ok "SHA256SUMS written and verified (${WORK}/SHA256SUMS)"
else
    bad "SHA256SUMS verification failed"
fi

printf 'verify: %d passed, %d failed\n' "${PASS}" "${FAIL}"
[ "${FAIL}" -eq 0 ] || exit 1
