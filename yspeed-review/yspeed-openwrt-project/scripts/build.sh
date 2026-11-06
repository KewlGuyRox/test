#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="${1:-$PWD/yspeed-openwrt-25.12.6}"
SRC="$WORKDIR/openwrt"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
TAG="v25.12.6"
REPO="https://git.openwrt.org/openwrt/openwrt.git"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing host tool: $1" >&2; exit 2; }; }
for x in git make gcc g++ python3 rsync file sha256sum; do need "$x"; done

if [[ ! -d "$SRC/.git" ]]; then
  mkdir -p "$WORKDIR"
  git clone --branch "$TAG" --depth 1 "$REPO" "$SRC"
fi

git -C "$SRC" fetch --tags --depth 1 origin "$TAG"
git -C "$SRC" checkout --detach "$TAG"
ACTUAL_TAG="$(git -C "$SRC" describe --tags --exact-match HEAD)"
[[ "$ACTUAL_TAG" == "$TAG" ]] || { echo "Unexpected source tag: $ACTUAL_TAG" >&2; exit 3; }

cp "$PROJECT_DIR/feeds.conf" "$SRC/feeds.conf"
cp "$PROJECT_DIR/board/mt7628an_yspeed_ys-gps-12832.dts" \
   "$SRC/target/linux/ramips/dts/mt7628an_yspeed_ys-gps-12832.dts"
cp -a "$PROJECT_DIR/package/yspeed-display" "$SRC/package/"
sh "$PROJECT_DIR/scripts/append-profile.sh" "$SRC"
sh "$PROJECT_DIR/scripts/disable-wireless-defaults.sh" "$SRC"

# Build the framebuffer core and ST7789V modules without the generic
# kmod-fb package's @DISPLAY_SUPPORT dependency (not enabled by mt76x8).
KERNEL_CONFIG="$SRC/target/linux/ramips/mt76x8/config-6.12"
for entry in \
  'CONFIG_GPIOLIB=y' \
  'CONFIG_SPI_BITBANG=y' \
  'CONFIG_FB=y' \
  'CONFIG_FB_DEVICE=y' \
  'CONFIG_FB_DEFERRED_IO=y' \
  'CONFIG_FB_SYSMEM_FOPS=y' \
  'CONFIG_FB_SYS_COPYAREA=y' \
  'CONFIG_FB_SYS_FILLRECT=y' \
  'CONFIG_FB_SYS_IMAGEBLIT=y' \
  'CONFIG_BACKLIGHT_CLASS_DEVICE=y' \
  'CONFIG_FB_BACKLIGHT=y' \
  'CONFIG_FONTS=y' \
  'CONFIG_FONT_8x8=y' \
  'CONFIG_FONT_8x16=y' \
  'CONFIG_FB_TFT=m' \
  'CONFIG_FB_TFT_ST7789V=m'; do
  grep -qxF "$entry" "$KERNEL_CONFIG" || printf '%s\n' "$entry" >> "$KERNEL_CONFIG"
done

VIDEO_MK="$SRC/package/kernel/linux/modules/video.mk"
if ! grep -qF 'KernelPackage/fb-tft-yspeed' "$VIDEO_MK"; then
cat >> "$VIDEO_MK" <<'ST7789_PACKAGE'

define KernelPackage/fb-tft-yspeed
  SUBMENU:=$(VIDEO_MENU)
  TITLE:=FBTFT core for YSPEED ST7789 panel
  KCONFIG:=CONFIG_FB_TFT
  FILES:=$(LINUX_DIR)/drivers/staging/fbtft/fbtft.ko
  AUTOLOAD:=$(call AutoLoad,08,fbtft)
endef

define KernelPackage/fb-tft-yspeed/description
  Framebuffer core for the YSPEED ribbon-connected ST7789V panel.
endef

$(eval $(call KernelPackage,fb-tft-yspeed))

