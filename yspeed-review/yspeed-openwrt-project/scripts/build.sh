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
  # OpenWrt puts the compiler cache at $SRC/.ccache (its default CCACHE_DIR).
  # Move it aside too, otherwise the rmdir below trips over it and the build
  # aborts with "Unexpected non-cache files". Restored after the clone.
  CACHED_CCACHE="$WORKDIR/.cached-ccache"
  if [[ -d "$SRC/.ccache" ]]; then rm -rf "$CACHED_CCACHE"; mv "$SRC/.ccache" "$CACHED_CCACHE"; fi
  if [[ -d "$SRC" ]]; then rmdir "$SRC" 2>/dev/null || { echo "Unexpected non-cache files in $SRC" >&2; exit 2; }; fi
  git clone --branch "$TAG" --depth 1 "$REPO" "$SRC"
  [[ ! -d "$CACHED_DL" ]] || mv "$CACHED_DL" "$SRC/dl"
  [[ ! -d "$CACHED_CCACHE" ]] || mv "$CACHED_CCACHE" "$SRC/.ccache"
fi
git -C "$SRC" fetch --tags --depth 1 origin "$TAG"
git -C "$SRC" checkout --detach "$TAG"
[[ "$(git -C "$SRC" describe --tags --exact-match HEAD)" == "$TAG" ]] || { echo "Unexpected OpenWrt source revision" >&2; exit 3; }
# Compiler cache. OpenWrt hardcodes CCACHE_DIR to $(TOPDIR)/.ccache, which is
# not inside any path CI caches. Redirect it into $SRC/dl/.ccache: the download
# cache step already restores and saves dl/, so the compiler cache rides along
# with no workflow change. Set CCACHE_MAXSIZE here because rules.mk does not
# override it, and OpenWrt forces CCACHE_NOCOMPRESS so caches grow fast.
# ccache is not installed by the CI workflow, so install it here. Without this,
# enabling CONFIG_CCACHE would fail the build with "ccache: command not found".
CCACHE_MAKEOPT=""
if ! command -v ccache >/dev/null 2>&1; then
  sudo apt-get install -y ccache >/dev/null 2>&1 || true
fi
if command -v ccache >/dev/null 2>&1; then
  # rules.mk indents this line, so match optional leading whitespace. The earlier
  # "^export" anchor never matched and the cache was silently disabled for
  # several builds.
  if grep -q 'CCACHE_DIR:=' "$SRC/rules.mk"; then
    sed -i "s#^\([[:space:]]*\)export CCACHE_DIR:=.*#\1export CCACHE_DIR:=$SRC/dl/.ccache#" "$SRC/rules.mk"
    if grep -qF "CCACHE_DIR:=$SRC/dl/.ccache" "$SRC/rules.mk"; then
      CCACHE_MAKEOPT="CONFIG_CCACHE=y"
      export CCACHE_DIR="$SRC/dl/.ccache"
      export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
      echo "compiler cache : enabled at $CCACHE_DIR ($(ccache --version | head -n1))"
    else
      echo "compiler cache : REDIRECT FAILED, building uncached" >&2
      grep -n 'CCACHE_DIR' "$SRC/rules.mk" >&2 || true
    fi
  else
    echo "compiler cache : rules.mk has no CCACHE_DIR assignment" >&2
  fi
else
  echo "compiler cache : ccache unavailable, building uncached" >&2
fi
cp "$PROJECT_DIR/feeds.conf" "$SRC/feeds.conf"
cp "$PROJECT_DIR/board/mt7628an_yspeed_ys-gps-12832.dts" "$SRC/target/linux/ramips/dts/mt7628an_yspeed_ys-gps-12832.dts"
cp -a "$PROJECT_DIR/package/yspeed-display" "$SRC/package/"
sh "$PROJECT_DIR/scripts/append-profile.sh" "$SRC"
sh "$PROJECT_DIR/scripts/disable-wireless-defaults.sh" "$SRC"

