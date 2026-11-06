#!/bin/sh
set -eu
TOPDIR=$1
PROFILE_FILE="$TOPDIR/target/linux/ramips/image/mt76x8.mk"
MARKER='define Device/yspeed_ys-gps-12832'
if grep -qF "$MARKER" "$PROFILE_FILE"; then
    echo "YSPEED profile already present"
    exit 0
fi
cat >> "$PROFILE_FILE" <<'PROFILE'

# YSPEED YS-GPS-12832 — custom wired-only MT7628AN GNSS/NTP appliance.
define Device/yspeed_ys-gps-12832
  IMAGE_SIZE := 32448k
  DEVICE_VENDOR := YSPEED
  DEVICE_MODEL := YS-GPS-12832
  DEVICE_DTS := mt7628an_yspeed_ys-gps-12832
  DEVICE_PACKAGES := gpsd gpsd-clients chrony kmod-pps-gpio kmod-i2c-mt7628 kmod-rtc-ds1307 kmod-usb2 kmod-usb-ohci \
                     luci-light luci-mod-network luci-mod-status luci-mod-system \
                     luci-ssl luci-theme-bootstrap
  SUPPORTED_DEVICES += yspeed,ys-gps-12832
  # RAM-boot build only: suppress sysupgrade/factory/flash image generation.
  IMAGES :=
endef
TARGET_DEVICES += yspeed_ys-gps-12832
PROFILE