define KernelPackage/fb-tft-st7789v-yspeed
  SUBMENU:=$(VIDEO_MENU)
  TITLE:=FB driver for the Sitronix ST7789V LCD controller
  DEPENDS:=+kmod-fb-tft-yspeed
  KCONFIG:=CONFIG_FB_TFT_ST7789V
  FILES:=$(LINUX_DIR)/drivers/staging/fbtft/fb_st7789v.ko
  AUTOLOAD:=$(call AutoLoad,09,fb_st7789v)
endef

define KernelPackage/fb-tft-st7789v-yspeed/description
  Framebuffer driver for the ribbon-connected 240x320 ST7789V panel.
endef

$(eval $(call KernelPackage,fb-tft-st7789v-yspeed))
ST7789_PACKAGE
fi

# Pin feed revisions using the official 25.12.6 feeds.buildinfo snapshot.
(cd "$SRC" && ./scripts/feeds update packages luci && \
 ./scripts/feeds install -p packages gpsd gpsd-clients chrony && \
 ./scripts/feeds install -p luci luci-light luci-mod-network luci-mod-status luci-mod-system luci-ssl luci-theme-bootstrap)

cp "$PROJECT_DIR/config/ramips-mt76x8-yspeed.config" "$SRC/.config"
# Use the hosted runner's persistent ccache directory for repeatable builds.
grep -qxF 'CONFIG_CCACHE=y' "$SRC/.config" || printf '%s\n' 'CONFIG_CCACHE=y' >> "$SRC/.config"
cp -a "$PROJECT_DIR/files/." "$SRC/files/"

# Enforce the owner's network contract before resolving the OpenWrt config:
# eth0.2 is the only Layer-3 interface and receives its address via DHCP.
NETWORK_CONFIG="$PROJECT_DIR/files/etc/config/network"
# Verify the WAN logical interface maps to eth0.2 and uses DHCP. Ignore the
# loopback interface's required 127.0.0.1 static address.
awk '
function uq(v) { gsub(/^["\047]|["\047]$/, "", v); return v }
/^config / { section=$2; name=uq($3); next }
section == "interface" && name == "wan" && $1 == "option" {
  if ($2 == "device" && uq($3) == "eth0.2") device_ok=1
  if ($2 == "proto" && uq($3) == "dhcp") proto_ok=1
}
section == "interface" && name == "lan" { bad=1 }
section == "interface" && name != "loopback" && $1 == "option" && $2 == "ipaddr" { bad=1 }
section == "interface" && name != "loopback" && $1 == "option" &&
  ($2 == "device" || $2 == "ifname") && uq($3) == "eth0.1" { bad=1 }
END { if (!device_ok || !proto_ok || bad) exit 1 }
' "$NETWORK_CONFIG" || {
  echo "Network contract violation: require DHCP on eth0.2 only; no eth0.1 or other static L3 interface" >&2
  exit 4
}
make -C "$SRC" defconfig

# Hard guard: this project must not produce a flash/sysupgrade/factory image.
if ! grep -q '^CONFIG_TARGET_ROOTFS_INITRAMFS=y$' "$SRC/.config"; then
  echo "INITRAMFS configuration did not survive defconfig" >&2; exit 4
fi
if ! grep -q '^CONFIG_TARGET_DEVICE_ramips_mt76x8_DEVICE_yspeed_ys-gps-12832=y$' "$SRC/.config"; then
  echo "YSPEED profile is not selected in .config" >&2; exit 4
fi
if ! grep -q '^CONFIG_CCACHE=y$' "$SRC/.config"; then
  echo "Compiler cache support was dropped by defconfig" >&2; exit 4
fi
for package in kmod-fb-tft-yspeed kmod-fb-tft-st7789v-yspeed yspeed-display kmod-gpio-button-hotplug dropbear uhttpd; do
  if ! grep -q "^CONFIG_PACKAGE_${package}=y$" "$SRC/.config"; then
    echo "Required ST7789/display package was dropped by defconfig: $package" >&2
    exit 4
  fi
