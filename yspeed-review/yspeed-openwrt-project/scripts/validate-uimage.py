#!/usr/bin/env python3
import argparse, hashlib, os, struct, sys, zlib

p = argparse.ArgumentParser(description="Validate a YSPEED RAM-boot legacy uImage")
p.add_argument("image")
p.add_argument("--max-size", type=int, default=0x01fb0000,
               help="informational firmware partition size; RAM image is not written to flash")
a = p.parse_args()
data = open(a.image, "rb").read()
if len(data) < 64:
    sys.exit("FAIL: file shorter than legacy uImage header")
fields = struct.unpack(">7I4B", data[:32])
magic, hcrc, timestamp, size, load, entry, dcrc, os_id, arch, image_type, comp = fields
header = bytearray(data[:64])
header[4:8] = b"\0\0\0\0"
actual_hcrc = zlib.crc32(header) & 0xffffffff
payload = data[64:64+size]
actual_dcrc = zlib.crc32(payload) & 0xffffffff
checks = [
    (magic == 0x27051956, f"legacy uImage magic 0x{magic:08x}"),
    (hcrc == actual_hcrc, f"header CRC stored={hcrc:08x} computed={actual_hcrc:08x}"),
    (size == len(payload), f"payload size header={size} actual={len(payload)}"),
    (len(data) == 64 + size, f"file length={len(data)} header+payload={64+size}"),
    (dcrc == actual_dcrc, f"data CRC stored={dcrc:08x} computed={actual_dcrc:08x}"),
    (os_id == 5, f"OS id={os_id} (Linux=5)"),
    (arch == 5, f"architecture={arch} (MIPS=5)"),
    (image_type == 2, f"image type={image_type} (kernel=2)"),
    (comp == 3, f"compression={comp} (LZMA=3)"),
    (load == 0x80000000, f"load address=0x{load:08x}"),
    (entry == 0x80000000, f"entry address=0x{entry:08x}"),
]
for ok, desc in checks:
    print(("PASS: " if ok else "FAIL: ") + desc)
print(f"file_bytes={len(data)}")
print(f"payload_bytes={size}")
print(f"sha256={hashlib.sha256(data).hexdigest()}")
print(f"firmware_partition_bytes={a.max_size} (reference only; this artifact is RAM-boot-only)")
print("flash_write_permitted=no")
if not all(ok for ok, _ in checks):
    sys.exit(1)