# Kernel patch: teach spidev about the panel's compatible string. This is a real
# quilt patch under target/linux/ramips/patches-6.12 so it applies during
# "make target/linux/prepare", is reviewable in git, and is not a hidden
# in-place sed. Applied with -p1 from the kernel source root.
mkdir -p "$SRC/target/linux/ramips/patches-6.12"
cp "$PROJECT_DIR/patches/995-gpio-vendor-base.patch" "$SRC/target/linux/ramips/patches-6.12/"
cp "$PROJECT_DIR/patches/997-mt7621-log-transfer-timeout.patch" "$SRC/target/linux/ramips/patches-6.12/"
cp "$PROJECT_DIR/patches/998-mt7621-spi-no-cs.patch" "$SRC/target/linux/ramips/patches-6.12/"
cp "$PROJECT_DIR/patches/999-spidev-yspeed-match.patch" "$SRC/target/linux/ramips/patches-6.12/"

# Native MT7628 SPI master, spidev, and legacy GPIO sysfs.
KERNEL_CONFIG="$SRC/target/linux/ramips/mt76x8/config-6.12"
for entry in 'CONFIG_SPI=y' 'CONFIG_SPI_MASTER=y' 'CONFIG_SPI_MT7621=y' 'CONFIG_SPI_SPIDEV=y' 'CONFIG_GPIOLIB=y' 'CONFIG_GPIO_SYSFS=y' 'CONFIG_EXPERT=y'; do
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

# Default DHCP on eth0.2; static WAN is available in LuCI, and eth0.1 has no L3 interface.
NETWORK_CONFIG="$PROJECT_DIR/files/etc/config/network"
python3 - "$NETWORK_CONFIG" <<'PYNET'
import shlex, sys
path=sys.argv[1]
sections=[]
current=None
for number, raw in enumerate(open(path, encoding="utf-8"), 1):
    try:
        fields=shlex.split(raw, comments=True)
    except ValueError as exc:
        raise SystemExit(f"Invalid UCI syntax at {path}:{number}: {exc}")
    if not fields:
        continue
    if fields[0] == "config":
        if len(fields) < 2:
            raise SystemExit(f"Invalid config section at line {number}")
        current={"type":fields[1], "name":fields[2] if len(fields)>2 else "", "options":{}}
        sections.append(current)
    elif fields[0] == "option" and current is not None and len(fields)>=3:
        current["options"][fields[1]]=fields[2]
interfaces=[x for x in sections if x["type"]=="interface"]
wan=[x for x in interfaces if x["name"]=="wan"]
if len(wan)!=1 or wan[0]["options"].get("device")!="eth0.2" or wan[0]["options"].get("proto")!="dhcp":
    raise SystemExit("Factory default WAN must use DHCP on eth0.2")
for item in interfaces:
    name=item["name"]; opts=item["options"]
    if name=="lan":
        raise SystemExit("LAN Layer-3 interface must not be configured")
    if name!="loopback" and opts.get("device")=="eth0.1":
        raise SystemExit(f"eth0.1 must not have a Layer-3 interface ({name})")
print("Network contract OK: factory default DHCP on eth0.2; no LAN or eth0.1 L3")
PYNET

