#!/usr/bin/env bash
# build-tree.sh -- stage 1: debootstrap the Ubuntu (arm64) rootfs tree.
#
# Usage (invoked by ../build.sh, as root, with WORK/META/PRESENTS/SUITE/
# MIRROR in the env).
#
# Runs ON THE HOST, not in a container: bootstrapping and image packing are
# not compile jobs, and debootstrap produces a real system root filesystem
# (proper maintainer scripts, systemd presets, first-boot semantics) --
# the container-image-export flow this replaced yields a container-shaped
# rootfs instead. arm64 code inside the chroot executes through the host
# qemu-aarch64 binfmt_misc registration (flags include F, so the
# interpreter works inside chroots without copying it in).
#
# Flow:
#   1. debootstrap --foreign  (stage 1: pure data, download + unpack)
#   2. bind /proc /sys /dev + the shared apt caches into the target
#   3. chroot second-stage, then chroot apt-get for base + preset packages
#      (preset apt-preferences are applied BEFORE any install; preset
#      debs are dpkg -i'd; preset recommends rounds run with recommends)
#   4. hygiene + preset overlays/hooks + kernel guard + records (chroot)
#   5. tar with numeric ownership -> WORK/rootfs.tar; the tree stays
#      root-owned on the host for make-image.sh
#
# Output:
#   WORK/tree         the root filesystem tree (root-owned)
#   WORK/rootfs.tar   same content, numeric ids (input for verify)
#   WORK/staging/*    arch, os-release, dpkg dump, initramfs file lists
set -euo pipefail

WORK="${WORK:?set by build.sh}"
META="${META:?set by build.sh}"
PRESENTS="${PRESENTS:-}"
SUITE="${SUITE:?set by build.sh}"
MIRROR="${MIRROR:?set by build.sh}"
SUDOUID="${SUDO_UID:-1000}"
TREE="${WORK}/tree"
KEYRING=/usr/share/keyrings/ubuntu-archive-keyring.gpg

