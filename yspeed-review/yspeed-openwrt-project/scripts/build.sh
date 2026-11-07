#!/usr/bin/env bash
set -Eeuo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WORKDIR="$1"; [ -n "$WORKDIR" ] || WORKDIR="$PWD/yspeed-openwrt-25.12.6"
SRC="$WORKDIR/openwrt"; JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
TAG="v25.12.6"; REPO="https://git.openwrt.org/openwrt/openwrt.git"
need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing host tool: $1" >&2; exit 2; }; }
for x in git make gcc g++ python3 rsync file sha256sum; do need "$x"; done
if [[ ! -d "$SRC/.git" ]]; then
  mkdir -p "$WORKDIR"; CACHED_DL="$WORKDIR/.cached-openwrt-dl"
  if [[ -d "$SRC/dl" ]]; then rm -rf "$CACHED_DL"; mv "$SRC/dl" "$CACHED_DL"; fi
  if [[ -d "$SRC" ]]; then rmdir "$SRC" 2>/dev/null || { echo "Unexpected non-cache files in $SRC" >&2; exit 2; }; fi
  git clone --branch "$TAG" --depth 1 "$REPO" "$SRC"
  [[ ! -d "$CACHED_DL" ]] || mv "$CACHED_DL" "$SRC/dl"
fi
git -C "$SRC" fetch --tags --depth 1 origin "$TAG"
git -C "$SRC" checkout --detach "$TAG"
[[ "$(git -C "$SRC" describe --tags --exact-match HEAD)" == "$TAG" ]] || { echo "Unexpected OpenWrt source revision" >&2; exit 3; }
cp "$PROJECT_DIR/feeds.conf" "$SRC/feeds.conf"
cp "$PROJECT_DIR/board/mt7628an_yspeed_ys-gps-12832.dts" "$SRC/target/linux/ramips/dts/mt7628an_yspeed_ys-gps-12832.dts"
cp -a "$PROJECT_DIR/package/yspeed-display" "$SRC/package/"
sh "$PROJECT_DIR/scripts/append-profile.sh" "$SRC"
sh "$PROJECT_DIR/scripts/disable-wireless-defaults.sh" "$SRC"

# Native MT7628 SPI master, spidev, and legacy GPIO sysfs.
KERNEL_CONFIG="$SRC/target/linux/ramips/mt76x8/config-6.12"
for entry in 'CONFIG_SPI=y' 'CONFIG_SPI_MASTER=y' 'CONFIG_SPI_MT7621=y' 'CONFIG_SPI_SPIDEV=y' 'CONFIG_GPIOLIB=y' 'CONFIG_GPIO_SYSFS=y'; do
  grep -qxF "$entry" "$KERNEL_CONFIG" || printf '%s\n' "$entry" >> "$KERNEL_CONFIG"
done
(cd "$SRC" && ./scripts/feeds update packages luci && \
 ./scripts/feeds install -p packages gpsd gpsd-clients chrony && \
 ./scripts/feeds install -p luci luci-light luci-mod-network luci-mod-status luci-mod-system luci-ssl luci-theme-bootstrap)
cp "$PROJECT_DIR/config/ramips-mt76x8-yspeed.config" "$SRC/.config"
cp -a "$PROJECT_DIR/files/." "$SRC/files/"
for dir in "$SRC/files/etc/uci-defaults" "$SRC/files/etc/hotplug.d/iface" "$SRC/files/usr/sbin" "$SRC/files/www/cgi-bin"; do
  [ ! -d "$dir" ] || find "$dir" -type f -exec chmod 0755 {} + 2>/dev/null || true
done

