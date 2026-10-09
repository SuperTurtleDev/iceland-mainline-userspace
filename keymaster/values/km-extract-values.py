#!/usr/bin/env python3
"""km-extract-values.py -- derive the forged boot-state values km-init
reports to the keymaster TA, from the stock partition dumps (iceland-fw).

Inputs (all optional except vbmeta.img):
  vbmeta.img, vbmeta_system.img, vbmeta_vendor.img  -- AVB0 images; the
  chain is walked through chain-partition descriptors exactly like
  avb_slot_verify() does (parent first, depth-first, descriptor order),
  and the VBH is SHA256 over the vbmeta blobs in that order -- the same
  value stock Android prints as androidboot.vbmeta.digest.

Outputs /etc/km-init/values.conf keys:
  avb_public_key         raw public key blob of the root vbmeta
  vbmeta_digest          the VBH (cross-check with --digest-from-cmdline!)
  os_version_packed      (maj<<14)|(min<<7)|sub           (ABL semantics)
  security_patch_packed  (day<<11)|((year-2000)<<4)|month (ABL semantics)
  color=0 is_unlocked=0 send_milestone=1

Usage:
  ./km-extract-values.py <fw_dir> [-o values.conf]
                         [--digest-from-cmdline <hex>]

The packing mirrors QcomModulePkg/Library/avb/VerifiedBoot.c
(ParseFooterOsVersion / ParseFooterSecPatch) so the numbers the TA sees
are byte-identical to a stock locked boot.
"""

import argparse
import hashlib
import os
import struct
import sys

VBMETA_HEADER_SIZE = 256

# descriptor tags (avb_descriptor.h)
TAG_PROPERTY = 0
TAG_HASHTREE = 1
TAG_HASH = 2
TAG_KERNEL_CMDLINE = 3
TAG_CHAIN_PARTITION = 4

PROP_OS_VERSION = "com.android.build.boot.os_version"
PROP_SEC_PATCH = "com.android.build.boot.security_patch"


class VBMeta:
    def __init__(self, name, data):
        self.name = name
        if data[:4] != b"AVB0":
            # chained partitions may be plain images with an AVB footer
            # (boot/init_boot/...): the vbmeta struct is embedded at the
            # end, pointed to by the footer.  avb_slot_verify records
            # exactly those vbmeta_size bytes in SlotData->vbmeta_images.
            footer = data[-64:]
            if footer[:4] != b"AVBf":
                raise ValueError(f"{name}: neither AVB0 nor AVBf image")
            (vb_off, vb_size) = struct.unpack_from(">QQ", footer, 12)
            data = data[vb_off:vb_off + vb_size]
        self.data = data
        (self.auth_size, self.aux_size) = struct.unpack_from(">QQ", data, 12)
        (self.pk_off, self.pk_size) = struct.unpack_from(">QQ", data, 64)
        (self.desc_off, self.desc_size) = struct.unpack_from(">QQ", data, 96)

    @property
    def raw_public_key(self):
        base = VBMETA_HEADER_SIZE + self.auth_size
        return self.data[base + self.pk_off: base + self.pk_off + self.pk_size]

    @property
    def trimmed(self):
        """header + auth + aux block, without partition padding"""
        return self.data[:VBMETA_HEADER_SIZE + self.auth_size + self.aux_size]

    def descriptors(self):
        # descriptors_offset is relative to the auxiliary data block
        off = VBMETA_HEADER_SIZE + self.auth_size + self.desc_off
        end = off + self.desc_size
        while off + 16 <= end:
            tag, length = struct.unpack_from(">QQ", self.data, off)
            payload = self.data[off + 16: off + 16 + length]
            yield tag, payload
            off += 16 + length

    def chain_partitions(self):
        for tag, payload in self.descriptors():
            if tag != TAG_CHAIN_PARTITION:
                continue
            # AvbChainPartitionDescriptor payload (after tag+length):
            # u32 rollback_index_location, u32 partition_name_len,
            # u32 public_key_len, u32 flags, u8 reserved[60], then name+key
            (plen,) = struct.unpack_from(">I", payload, 4)
            name = payload[76:76 + plen].decode()
            yield name

    def properties(self):
        props = {}
        for tag, payload in self.descriptors():
            if tag != TAG_PROPERTY:
                continue
            # AvbPropertyDescriptor payload: u64 key_num_bytes,
            # u64 value_num_bytes, key bytes, NUL separator, value bytes
            # (trailing NUL padding to 8-byte alignment)
            kn, vn = struct.unpack_from(">QQ", payload, 0)
            key = payload[16:16 + kn].decode()
            value = payload[16 + kn + 1:16 + kn + 1 + vn].decode()
            props[key] = value
        return props


