#!/usr/bin/env python3
"""Validate that a RAM-boot uImage embeds the configured OpenWrt rootfs.

OpenWrt compresses the initramfs CPIO separately (CONFIG_INITRAMFS_COMPRESSION_LZMA)
before linking it into the kernel. The outer uImage LZMA decompression therefore
does not expose a plain newc archive. Validate both layers rather than searching
only for an uncompressed CPIO signature in the decompressed kernel.
"""
import argparse
import hashlib
import lzma
import struct
import sys

UIMAGE_MAGIC = 0x27051956
NEWC_MAGICS = (b"070701", b"070702")
TRAILER = b"TRAILER!!!"
# Files installed by this project and required for the replacement appliance.
REQUIRED = (
    "init",
    "etc/config/network",
    "etc/chrony/chrony.conf",
    "usr/sbin/yspeed-status",
    "usr/sbin/yspeed-display",
)


def decompress_uimage(payload, comp):
    if comp == 3:
        try:
            return lzma.decompress(payload, format=lzma.FORMAT_ALONE,
                                   memlimit=256 * 1024 * 1024)
        except lzma.LZMAError:
            return None
    if comp == 1:
        import gzip
        try:
            return gzip.decompress(payload)
        except OSError:
            return None
    if comp == 0:
        return payload
    return None


def walk_newc(buf, start=0):
    """Return archive member names only if a complete newc archive is present."""
    names = []
    pos = start
    while pos + 110 <= len(buf):
        if buf[pos:pos + 6] not in NEWC_MAGICS:
            return None
        try:
            filesize = int(buf[pos + 54:pos + 62], 16)
            namesize = int(buf[pos + 94:pos + 102], 16)
        except ValueError:
            return None
        if namesize < 1:
            return None
        name_start = pos + 110
        name_end = name_start + namesize
        if name_end > len(buf):
            return None
        raw_name = buf[name_start:name_end]
        if not raw_name.endswith(b"\0"):
            return None
        name = raw_name[:-1]
        if any(byte < 32 or byte > 126 for byte in name):
            return None
        if name == TRAILER:
            return names
        names.append(name.decode("ascii", "replace"))
        pos = (name_end + 3) & ~3
        data_end = pos + filesize
        if data_end > len(buf):
            return None
        pos = (data_end + 3) & ~3
        if len(names) > 100000:
            return None
    return None


def find_cpio(buf):
    """Find a plain newc archive in a byte buffer."""
    for magic in NEWC_MAGICS:
        pos = 0
        while True:
            pos = buf.find(magic, pos)
            if pos < 0:
                break
            names = walk_newc(buf, pos)
            if names is not None:
                return names, None
            pos += len(magic)
    return None, None


def find_embedded_initramfs(kernel):
    """Find either a plain CPIO or the kernel's separately LZMA-compressed CPIO."""
    names, _ = find_cpio(kernel)
    if names is not None:
        return names, "uncompressed CPIO"

    # OpenWrt's kernel initramfs compression uses LZMA-alone. The configured
    # lc=1/lp=2/pb=2 stream starts with property byte 0x6d; dictionary size is
    # little-endian and normally 8 MiB. Check plausible LZMA-alone headers and
    # accept a candidate only if it expands into a structurally complete CPIO.
    for pos in range(max(0, len(kernel) - 13)):
        props = kernel[pos]
        if props > 224:
            continue
        dictionary = int.from_bytes(kernel[pos + 1:pos + 5], "little")
        if dictionary < 4096 or dictionary > 64 * 1024 * 1024:
            continue
        if dictionary & (dictionary - 1):
            continue
        try:
            dec = lzma.LZMADecompressor(
                format=lzma.FORMAT_ALONE, memlimit=128 * 1024 * 1024)
            archive = dec.decompress(kernel[pos:])
            if not dec.eof:
                continue
        except (lzma.LZMAError, EOFError, MemoryError):
            continue
        names, _ = find_cpio(archive)
        if names is not None:
            return names, "LZMA-compressed CPIO"
    return None, None


def normalize_names(names):
    normalized = set()
    for name in names:
        while name.startswith("./"):
            name = name[2:]
        normalized.add(name.lstrip("/"))
    return normalized


def main():
    ap = argparse.ArgumentParser(
        description="Verify that a RAM-boot uImage contains the OpenWrt initramfs")
    ap.add_argument("image")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    if len(data) < 64:
        sys.exit("FAIL: file shorter than legacy uImage header")
    fields = struct.unpack(">7I4B", data[:32])
    magic, _, _, size, _, _, _, _, _, _, comp = fields
    if magic != UIMAGE_MAGIC:
        sys.exit("FAIL: not a legacy uImage")
    if len(data) != 64 + size:
        sys.exit("FAIL: file length does not match uImage payload size")

    payload = data[64:64 + size]
    kernel = decompress_uimage(payload, comp)
    if kernel is None:
        sys.exit("FAIL: could not decompress uImage payload (compression=%d)" % comp)

    print("== initramfs gate ==")
    print("image           : %s" % args.image)
    print("payload_bytes   : %d" % size)
    print("kernel_bytes    : %d" % len(kernel))
    names, method = find_embedded_initramfs(kernel)
    if names is None:
        print("initramfs       : NOT FOUND")
        sys.exit("FAIL: no structurally valid embedded CPIO initramfs found")

    members = normalize_names(names)
    missing = [path for path in REQUIRED if path not in members]
    print("initramfs       : found, %d entries (%s)" % (len(names), method))
    print("required files  : %d/%d present" % (len(REQUIRED) - len(missing), len(REQUIRED)))
    if missing:
        for path in missing:
            print("missing         : /" + path)
        sys.exit("FAIL: initramfs is missing required appliance files")

    print("required files  : " + ", ".join("/" + path for path in REQUIRED))
    print("sha256          : " + hashlib.sha256(data).hexdigest())
    print("result          : PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