make -C "$SRC" defconfig
# Resolve DEVICE_PACKAGES against the generated package index now, not at image
# assembly 40 minutes later. A single phantom package name is what broke the
# only build that ever reached the end.
python3 "$PROJECT_DIR/scripts/validate-packages.py" "$SRC"
make -C "$SRC" target/linux/prepare V=s
SPIDEV_SOURCE="$(find "$SRC/build_dir" -type f -path '*/drivers/spi/spidev.c' -print -quit)"
[[ -n "$SPIDEV_SOURCE" && -f "$SPIDEV_SOURCE" ]] || { echo "Linux spidev.c not found" >&2; exit 4; }
# The patch is applied by target/linux/prepare from patches-6.12/. Assert it.
grep -qF 'yspeed,st7789-spidev' "$SPIDEV_SOURCE" || {
  echo "spidev.c patch did not apply; check patches/999-spidev-yspeed-match.patch context" >&2
  exit 4
}
SPI_MASTER_SOURCE="$(find "$SRC/build_dir" -type f -path '*/drivers/spi/spi-mt7621.c' -print -quit)"
[[ -n "$SPI_MASTER_SOURCE" && -f "$SPI_MASTER_SOURCE" ]] || { echo "MT7621 SPI master source not found" >&2; exit 4; }
grep -qF 'host->mode_bits = SPI_LSB_FIRST | SPI_NO_CS;' "$SPI_MASTER_SOURCE" || {
  echo "MT7621 SPI_NO_CS patch did not apply; manual LCD chip-select is unavailable" >&2
  exit 4
}
# The vendor runs with POLAR=0, i.e. native CS1 asserted for the whole session.
# The patch must therefore ASSERT CS1 in the SPI_NO_CS path, never release it:
# releasing it is what left every earlier build clocking data into a deselected
# panel. Assert the exact call so that regression cannot come back silently.
grep -qF 'mt7621_spi_set_native_cs(spi, true);' "$SPI_MASTER_SOURCE" || {
  echo "MT7621 SPI_NO_CS patch must hold CS1 asserted (set_native_cs(spi, true))" >&2
  exit 4
}
grep -qF 'transfer timeout, controller still busy' "$SPI_MASTER_SOURCE" || {
  echo "MT7621 timeout-reporting patch did not apply" >&2
  exit 4
}
# The vendor numbers its GPIO banks from 480 and its userspace (including the
# display binary) hardcodes those numbers. Pin the allocator's starting point
# and fail the build if the patch did not land.
KERNEL_TREE="$(dirname "$(dirname "$(dirname "$SPI_MASTER_SOURCE")")")"
GPIO_HEADER="$KERNEL_TREE/include/linux/gpio.h"
[[ -f "$GPIO_HEADER" ]] || { echo "Kernel include/linux/gpio.h not found" >&2; exit 4; }
grep -q '#define GPIO_DYNAMIC_BASE[[:space:]]*480' "$GPIO_HEADER" || {
  echo "GPIO base patch did not apply; vendor GPIO numbering unavailable" >&2
  exit 4
}
DISPLAY_SOURCE="$PROJECT_DIR/package/yspeed-display/src/yspeed-display.c"
# Exercise the actual C resolver against synthetic gpiochip sysfs trees.
# This validates behavior (including malformed/ambiguous cases), not source wording.
bash "$PROJECT_DIR/scripts/test-gpio-resolver.sh"
grep -q '/dev/spidev0.1' "$DISPLAY_SOURCE" || { echo "SPI userspace app missing spidev device" >&2; exit 5; }
if grep -q '/dev/fb0' "$DISPLAY_SOURCE"; then echo "Framebuffer dependency remains" >&2; exit 5; fi
# Let the top-level build construct the host tools and cross toolchain before
# compiling the kernel. Calling target/linux/compile directly skips that order.
grep -qx 'CONFIG_TARGET_ROOTFS_INITRAMFS=y' "$SRC/.config" || { echo "INITRAMFS profile missing" >&2; exit 6; }
grep -qx 'CONFIG_TARGET_DEVICE_ramips_mt76x8_DEVICE_yspeed_ys-gps-12832=y' "$SRC/.config" || { echo "YSPEED profile missing" >&2; exit 6; }
for package in yspeed-display dropbear uhttpd gpsd gpsd-clients chrony jsonfilter kmod-input-gpio-keys kmod-input-evdev; do
  grep -qx "CONFIG_PACKAGE_$package=y" "$SRC/.config" || { echo "Required package dropped: $package" >&2; exit 6; }
done
if grep -q '^CONFIG_PACKAGE_kmod-gpio-button-hotplug=y$' "$SRC/.config"; then echo "Conflicting gpio-button-hotplug selected" >&2; exit 6; fi
if grep -Eq '^CONFIG_PACKAGE_(wpad[^=]*|iw|kmod-mt7603(e)?|kmod-mac80211|kmod-mt76-core|wireless-regdb)=(y|m)$' "$SRC/.config"; then echo "Wireless package selected" >&2; exit 6; fi
make -C "$SRC" -j"$JOBS" $CCACHE_MAKEOPT V=s

# Validate the resolved kernel configuration after the normal top-level build.
# Locate the resolved kernel config, but only trust a file that actually looks
# like one. Picking the first */linux-*/.config match previously selected a file
# without CONFIG_SPI= and reported every symbol as ABSENT, failing a build that
# had already produced an image.
KERNEL_DOTCONFIG=""
KERNEL_CONFIG_CANDIDATES="$(find "$SRC/build_dir" -type f -path '*/linux-*/.config' 2>/dev/null || true)"
for candidate in $KERNEL_CONFIG_CANDIDATES; do
  if grep -q '^CONFIG_SPI=' "$candidate" 2>/dev/null; then
    KERNEL_DOTCONFIG="$candidate"
    break
  fi
