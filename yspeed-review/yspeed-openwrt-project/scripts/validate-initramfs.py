#!/usr/bin/env python3
"""Verify that a YSPEED RAM-boot uImage embeds a usable initramfs.

Linux can compress the initramfs cpio archive independently of the outer
uImage compression. OpenWrt's Linux 6.12 build selects
CONFIG_INITRAMFS_COMPRESSION_LZMA and runs the LZMA-alone compressor on
initramfs_data.cpio. Searching only for literal newc magic in the decompressed
kernel therefore produces a false "NOT FOUND" result. This gate understands
plain, gzip, and LZMA-alone cpio archives and checks essential appliance files.
"""
import argparse
import gzip
import lzma
import struct
import sys
import zlib

FDT_MAGIC = bytes([0xd0, 0x0d, 0xfe, 0xed])
NEWC_MAGIC = b"070701"
TRAILER = b"TRAILER!!!"
NUL = bytes([0])
GZIP_MAGIC = bytes([0x1f, 0x8b])
MAX_ARCHIVE = 128 * 1024 * 1024
MAX_LZMA_DICT = 128 * 1024 * 1024


def decompress_uimage(payload, comp):
    """Return the outer uImage payload decompressed, or None."""
    if comp == 3:
        try:
            return lzma.decompress(payload, format=lzma.FORMAT_ALONE)
        except lzma.LZMAError:
            return None
    if comp == 1:
        try:
            return gzip.decompress(payload)
        except OSError:
            return None
    if comp == 0:
        return payload
    return None


def walk_newc(buf, start=0):
    """Walk a newc cpio archive at start. Return entry names, or []."""
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
        raw_name = buf[name_start:name_end]
        if not raw_name.endswith(NUL):
            return []
        name = raw_name[:-1]
        if any(byte < 32 or byte > 126 for byte in name):
            return []
        if name == TRAILER:
            return names
        names.append(name.decode("ascii", "replace"))
        pos = (name_end + 3) & ~3
        pos += filesize
        pos = (pos + 3) & ~3
        if len(names) > 100000:
            return names
    return names


def accepted_archive(archive):
    """Return parsed names and uncompressed byte count for a newc archive."""
    if not archive.startswith(NEWC_MAGIC):
        return None
    names = walk_newc(archive)
    if len(names) >= 3:
        return names, len(archive)
    return None


def find_lzma_newc(raw):
    """Find an LZMA-alone stream containing a newc archive.

    Linux 6.12's initramfs build uses 'lzma -9'. The stream has the standard
    13-byte LZMA-alone header (properties, dictionary size, uncompressed size).
    Candidate headers are screened before attempting decompression. Do not cap
    the number of candidates: executable kernel data can contain hundreds of
    byte sequences that resemble LZMA headers before the actual initramfs stream.
    """
    attempts = 0
    for offset in range(max(0, len(raw) - 12)):
        properties = raw[offset]
        if properties > 224:
            continue
        dictionary_size = struct.unpack_from("<I", raw, offset + 1)[0]
        if not 4096 <= dictionary_size <= MAX_LZMA_DICT:
            continue
        if dictionary_size & (dictionary_size - 1):
            power = dictionary_size.bit_length() - 1
            if dictionary_size != 3 * (1 << (power - 1)):
                continue
        uncompressed_size = struct.unpack_from("<Q", raw, offset + 5)[0]
        if uncompressed_size != 0xffffffffffffffff and not 110 <= uncompressed_size <= MAX_ARCHIVE:
            continue
        attempts += 1
        try:
            decoder = lzma.LZMADecompressor(
                format=lzma.FORMAT_ALONE,
                memlimit=256 * 1024 * 1024,
            )
            archive = decoder.decompress(raw[offset:], max_length=MAX_ARCHIVE)
            if decoder.eof:
                result = accepted_archive(archive)
                if result:
                    return result
        except (lzma.LZMAError, EOFError, MemoryError):
            pass
    return None


