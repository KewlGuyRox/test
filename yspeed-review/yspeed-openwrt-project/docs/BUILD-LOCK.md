# Build lock / source provenance

- OpenWrt release tag: `v25.12.6`
- OpenWrt source repository: `https://git.openwrt.org/openwrt/openwrt.git`
- Release tag short commit shown by the official repository: `64900e756e...`
- Official release date: 2026-10-18
- Target: `ramips/mt76x8`
- Architecture: `mipsel_24kc`
- Kernel line in OpenWrt 25.12.6: 6.12.x
- Packages feed commit: `1a304c1a7031e356c97279c9a4a273f1710f08df` (stable 25.12 branch; Chrony 4.8-r3)
- LuCI feed commit: `76f7b462d8d842cb392c2902d35af34ee4ae2130` (stable 25.12 branch)

The feeds revisions are pinned to the OpenWrt 25.12 stable branch heads on 2026-11-07. The build script records the exact source/feed commit IDs. Earlier checked-in image artifacts are not trusted as build outputs until validated; the tracked uImage was found to contain no initramfs and must not be booted.