def load_chain(fw_dir):
    """depth-first, parent-first walk, mirroring avb_slot_verify's
    SlotData->vbmeta_images population order"""
    order = []
    seen = set()

    def visit(name):
        if name in seen:
            return
        seen.add(name)
        path = os.path.join(fw_dir, name + ".img")
        if not os.path.isfile(path):
            print(f"  chain partition {name}: {path} missing, skipped",
                  file=sys.stderr)
            return
        m = VBMeta(name, open(path, "rb").read())
        order.append(m)
        for child in m.chain_partitions():
            visit(child)

    visit("vbmeta")
    return order


def parse_os_version(s):
    parts = s.split(".")
    if not 1 <= len(parts) <= 3 or not all(p.isdigit() for p in parts):
        raise ValueError(f"os_version {s!r} not x[.y[.z]]")
    nums = [int(p) for p in parts] + [0, 0]
    maj, mi, sub = nums[:3]
    return (maj << 14) | (mi << 7) | sub


def parse_sec_patch(s):
    y, m, d = (int(p) for p in s.split("-"))
    if not (2000 < y and 1 <= m <= 12 and 1 <= d <= 31):
        raise ValueError(f"security_patch {s!r}")
    return (d << 11) | ((y - 2000) << 4) | m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fw_dir")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--digest-from-cmdline", default=None,
                    help="hex of stock androidboot.vbmeta.digest (ground "
                         "truth); selects full-vs-trimmed blob hashing")
    args = ap.parse_args()

    chain = load_chain(args.fw_dir)
    if not chain:
        sys.exit("no vbmeta.img found")
    print(f"vbmeta chain (hash order): "
          + " -> ".join(m.name for m in chain))

    root = chain[0]
    for m in chain:
        print(f"  {m.name}.img: {len(m.data)} bytes, "
              f"pubkey {len(m.raw_public_key)} B, "
              f"chains -> {list(m.chain_partitions()) or '-'}")

    props = {}
    for m in chain:
        for k, v in m.properties().items():
            props.setdefault(k, (m.name, v))
    # ABL reads the two boot props from the vbmeta_images entry named
    # "boot" (the footer-embedded vbmeta of the boot partition) and only
    # falls back to the root vbmeta when there is none -- mirror that.
    boot_entry = next((m for m in chain if m.name == "boot"), None)
    for k in (PROP_OS_VERSION, PROP_SEC_PATCH):
        if boot_entry and k in boot_entry.properties():
            props[k] = ("boot", boot_entry.properties()[k])
    for k in (PROP_OS_VERSION, PROP_SEC_PATCH):
        if k not in props:
            sys.exit(f"property {k} not found in any vbmeta image")
    os_src, os_ver = props[PROP_OS_VERSION]
    sp_src, sp_patch = props[PROP_SEC_PATCH]
    print(f"  {PROP_OS_VERSION} = {os_ver} (from {os_src})")
    print(f"  {PROP_SEC_PATCH} = {sp_patch} (from {sp_src})")

    full = hashlib.sha256(b"".join(m.data for m in chain)).hexdigest()
    trimmed = hashlib.sha256(b"".join(m.trimmed for m in chain)).hexdigest()
    vbh = full
    note = "full partition content"
    if args.digest_from_cmdline:
        want = args.digest_from_cmdline.lower().replace("0x", "")
        if want == full:
            vbh, note = full, "full (matches cmdline)"
        elif want == trimmed:
            vbh, note = trimmed, "trimmed (matches cmdline)"
        else:
            print(f"WARNING: neither variant matches the cmdline digest\n"
                  f"  full:    {full}\n  trimmed: {trimmed}\n"
                  f"  cmdline: {want}", file=sys.stderr)
            vbh = want
            note = "cmdline override"
    print(f"  vbmeta digest (VBH), {note}: {vbh}")

    conf = "\n".join([
        "# generated by km-extract-values.py from "
        + os.path.basename(args.fw_dir.rstrip("/")),
        f"# vbmeta chain: " + " -> ".join(m.name for m in chain),
        f"# {PROP_OS_VERSION} = {os_ver} (from {os_src})",
        f"# {PROP_SEC_PATCH} = {sp_patch} (from {sp_src})",
        f"# VBH basis: {note}",
        "avb_public_key = " + root.raw_public_key.hex(),
        "vbmeta_digest = " + vbh,
        f"os_version_packed = {parse_os_version(os_ver)}"
        f"  # {os_ver}",
        f"security_patch_packed = {parse_sec_patch(sp_patch)}"
        f"  # {sp_patch}",
        "color = 0",
        "is_unlocked = 0",
        "send_milestone = 1",
    ]) + "\n"

    if args.output:
        open(args.output, "w").write(conf)
        print(f"wrote {args.output}")
    else:
        sys.stdout.write(conf)


if __name__ == "__main__":
    main()
