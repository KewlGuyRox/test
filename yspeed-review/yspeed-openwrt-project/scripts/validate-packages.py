#!/usr/bin/env python3
"""Fail fast when a device profile names a package that does not exist.

The only build failure this project has ever produced was at image assembly,
after ~40 minutes of compilation: DEVICE_PACKAGES listed kmod-fb-tft-yspeed,
which no package directory defines, so apk aborted with "no such package".

Running this straight after `make defconfig` turns that 40-minute failure into
a few-second one. It reads the package index OpenWrt has already generated
(tmp/.packageinfo and tmp/.targetinfo) and cross-checks the profile.
"""
import os
import re
import sys

PROFILE_REL = "target/linux/ramips/image/mt76x8.mk"
DEVICE_MATCH = "Device/yspeed_ys-gps-12832"


def read_names(path):
    """Return the set of Package:/Provides: names declared in a .packageinfo file."""
    names = set()
    if not os.path.exists(path):
        return names
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("Package:") or line.startswith("Provides:"):
                for token in line.split(":", 1)[1].split():
                    names.add(token.strip())
    return names


def device_packages(src):
    """Extract the continued DEVICE_PACKAGES assignment from the YSPEED device definition."""
    path = os.path.join(src, PROFILE_REL)
    if not os.path.exists(path):
        sys.exit("FAIL: profile file not found: %s" % path)

    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if DEVICE_MATCH in line)
    except StopIteration:
        sys.exit("FAIL: %s block not found in %s" % (DEVICE_MATCH, PROFILE_REL))

    assignment = re.compile(r"^\\s*DEVICE_PACKAGES\\s*:?=\\s*(.*)$")
    for i in range(start, len(lines)):
        match = assignment.match(lines[i])
        if not match:
            continue
        parts = []
        value = match.group(1)
        while True:
            continued = value.rstrip().endswith("\\")
            if continued:
                value = value.rstrip()[:-1]
            parts.append(value.split("#", 1)[0])
            if not continued or i + 1 >= len(lines):
                break
            i += 1
            value = lines[i].strip()
        tokens = " ".join(parts).split()
        # A leading '-' means "remove this from the defaults"; it need not exist.
        return [token for token in tokens if not token.startswith("-")]

    return []

def main():
    if len(sys.argv) != 2:
        sys.exit("usage: validate-packages.py <openwrt-source-dir>")
    src = sys.argv[1]

    wanted = device_packages(src)
    if not wanted:
        print("package check   : no DEVICE_PACKAGES entries found, skipping")
        return 0

    available = set()
    for name in (".packageinfo", ".targetinfo"):
        available |= read_names(os.path.join(src, "tmp", name))

    if not available:
        print("package check   : SKIPPED (tmp/.packageinfo missing)")
        print("                  run 'make defconfig' before this check")
        return 0

    missing = [p for p in wanted if p not in available]

    print("package check   : %d declared, %d in index" % (len(wanted), len(available)))
    if missing:
        print("MISSING PACKAGES:")
        for p in missing:
            print("  %s" % p)
        print("")
        print("These names appear in DEVICE_PACKAGES but no package provides them.")
        print("Image assembly would fail after the full compile.")
        return 1

    print("package check   : all DEVICE_PACKAGES resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