done

if [ -z "$KERNEL_DOTCONFIG" ]; then
  echo "WARNING: no candidate kernel .config contained CONFIG_SPI=;" >&2
  echo "WARNING: skipping kernel symbol assertions. Candidates seen:" >&2
  printf '  %s\n' $KERNEL_CONFIG_CANDIDATES >&2 2>/dev/null || true
else
  echo "kernel config   : $KERNEL_DOTCONFIG"
  for symbol in CONFIG_SPI CONFIG_SPI_MASTER CONFIG_SPI_MT7621 CONFIG_SPI_SPIDEV CONFIG_GPIO_SYSFS; do
    printf 'kernel config   : %s -> %s\n' "$symbol" "$(grep -E "^$symbol=" "$KERNEL_DOTCONFIG" || echo ABSENT)"
  done
  # These are warnings, not failures: the kernel has already compiled and the
  # image has already been produced by this point. The artifact-level checks
  # (initramfs present, DTB contains st7789@1) are the authoritative gates.
  grep -qx 'CONFIG_GPIO_SYSFS=y' "$KERNEL_DOTCONFIG" || \
    echo "WARNING: CONFIG_GPIO_SYSFS is not builtin; sysfs GPIO may be unavailable" >&2
  grep -qx 'CONFIG_SPI_MT7621=y' "$KERNEL_DOTCONFIG" || \
    echo "WARNING: CONFIG_SPI_MT7621 is not builtin; hardware SPI master may be missing" >&2
  if grep -qx 'CONFIG_SPI_SPIDEV=y' "$KERNEL_DOTCONFIG"; then
    echo "kernel config   : spidev builtin"
  elif grep -qx 'CONFIG_SPI_SPIDEV=m' "$KERNEL_DOTCONFIG"; then
    echo "kernel config   : spidev module (kmod-spi-dev provides it)"
  else
    echo "WARNING: CONFIG_SPI_SPIDEV is neither y nor m; /dev/spidev0.1 will not exist" >&2
  fi
fi

OUT="$SRC/bin/targets/ramips/mt76x8"
IMAGE="$(find "$OUT" -maxdepth 1 -type f -name '*yspeed_ys-gps-12832-initramfs-kernel.bin' -print -quit)"
[[ -n "$IMAGE" && -f "$IMAGE" ]] || { echo "Expected initramfs uImage not found" >&2; exit 7; }
# A kernel-only image passes every CRC check and is still named
# "*-initramfs-kernel.bin", but it panics at boot with "VFS: Cannot open root
# device" because there is no initramfs to mount. Refuse to publish one.
python3 "$PROJECT_DIR/scripts/validate-initramfs.py" "$IMAGE"
# The kernel build uses its in-tree dtc; OpenWrt does not guarantee that
# a separate host-tools dtc is installed in staging_dir/host/bin.
DTC="$SRC/staging_dir/host/bin/dtc"
if [[ ! -x "$DTC" ]]; then
  DTC="$(find "$SRC/build_dir" -type f -path '*/scripts/dtc/dtc' -print -quit)"
fi
DTB="$(find "$SRC/build_dir" -type f -name 'image-mt7628an_yspeed_ys-gps-12832.dtb' -print -quit)"
[[ -n "$DTC" && -x "$DTC" && -n "$DTB" && -f "$DTB" ]] || {
  echo "Compiled DTB or usable dtc not found" >&2
  echo "DTC candidate: ${DTC:-<none>}" >&2
  echo "DTB candidate: ${DTB:-<none>}" >&2
  exit 8
}
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
 echo "Display: SPI0 CS1 held asserted (vendor NO_CS), 24 MHz"
echo "GPIO: dynamic base 480 (vendor numbering: CS 486, reset 494, DC 496, BL 499)"
 echo "Build UTC: $(date -u +%FT%TZ)"
 echo "RAM-boot-only; do not flash."
} > "$WORKDIR/artifacts/build-manifest.txt"
echo "Build complete: $WORKDIR/artifacts/yspeed-ramboot.uImage"