# DHCP on eth0.2 only; no static management address or eth0.1 L3 interface.
NETWORK_CONFIG="$PROJECT_DIR/files/etc/config/network"
awk '
function uq(v) { gsub(/^[\"\\047]|[\"\\047]$/, \"\", v); return v }
/^config / { section=$2; name=uq($3); next }
section == "interface" && name == "wan" && $1 == "option" {
  if ($2 == "device" && uq($3) == "eth0.2") device_ok=1
  if ($2 == "proto" && uq($3) == "dhcp") proto_ok=1
}
section == "interface" && name == "lan" { bad=1 }
section == "interface" && name != "loopback" && $1 == "option" && $2 == "ipaddr" { bad=1 }
section == "interface" && name != "loopback" && $1 == "option" && ($2 == "device" || $2 == "ifname") && uq($3) == "eth0.1" { bad=1 }
END { if (!device_ok || !proto_ok || bad) exit 1 }
' "$NETWORK_CONFIG" || { echo "Network contract violation: DHCP on eth0.2 only" >&2; exit 4; }

make -C "$SRC" defconfig
make -C "$SRC" target/linux/prepare V=s
SPIDEV_SOURCE="$(find "$SRC/build_dir" -type f -path '*/drivers/spi/spidev.c' -print -quit)"
[[ -n "$SPIDEV_SOURCE" && -f "$SPIDEV_SOURCE" ]] || { echo "Linux spidev.c not found" >&2; exit 4; }
python3 - "$SPIDEV_SOURCE" <<'PY'
import pathlib, sys
p=pathlib.Path(sys.argv[1]); s=p.read_text()
entry='{ .compatible = "yspeed,st7789-spidev", .data = &spidev_of_check },'
if entry not in s:
    anchor='static const struct of_device_id spidev_dt_ids[] = {'
    if s.count(anchor)!=1: raise SystemExit("Cannot uniquely locate spidev OF match table")
    s=s.replace(anchor,anchor+'\n\t'+entry,1)
p.write_text(s)
if entry not in p.read_text(): raise SystemExit("YSPEED spidev match patch failed")
PY
make -C "$SRC" target/linux/compile V=s -j"$JOBS"
KERNEL_DOTCONFIG="$(find "$SRC/build_dir" -type f -path '*/linux-*/.config' -print -quit)"
[[ -n "$KERNEL_DOTCONFIG" ]] || { echo "Resolved kernel .config not found" >&2; exit 5; }
for symbol in CONFIG_SPI_SPIDEV CONFIG_GPIO_SYSFS CONFIG_SPI_MT7621; do
  grep -qx "$symbol=y" "$KERNEL_DOTCONFIG" || { echo "Required kernel symbol missing: $symbol" >&2; exit 5; }
done
grep -qx 'CONFIG_TARGET_ROOTFS_INITRAMFS=y' "$SRC/.config" || { echo "INITRAMFS profile missing" >&2; exit 6; }
grep -qx 'CONFIG_TARGET_DEVICE_ramips_mt76x8_DEVICE_yspeed_ys-gps-12832=y' "$SRC/.config" || { echo "YSPEED profile missing" >&2; exit 6; }
for package in yspeed-display dropbear uhttpd gpsd gpsd-clients chrony jsonfilter kmod-input-gpio-keys kmod-input-evdev; do
  grep -qx "CONFIG_PACKAGE_$package=y" "$SRC/.config" || { echo "Required package dropped: $package" >&2; exit 6; }
done
if grep -q '^CONFIG_PACKAGE_kmod-gpio-button-hotplug=y$' "$SRC/.config"; then echo "Conflicting gpio-button-hotplug selected" >&2; exit 6; fi
if grep -Eq '^CONFIG_PACKAGE_(wpad[^=]*|iw|kmod-mt7603(e)?|kmod-mac80211|kmod-mt76-core|wireless-regdb)=(y|m)$' "$SRC/.config"; then echo "Wireless package selected" >&2; exit 6; fi
make -C "$SRC" -j"$JOBS" V=s

OUT="$SRC/bin/targets/ramips/mt76x8"
IMAGE="$(find "$OUT" -maxdepth 1 -type f -name '*yspeed_ys-gps-12832-initramfs-kernel.bin' -print -quit)"
[[ -n "$IMAGE" && -f "$IMAGE" ]] || { echo "Expected initramfs uImage not found" >&2; exit 7; }
DTC="$SRC/staging_dir/host/bin/dtc"
DTB="$(find "$SRC/build_dir" -type f -name 'image-mt7628an_yspeed_ys-gps-12832.dtb' -print -quit)"
[[ -x "$DTC" && -n "$DTB" ]] || { echo "Compiled DTB or host dtc not found" >&2; exit 8; }
mkdir -p "$WORKDIR/artifacts"
"$DTC" -I dtb -O dts "$DTB" > "$WORKDIR/artifacts/yspeed-compiled.dts"
for token in 'YSPEED YS-GPS-12832' 'pps-gpio' 'dallas,ds1307' 'denx,uimage' 'yspeed,st7789-spidev' 'st7789@1' 'spi_cs1' 'LCD Up' 'LCD Enter' 'LCD Down'; do
  grep -qF "$token" "$WORKDIR/artifacts/yspeed-compiled.dts" || { echo "Compiled DTB missing: $token" >&2; exit 8; }
done
if grep -Eq 'sitronix,st7789v|wealth,sh1106|display@1' "$WORKDIR/artifacts/yspeed-compiled.dts"; then echo "Obsolete LCD binding remains" >&2; exit 8; fi
cp "$IMAGE" "$WORKDIR/artifacts/yspeed-ramboot.uImage"
python3 "$PROJECT_DIR/scripts/validate-uimage.py" "$WORKDIR/artifacts/yspeed-ramboot.uImage" | tee "$WORKDIR/artifacts/uimage-validation.txt"

# Overlay update excludes network and firewall files.
UPDATE_ROOT="$WORKDIR/yspeed-rack-update-root"; rm -rf "$UPDATE_ROOT"
mkdir -p "$UPDATE_ROOT/etc/chrony" "$UPDATE_ROOT/etc/hotplug.d/iface" "$UPDATE_ROOT/usr/sbin" "$UPDATE_ROOT/www/cgi-bin" "$UPDATE_ROOT/www/yspeed"
cp "$PROJECT_DIR/files/etc/chrony/chrony.conf" "$UPDATE_ROOT/etc/chrony/"
cp "$PROJECT_DIR/files/etc/hotplug.d/iface/95-yspeed-chrony" "$UPDATE_ROOT/etc/hotplug.d/iface/"
cp "$PROJECT_DIR/files/usr/sbin/yspeed-status" "$UPDATE_ROOT/usr/sbin/"
cp "$PROJECT_DIR/files/usr/sbin/yspeed-apply-update" "$UPDATE_ROOT/usr/sbin/"
cp "$PROJECT_DIR/files/www/cgi-bin/yspeed-status" "$UPDATE_ROOT/www/cgi-bin/"
cp "$PROJECT_DIR/files/www/yspeed/index.html" "$UPDATE_ROOT/www/yspeed/"
tar -czf "$WORKDIR/artifacts/yspeed-rack-update.tar.gz" -C "$UPDATE_ROOT" .
sha256sum "$WORKDIR/artifacts/yspeed-rack-update.tar.gz" > "$WORKDIR/artifacts/yspeed-rack-update.tar.gz.sha256"
if find "$OUT" -maxdepth 1 -type f \( -name '*yspeed*sysupgrade*' -o -name '*yspeed*factory*' \) | grep -q .; then echo "Flashable image unexpectedly generated" >&2; exit 9; fi
{
 echo "OpenWrt tag: $TAG"
 echo "OpenWrt commit: $(git -C "$SRC" rev-parse HEAD)"
 echo "Packages feed: $(git -C "$SRC/feeds/packages" rev-parse HEAD)"
 echo "LuCI feed: $(git -C "$SRC/feeds/luci" rev-parse HEAD)"
 echo "DTS SHA256: $(sha256sum "$PROJECT_DIR/board/mt7628an_yspeed_ys-gps-12832.dts" | awk '{print $1}')"
 echo "uImage SHA256: $(sha256sum "$WORKDIR/artifacts/yspeed-ramboot.uImage" | awk '{print $1}')"
 echo "Display: userspace SPI0 CS1 /dev/spidev0.1"
 echo "Build UTC: $(date -u +%FT%TZ)"
 echo "RAM-boot-only; do not flash."
} > "$WORKDIR/artifacts/build-manifest.txt"
echo "Build complete: $WORKDIR/artifacts/yspeed-ramboot.uImage"