done
if grep -Eq '^CONFIG_PACKAGE_(wpad[^=]*|iw|kmod-mt7603(e)?|kmod-mac80211|kmod-mt76-core|wireless-regdb)=(y|m)$' "$SRC/.config"; then
  echo "Wireless package selected in wired-only build config" >&2
  grep -E '^CONFIG_PACKAGE_(wpad[^=]*|iw|kmod-mt7603(e)?|kmod-mac80211|kmod-mt76-core|wireless-regdb)=(y|m)$' "$SRC/.config" >&2 || true
  exit 4
fi

make -C "$SRC" -j"$JOBS" V=s

OUT="$SRC/bin/targets/ramips/mt76x8"
IMAGE="$(find "$OUT" -maxdepth 1 -type f -name '*yspeed_ys-gps-12832-initramfs-kernel.bin' -print -quit)"
[[ -n "$IMAGE" && -f "$IMAGE" ]] || { echo "Expected initramfs uImage not found under $OUT" >&2; exit 5; }

# Verify compiled DTB with the host dtc built by OpenWrt.
DTC="$SRC/staging_dir/host/bin/dtc"
if [[ ! -x "$DTC" ]]; then
  DTC="$(find "$SRC/build_dir/target-mipsel_24kc_musl/linux-ramips_mt76x8" \
    -type f -path '*/scripts/dtc/dtc' -perm -111 -print -quit 2>/dev/null)"
fi
DTB="$(find "$SRC/build_dir" -type f -name 'image-mt7628an_yspeed_ys-gps-12832.dtb' -print -quit)"
[[ -x "$DTC" && -n "$DTB" ]] || { echo "Compiled DTB or host dtc not found" >&2; exit 6; }
mkdir -p "$WORKDIR/artifacts"
"$DTC" -I dtb -O dts "$DTB" > "$WORKDIR/artifacts/yspeed-compiled.dts"
for token in 'YSPEED YS-GPS-12832' 'pps-gpio' 'dallas,ds1307' 'denx,uimage' 'yspeed,ys-gps-12832' 'mac-address@2e' 'sitronix,st7789v' 'display@1' 'pinctrl-0' 'LCD Up' 'LCD Enter' 'LCD Down'; do
  grep -qF "$token" "$WORKDIR/artifacts/yspeed-compiled.dts" || { echo "DTB check missing: $token" >&2; exit 7; }
done

cp "$IMAGE" "$WORKDIR/artifacts/yspeed-ramboot.uImage"
python3 "$PROJECT_DIR/scripts/validate-uimage.py" "$WORKDIR/artifacts/yspeed-ramboot.uImage" \
  | tee "$WORKDIR/artifacts/uimage-validation.txt"

# Enforce that no flashable output is published by this RAM-only profile.
if find "$OUT" -maxdepth 1 -type f \( -name '*yspeed*sysupgrade*' -o -name '*yspeed*factory*' \) | grep -q .; then
  echo "Unexpected flashable YSPEED image was generated; refusing to publish." >&2
  exit 8
fi

{
  echo "OpenWrt tag: $TAG"
  echo "OpenWrt commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "Packages feed: $(git -C "$SRC/feeds/packages" rev-parse HEAD)"
  echo "LuCI feed: $(git -C "$SRC/feeds/luci" rev-parse HEAD)"
  echo "DTS SHA256: $(sha256sum "$PROJECT_DIR/board/mt7628an_yspeed_ys-gps-12832.dts" | awk '{print $1}')"
  echo "uImage SHA256: $(sha256sum "$WORKDIR/artifacts/yspeed-ramboot.uImage" | awk '{print $1}')"
  echo "Build UTC: $(date -u +%FT%TZ)"
  echo "Output is RAM-boot-only; do not flash."
} > "$WORKDIR/artifacts/build-manifest.txt"

echo "Build complete. RAM-only artifact: $WORKDIR/artifacts/yspeed-ramboot.uImage"
echo "Compiled DTB source dump: $WORKDIR/artifacts/yspeed-compiled.dts"
echo "No sysupgrade/factory image is published."