def find_initramfs(raw):
    """Locate a plain, gzip-wrapped, or LZMA-alone-wrapped newc archive."""
    idx = 0
    while True:
        idx = raw.find(NEWC_MAGIC, idx)
        if idx < 0:
            break
        result = accepted_archive(raw[idx:])
        if result:
            return result[0], result[1], "plain-newc"
        idx += len(NEWC_MAGIC)

    idx = 0
    while True:
        idx = raw.find(GZIP_MAGIC, idx)
        if idx < 0:
            break
        try:
            archive = gzip.decompress(raw[idx:])
            result = accepted_archive(archive)
            if result:
                return result[0], result[1], "gzip"
        except (OSError, EOFError, zlib.error):
            pass
        idx += 2

    result = find_lzma_newc(raw)
    if result:
        return result[0], result[1], "lzma-alone"
    return [], 0, "not-found"


def main():
    ap = argparse.ArgumentParser(
        description="Verify a YSPEED RAM-boot uImage embeds a usable initramfs"
    )
    ap.add_argument("image")
    ap.add_argument("--min-entries", type=int, default=3)
    ap.add_argument(
        "--min-mib",
        type=float,
        default=1.0,
        help="minimum uncompressed initramfs archive size in MiB",
    )
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    if len(data) < 64:
        sys.exit("FAIL: image is shorter than a legacy uImage header")
    header = struct.unpack(">7I4B", data[:32])
    magic, hcrc, ts, size, load, entry, dcrc, os_id, arch, itype, comp = header

    print("== initramfs gate ==")
    print("image           : %s" % args.image)
    print("compression     : %d" % comp)
    print("payload_bytes   : %d" % size)

    if magic != 0x27051956:
        sys.exit("FAIL: not a legacy uImage")
    if 64 + size > len(data):
        sys.exit("FAIL: uImage payload length exceeds file size")

    raw = decompress_uimage(data[64:64 + size], comp)
    if raw is None:
        sys.exit("FAIL: could not decompress payload (compression=%d)" % comp)

    kernel_mib = len(raw) / 1048576.0
    print("kernel_payload  : %d bytes (%.2f MiB)" % (len(raw), kernel_mib))

    names, archive_bytes, archive_codec = find_initramfs(raw)
    if not names:
        print("initramfs       : NOT FOUND")
        print("hint            : the kernel may contain an independently compressed cpio archive")
        sys.exit("FAIL: no embedded initramfs - this image would not boot")
    if len(names) < args.min_entries:
        sys.exit("FAIL: initramfs has only %d entries; minimum is %d" % (len(names), args.min_entries))

    archive_mib = archive_bytes / 1048576.0
    print("initramfs       : found, %d entries, codec=%s" % (len(names), archive_codec))
    print("initramfs_size  : %d bytes (%.2f MiB uncompressed)" % (archive_bytes, archive_mib))
    print("initramfs_first : %s" % ", ".join(names[:5]))

    normalized = {name.removeprefix("./").lstrip("/") for name in names}
    required = {
        "init": "OpenWrt init",
        "etc/preinit": "preinit script",
        "bin/busybox": "BusyBox",
        "etc/config/network": "YSPEED network configuration",
        "usr/sbin/yspeed-display": "YSPEED display executable",
    }
    missing = [description for path, description in required.items() if path not in normalized]
    if missing:
        sys.exit("FAIL: initramfs is missing required files: " + ", ".join(missing))

    if archive_mib < args.min_mib:
        sys.exit(
            "FAIL: uncompressed initramfs is only %.2f MiB; minimum is %.1f MiB"
            % (archive_mib, args.min_mib)
        )

    dtb = raw.rfind(FDT_MAGIC)
    if dtb >= 0 and dtb + 8 <= len(raw):
        total = struct.unpack(">I", raw[dtb + 4:dtb + 8])[0]
        print("appended DTB    : offset 0x%x size %d" % (dtb, total))
    else:
        print("appended DTB    : not found")

    print("result          : PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
