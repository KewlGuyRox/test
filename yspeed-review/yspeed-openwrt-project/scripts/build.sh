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
"$PROJECT_DIR/scripts/append-profile.sh" "$SRC"
"$PROJECT_DIR/scripts/disable-wireless-defaults.sh" "$SRC"

# Pin feed revisions using the official 25.12.6 feeds.buildinfo snapshot.
(cd "$SRC" && ./scripts/feeds update packages luci && \
 ./scripts/feeds install -p packages gpsd gpsd-clients chrony && \
 ./scripts/feeds install -p luci luci-light luci-mod-network luci-mod-status luci-mod-system luci-ssl luci-theme-bootstrap)

cp "$PROJECT_DIR/config/ramips-mt76x8-yspeed.config" "$SRC/.config"
cp -a "$PROJECT_DIR/files/." "$SRC/files/"

# Enforce the owner's network contract before resolving the OpenWrt config:
# eth0.2 is the only Layer-3 interface and receives its address via DHCP.
NETWORK_CONFIG="$PROJECT_DIR/files/etc/config/network"
grep -qE "option device ['\\"]eth0\\.2['\\"]" "$NETWORK_CONFIG" || {
  echo "Network contract violation: eth0.2 is not configured" >&2; exit 4;
}
grep -qE "option proto ['\\"]dhcp['\\"]" "$NETWORK_CONFIG" || {
  echo "Network contract violation: eth0.2 must use DHCP" >&2; exit 4;
}
if grep -Eq "option (device|ifname) ['\\"]eth0\\.1['\\"]|option ipaddr" "$NETWORK_CONFIG"; then
  echo "Network contract violation: eth0.1/static Layer-3 configuration detected" >&2
  exit 4
fi
if grep -Eq "option interface ['\\"]lan['\\"]|config interface ['\\"]lan['\\"]" "$NETWORK_CONFIG"; then
  echo "Network contract violation: unexpected LAN interface stanza" >&2
  exit 4
fi
make -C "$SRC" defconfig

# Hard guard: this project must not produce a flash/sysupgrade/factory image.
if ! grep -q '^CONFIG_TARGET_ROOTFS_INITRAMFS=y$' "$SRC/.config"; then
  echo "INITRAMFS configuration did not survive defconfig" >&2; exit 4
fi
if ! grep -q '^CONFIG_TARGET_DEVICE_ramips_mt76x8_DEVICE_yspeed_ys-gps-12832=y$' "$SRC/.config"; then
  echo "YSPEED profile is not selected in .config" >&2; exit 4
fi
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
for token in 'YSPEED YS-GPS-12832' 'pps-gpio' 'dallas,ds1307' 'denx,uimage' 'yspeed,ys-gps-12832' 'mac-address@2e'; do
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