log() { printf '[build-tree] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root (build.sh re-execs under sudo)"
command -v debootstrap >/dev/null 2>&1 || die "debootstrap not found on host"
[ -f "${KEYRING}" ] || die "archive keyring missing: ${KEYRING}"
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || die "binfmt qemu-aarch64 not registered (install qemu-user-static)"

# mounts to undo on exit (most-nested first)
MOUNTED=""
cleanup() {
    set +e
    for m in ${MOUNTED}; do umount -l "${TREE}${m}" >/dev/null 2>&1; done
}
trap cleanup EXIT

bind() {   # bind <target-relative-path> <host-path>
    mkdir -p "${TREE}$1"
    mount --bind "$2" "${TREE}$1"
    MOUNTED="$1 ${MOUNTED}"
}

chroot_run() {
    chroot "${TREE}" /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin \
        DEBIAN_FRONTEND=noninteractive LC_ALL=C TZ=UTC "$@"
}

pkglist() {
    sed -e "s/[[:space:]]*#.*//" "$1" | grep -E -v "^[[:space:]]*$" | xargs
}

# ---- 1) debootstrap stage 1 (data-only) --------------------------------------
log "debootstrap --foreign ${SUITE} arm64 from ${MIRROR}"
# self-heal: drop stale bind mounts an interrupted earlier run may have left
STALE="$(awk '{print $2}' /proc/mounts | grep -F "${TREE}/" | tac || true)"
if [ -n "${STALE}" ]; then
    log "unmounting stale binds under ${TREE}"
    echo "${STALE}" | xargs -r -n1 umount -l || true
fi
rm -rf "${TREE}"
debootstrap --arch=arm64 --foreign --variant=minbase \
    --components=main,restricted,universe,multiverse \
    --keyring="${KEYRING}" \
    "${SUITE}" "${TREE}" "${MIRROR}" 2>&1 | tail -5

# ---- 2) chroot environment -----------------------------------------------------
cp -L /etc/resolv.conf "${TREE}/etc/resolv.conf"
# fresh proc/sysfs mounts (not binds): a bind cannot be detected as a
# mountpoint from inside the chroot, which makes systemd-tmpfiles and
# friends warn "/proc/ is not mounted" during package configuration
mount -t proc proc "${TREE}/proc"
MOUNTED="/proc ${MOUNTED}"
mount -t sysfs sysfs "${TREE}/sys"
MOUNTED="/sys ${MOUNTED}"
# /dev gets its own tmpfs, NOT a bind of the host /dev: a bind shares the
# device inodes, so any chmod inside the chroot (package postinsts have
# done exactly that to /dev/null) would clobber the host devices
mount -t tmpfs tmpfs "${TREE}/dev"
MOUNTED="/dev ${MOUNTED}"
mkdir -p "${TREE}/dev/pts" "${TREE}/dev/shm"
mount -t devpts devpts "${TREE}/dev/pts"
MOUNTED="/dev/pts ${MOUNTED}"
mknod -m 666 "${TREE}/dev/null" c 1 3
mknod -m 666 "${TREE}/dev/zero" c 1 5
mknod -m 666 "${TREE}/dev/full" c 1 7
mknod -m 666 "${TREE}/dev/random" c 1 8
mknod -m 666 "${TREE}/dev/urandom" c 1 9
mknod -m 666 "${TREE}/dev/tty" c 5 0
mknod -m 600 "${TREE}/dev/console" c 5 1
# NOTE: the shared apt caches are bound only AFTER the second stage --
# stage 1 leaves its downloaded .debs in target /var/cache/apt/archives and
# the second stage installs exactly those; an early bind would shadow them
mkdir -p "${WORK}/apt-cache/archives/partial" "${WORK}/apt-cache/lists/partial"

# ---- 3) second stage + package installation -------------------------------------
log "debootstrap second stage (arm64 via binfmt)"
chroot "${TREE}" /debootstrap/debootstrap --second-stage 2>&1 | tail -8
bind /var/cache/apt/archives "${WORK}/apt-cache/archives"
bind /var/lib/apt/lists "${WORK}/apt-cache/lists"

log "presents: ${PRESENTS:-none}"
printf "%s\n" "${PRESENTS:-none}" > "${WORK}/staging/presets.txt"

# preset apt preferences (masks/pins) go in before any install
for p in ${PRESENTS:-}; do
    if [ -d "${META}/presets/${p}/apt-preferences" ]; then
        log "applying apt preferences: ${p}"
        cp -a "${META}/presets/${p}/apt-preferences/." "${TREE}/etc/apt/preferences.d/"
    fi
done

chroot_run apt-get update

PKGS="$(pkglist "${META}/packages.list")"
for p in ${PRESENTS:-}; do
    [ -d "${META}/presets/${p}" ] || die "preset not found: ${p}"
    [ -f "${META}/presets/${p}/packages.list" ] \
        && PKGS="${PKGS} $(pkglist "${META}/presets/${p}/packages.list")"
done
log "installing (no recommends): ${PKGS}"
chroot_run apt-get install -y --no-install-recommends ${PKGS}

for p in ${PRESENTS:-}; do
    if [ -f "${META}/presets/${p}/packages-recommends.list" ]; then
        log "installing (with recommends) preset ${p}: $(pkglist "${META}/presets/${p}/packages-recommends.list")"
        chroot_run apt-get install -y $(pkglist "${META}/presets/${p}/packages-recommends.list")
    fi
    if [ -f "${META}/presets/${p}/debs.list" ]; then
        for g in $(pkglist "${META}/presets/${p}/debs.list"); do
            for d in "${WORK}/debs"/${g}; do
                [ -f "$d" ] || die "deb not found for preset ${p}: ${g} (in ${WORK}/debs)"
                log "installing preset ${p} deb: $(basename "$d")"
                cp -f "$d" "${TREE}/tmp/"
                chroot_run dpkg -i "/tmp/$(basename "$d")"
                rm -f "${TREE}/tmp/$(basename "$d")"
            done
        done
    fi
done

# ---- 4) hygiene + presets + guard ------------------------------------------------
log "applying base hygiene"
# default login: debootstrap minbase has no unprivileged user; create one
# with a known bring-up password (hashed on the host: chpasswd in a chroot
# trips over pam_chauthtok) and the sudo group so both gdm and a serial
# console getty can log in (root itself stays password-locked)
UBUNTU_HASH="$(openssl passwd -6 'ubuntu')"
chroot_run useradd -m -c "Ubuntu" -s /bin/bash -G sudo -p "${UBUNTU_HASH}" ubuntu
# hostname + hosts
printf "sm8850\n" > "${TREE}/etc/hostname"
cat > "${TREE}/etc/hosts" <<HOSTS
127.0.0.1       localhost
127.0.1.1       sm8850
::1             localhost ip6-localhost ip6-loopback
HOSTS
# NetworkManager: enable deterministically
if [ -f "${TREE}/usr/lib/systemd/system/NetworkManager.service" ]; then
    mkdir -p "${TREE}/etc/systemd/system/multi-user.target.wants"
    ln -sfn /usr/lib/systemd/system/NetworkManager.service \
        "${TREE}/etc/systemd/system/multi-user.target.wants/NetworkManager.service"
fi
# /etc/fstab: the first-boot bootstrap may hand over the rootfs mounted
# read-only (kernel cmdline "ro"); with this entry systemd-remount-fs
# flips it rw at the earliest boot stage, before /var-writing services
# start (without it cups/power-profiles-daemon/logind/gdm/... all fail
# with "Read-only file system")
cat > "${TREE}/etc/fstab" <<FSTAB
# <device> <mount> <type> <options> <dump> <pass>
/dev/root  /       auto   rw        0       1
FSTAB
# power: deep sleep does not survive this platform's warm boot (the
# bootloader comes back with a different memory mapping, so resuming from
# deep power collapse is impossible). Force s2idle and mask the
# hibernate-family units; plain suspend (s2idle) stays available.
printf 'W /sys/power/mem_sleep - - - - s2idle\n' \
    > "${TREE}/usr/lib/tmpfiles.d/mem-sleep.conf"
for u in systemd-hibernate.service systemd-suspend-then-hibernate.service \
         systemd-hybrid-sleep.service; do
    ln -sfn /dev/null "${TREE}/etc/systemd/system/${u}"
done
# apt sources: debootstrap only writes a single-line sources.list for the
# base suite; ship the standard complete set instead (deb822, installer
# layout: general pockets + security stanza, both from the mirror since
# arm64 has no separate security host)
rm -f "${TREE}/etc/apt/sources.list"
mkdir -p "${TREE}/etc/apt/sources.list.d"
cat > "${TREE}/etc/apt/sources.list.d/ubuntu.sources" <<APTSRC
Types: deb
URIs: ${MIRROR}
Suites: ${SUITE} ${SUITE}-updates ${SUITE}-backports
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: ${MIRROR}
Suites: ${SUITE}-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APTSRC
# power button, Android-style: short press LOCKS the screen, long press
# powers off, and nothing ever suspends (s2idle does not resume on this
# platform yet). logind's lock action drives the session Lock() that
# gnome-shell turns into the lock screen; GNOME itself is set to 'nothing'
# so the key falls through to logind, and idle auto-suspend is disabled
mkdir -p "${TREE}/etc/systemd/logind.conf.d"
cat > "${TREE}/etc/systemd/logind.conf.d/50-power-button.conf" <<LOGIND
[Login]
HandlePowerKey=lock
HandlePowerKeyLongPress=poweroff
HandleSuspendKey=ignore
HandleHibernateKey=ignore
LOGIND
mkdir -p "${TREE}/etc/dconf/profile" "${TREE}/etc/dconf/db/local.d"
printf 'user-db:user\nsystem-db:local\n' > "${TREE}/etc/dconf/profile/user"
cat > "${TREE}/etc/dconf/db/local.d/00-power" <<DCONF
[org/gnome/settings-daemon/plugins/power]
power-button-action='nothing'
sleep-inactive-battery-type='nothing'
sleep-inactive-ac-type='nothing'
DCONF
# empty machine-id (re-created on first boot), headless default target,
# well-known mountpoint dirs, no variable leftovers inside the tree.
# NOTE: the apt caches are bind mounts -- do NOT clean them here (that
# would wipe the shared host cache); they never end up in the tree anyway.
rm -f "${TREE}/etc/machine-id"
: > "${TREE}/etc/machine-id"
ln -sfn multi-user.target "${TREE}/etc/systemd/system/default.target"
mkdir -p "${TREE}/boot" "${TREE}/usr/lib/modules"
rm -rf "${TREE}/tmp"/* "${TREE}/var/tmp"/*

for p in ${PRESENTS:-}; do
    if [ -d "${META}/presets/${p}/tree" ]; then
        log "applying preset overlay: ${p}"
        cp -a "${META}/presets/${p}/tree/." "${TREE}/"
    fi
    if [ -f "${META}/presets/${p}/hook.sh" ]; then
        log "running preset hook (in chroot): ${p}"
        cp -f "${META}/presets/${p}/hook.sh" "${TREE}/tmp/hook.sh"
        chroot "${TREE}" /usr/bin/env -i PRESENT="${p}" PATH=/usr/sbin:/usr/bin:/sbin:/bin \
            bash /tmp/hook.sh
        rm -f "${TREE}/tmp/hook.sh"
    fi
done

# compile the dconf system database (only exists on desktop variants)
[ -x "${TREE}/usr/bin/dconf" ] && chroot_run dconf update || true

# ---- no kernel by design --------------------------------------------------------
BAD="$(chroot_run dpkg-query -W -f="\${binary:Package}\n" 2>/dev/null \
    | grep -E "^linux-(image|modules|headers)" || true)"
[ -z "${BAD}" ] || die "kernel packages leaked in: ${BAD}"

# ---- 5) records + tar --------------------------------------------------------------
log "recording tree state"
aarch64_ok=0
[ "$(chroot_run uname -m 2>/dev/null)" = aarch64 ] && aarch64_ok=1
echo "$([ "${aarch64_ok}" = 1 ] && echo aarch64 || echo unknown)" \
    > "${WORK}/staging/tree-arch.txt"
cp "${TREE}/etc/os-release" "${WORK}/staging/tree-os-release"
chroot_run dpkg-query -W -f="\${binary:Package}\t\${Version}\n" \
    > "${WORK}/staging/dpkg-packages.txt"
cp "${TREE}/var/lib/dpkg/status" "${WORK}/staging/dpkg-status"
: > "${WORK}/staging/initramfs-files.txt"
for p in initramfs-tools initramfs-tools-core busybox-initramfs; do
    printf "=== %s ===\n" "${p}" >> "${WORK}/staging/initramfs-files.txt"
    chroot_run dpkg -L "${p}" >> "${WORK}/staging/initramfs-files.txt" || true
done
chown -R "${SUDOUID}" "${WORK}/staging"

# drop every bind mount BEFORE packing: leaving /proc /sys /dev mounted
# would archive the host's pseudo-filesystems into the image
log "unmounting chroot binds"
for m in ${MOUNTED}; do umount -l "${TREE}${m}" >/dev/null 2>&1 || true; done
MOUNTED=""

log "packing ${WORK}/rootfs.tar"
rm -f "${WORK}/rootfs.tar"
tar --numeric-owner -C "${TREE}" -cf "${WORK}/rootfs.tar" .
chown "${SUDOUID}" "${WORK}/rootfs.tar"

log "done: ${TREE} (+ rootfs.tar, staging/)"
