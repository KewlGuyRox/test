#!/usr/bin/env python3
"""Verify that a YSPEED RAM-boot uImage actually contains an initramfs.

A kernel-only image still passes every CRC check in validate-uimage.py and is
still named ...-initramfs-kernel.bin, but it panics at boot with
"VFS: Cannot open root device" because there is nothing to mount. That has
already happened once in this project, so the build now fails closed here.
"""
import argparse
import gzip
import lzma
import struct
import sys

FDT_MAGIC = b"\xd0\x0d\xfe\xed"
NEWC_MAGIC = b"070701"
TRAILER = b"TRAILER!!!"


def decompress(payload, comp):
    """Return the decompressed kernel stream, or None."""
    if comp == 3:  # LZMA, OpenWrt default for this target
        try:
            return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(payload)
        except lzma.LZMAError:
            return None
    if comp == 1:  # gzip
        try:
            return gzip.decompress(payload)
        except OSError:
            return None
    if comp == 0:  # uncompressed
        return payload
    return None


def walk_newc(buf, start):
    """Walk a newc cpio archive at `start`. Return list of names or []."""
    names = []
    pos = start
    while pos + 110 <= len(buf):
        if buf[pos:pos + 6] != NEWC_MAGIC:
            return []
        try:
            namesize = int(buf[pos + 94:pos + 102], 16)
            filesize = int(buf[pos + 54:pos + 62], 16)
        except ValueError:
            return []
        name_start = pos + 110
        name_end = name_start + namesize
        if namesize <= 0 or name_end > len(buf):
            return []
        raw = buf[name_start:name_end]
        if not raw.endswith(b"\0"):
            return []
        name = raw[:-1]
        if any(b < 32 or b > 126 for b in name):
            return []
        if name == TRAILER:
            return names
        names.append(name.decode("ascii", "replace"))
        pos = name_end
        pos = (pos + 3) & ~3
        pos += filesize
        pos = (pos + 3) & ~3
        if len(names) > 100000:
            return names
    return names


def find_initramfs(raw):
    """Locate a walkable newc archive, plain or gzip-wrapped."""
    idx = 0
    while True:
        idx = raw.find(NEWC_MAGIC, idx)
        if idx < 0:
            return []
        names = walk_newc(raw, idx)
        if len(names) >= 3:  # a real rootfs has many entries
            return names
        idx += 6


def main():
    ap = argparse.ArgumentParser(description="Verify a YSPEED RAM-boot image embeds an initramfs")
    ap.add_argument("image")
    ap.add_argument("--min-entries", type=int, default=3)
    ap.add_argument(
        "--min-mib", type=float, default=12.0,
        help="sanity floor for the decompressed kernel+initramfs size in MiB")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    magic, hcrc, ts, size, load, entry, dcrc, os_id, arch, itype, comp = \
        struct.unpack(">7I4B", data[:32])

    print("== initramfs gate ==")
    print("image           : %s" % args.image)
    print("compression     : %d" % comp)
    print("payload_bytes   : %d" % size)

    if magic != 0x27051956:
        sys.exit("FAIL: not a legacy uImage")

    payload = data[64:64 + size]
    raw = decompress(payload, comp)
    if raw is None:
        sys.exit("FAIL: could not decompress payload (compression=%d)" % comp)

    mib = len(raw) / 1048576.0
    print("decompressed    : %d bytes (%.2f MiB)" % (len(raw), mib))

    names = find_initramfs(raw)
    if not names:
        print("initramfs       : NOT FOUND")
        print("hint            : a kernel-only image looks exactly like this;")
        print("                  build the rootfs before the kernel (full make),")
        print("                  not just target/linux/compile")
        sys.exit("FAIL: no initramfs in the image - it would panic at boot")

    print("initramfs       : found, %d entries, first: %s"
          % (len(names), ", ".join(names[:5])))

    if mib < args.min_mib:
        print("size            : only %.2f MiB decompressed" % mib)
        sys.exit("FAIL: decompressed image is smaller than %.1f MiB; "
                 "the initramfs is probably missing most packages" % args.min_mib)

    dtb = raw.rfind(FDT_MAGIC)
    if dtb >= 0:
        total = struct.unpack(">I", raw[dtb + 4:dtb + 8])[0]
        print("appended DTB    : offset 0x%x size %d" % (dtb, total))
    else:
        print("appended DTB    : not found")

    print("result          : PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
