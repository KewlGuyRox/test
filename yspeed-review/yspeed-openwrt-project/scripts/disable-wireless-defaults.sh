#!/bin/sh
set -eu

TOPDIR=${1:?Usage: disable-wireless-defaults.sh OPENWRT_SOURCE_DIR}
TARGET_MK="$TOPDIR/target/linux/ramips/mt76x8/target.mk"
DEFAULT_LINE='DEFAULT_PACKAGES += kmod-mt7603 wpad-basic-mbedtls swconfig'
WIRED_LINE='DEFAULT_PACKAGES += swconfig'

[ -f "$TARGET_MK" ] || { echo "Missing MT76x8 target defaults: $TARGET_MK" >&2; exit 2; }

if grep -qF "$WIRED_LINE" "$TARGET_MK" && ! grep -qF "$DEFAULT_LINE" "$TARGET_MK"; then
    echo "MT76x8 defaults already restricted to wired switch support"
elif grep -qF "$DEFAULT_LINE" "$TARGET_MK"; then
    sed "s|$DEFAULT_LINE|$WIRED_LINE|" "$TARGET_MK" > "$TARGET_MK.tmp"
    mv "$TARGET_MK.tmp" "$TARGET_MK"
    echo "Removed MT76x8 default Wi-Fi packages; retained swconfig"
else
    echo "Unexpected MT76x8 DEFAULT_PACKAGES definition; refusing an ambiguous edit" >&2
    grep -n 'DEFAULT_PACKAGES' "$TARGET_MK" >&2 || true
    exit 3
fi

if grep -Eq '^[[:space:]]*DEFAULT_PACKAGES.*(kmod-mt7603|wpad)' "$TARGET_MK"; then
    echo "Wireless default remains in MT76x8 target configuration" >&2
    exit 4
fi
