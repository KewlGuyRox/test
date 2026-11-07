#!/bin/sh
set -eu

TOPDIR=${1:?Usage: disable-wireless-defaults.sh OPENWRT_SOURCE_DIR}
TARGET_MK="$TOPDIR/target/linux/ramips/mt76x8/target.mk"
DEFAULT_LINE='DEFAULT_PACKAGES += kmod-mt7603 wpad-basic-mbedtls swconfig'
WIRED_LINE='DEFAULT_PACKAGES += swconfig'

# ramips (parent target) adds gpio-button-hotplug globally, before the
# mt76x8 subtarget and device profile are resolved. That package replaces
# gpio_keys and conflicts with this board's evdev-based front-panel keys.
# Remove only that parent default in this disposable single-board build.
RAMIPS_MK="$TOPDIR/target/linux/ramips/Makefile"
RAMIPS_DEFAULT='DEFAULT_PACKAGES += kmod-leds-gpio kmod-gpio-button-hotplug'
RAMIPS_WIRED='DEFAULT_PACKAGES += kmod-leds-gpio'
[ -f "$RAMIPS_MK" ] || { echo "Missing ramips target defaults: $RAMIPS_MK" >&2; exit 2; }
if grep -qF "$RAMIPS_DEFAULT" "$RAMIPS_MK"; then
    sed "s|$RAMIPS_DEFAULT|$RAMIPS_WIRED|" "$RAMIPS_MK" > "$RAMIPS_MK.tmp"
    mv "$RAMIPS_MK.tmp" "$RAMIPS_MK"
elif ! grep -qF 'kmod-gpio-button-hotplug' "$RAMIPS_MK"; then
    echo "ramips parent default already excludes gpio-button-hotplug"
else
    echo "Unexpected ramips gpio-button-hotplug default; refusing ambiguous edit" >&2
    grep -n 'gpio-button-hotplug' "$RAMIPS_MK" >&2 || true
    exit 5
fi

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


# The MT7628AN board has GPIO and SPI-attached display hardware. Advertise
# those target capabilities so the framebuffer/backlight kernel packages,
# whose OpenWrt dependencies are @GPIO_SUPPORT and @DISPLAY_SUPPORT, can be
# selected by the YSPEED profile.
if ! grep -Eq '^FEATURES.*(^|[[:space:]])gpio([[:space:]]|$)' "$TARGET_MK"; then
    printf '%s\n' 'FEATURES += gpio' >> "$TARGET_MK"
fi
if ! grep -Eq '^FEATURES.*(^|[[:space:]])display([[:space:]]|$)' "$TARGET_MK"; then
    printf '%s\n' 'FEATURES += display' >> "$TARGET_MK"
fi

if grep -Eq '^[[:space:]]*DEFAULT_PACKAGES.*(kmod-mt7603|wpad)' "$TARGET_MK"; then
    echo "Wireless default remains in MT76x8 target configuration" >&2
    exit 4
fi
