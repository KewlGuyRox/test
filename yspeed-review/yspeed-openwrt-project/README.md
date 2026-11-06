# YSPEED YS-GPS-12832 OpenWrt RAM-boot build project

## Status

This is a reproducible **build project**, not a compiled firmware image. The
current ChatGPT execution container had no DNS/network route to the OpenWrt
source host and no installed `dtc` or MIPS cross-compiler. The build script is
provided for an Internet-connected Linux build host. It pins OpenWrt 25.12.6
and the package/LuCI feed commits recorded in the official 25.12.6
`feeds.buildinfo`.

## Board and network assumptions

- SoC: MediaTek MT7628AN, `ramips/mt76x8`, MIPS little-endian.
- Flash: 32 MiB W25Q256.
- Protected regions: U-Boot `0x00000000+0x30000`, environment
  `0x00030000+0x10000`, factory `0x00040000+0x10000`.
- Firmware partition: `0x00050000+0x01fb0000` (32,448 KiB).
- `eth0.2` is the **only operational network interface**. The vendor calls
  this VLAN/interface WAN, but physically and functionally it connects to the
  owner's existing LAN. OpenWrt's logical network is named `wan` only to match
  the vendor-derived firewall/configuration references; that label does not
  mean the device is intended to be used as an Internet-edge router.
- The `wan` logical interface binds to `eth0.2`, uses DHCP, and accepts its
  address, gateway, and DNS from the owner's existing network. No static
  management address is embedded.
- `eth0.1` is the vendor-labeled LAN VLAN and is **not** a management or
  service interface in this project. It has no OpenWrt Layer-3 interface,
  address, DHCP server, bridge, or default route. The captured switch VLAN
  membership is retained pending physical-port verification; VLAN membership
  alone does not create an IP interface.
- Switch map retained from the captured vendor configuration: VLAN 1 ports
  1-4 plus CPU port 6 tagged; VLAN 2 port 0 plus CPU port 6 tagged. These are
  hardware switch VLAN definitions, not a statement that both VLANs are used
  for IP networking.
- OpenWrt's standard firewall defaults are retained; a first-boot UCI-defaults
  script adds only UDP/123 from the WAN zone so upstream LAN NTP clients can
  query Chrony. This does not alter the running vendor device.
- No Wi-Fi functionality: no radio nodes enabled and no Wi-Fi packages added.
  USB 2.0/OHCI host support is retained because it is present in the vendor DTB.
- GNSS UART1 remains at its existing 9600-baud setting. No receiver commands
  or baud-rate changes are performed.
- PPS is GPIO11 active-high. GPS/PPS Chrony refclocks remain `noselect` until
  UTC-second association is verified.
- DS1307 RTC is at I2C `0x68`. The display is a 240x320 ST7789 SPI TFT, not
  an SH1106 I2C OLED. The checked-in DTS and prior compiled DTB still contain
  a stale SH1106 node; ST7789 SPI chip-select and control GPIO wiring must be
  reconciled before the display driver can safely be enabled. The current
  candidate image must therefore be treated as a bring-up baseline, not a
  display-complete release.

## Why the image is RAM-boot-only

The YSPEED profile sets `IMAGES :=` to suppress sysupgrade/factory image
creation. OpenWrt's MT76x8 default kernel pipeline appends the DTB, LZMA
compresses the kernel, and wraps it in a legacy MIPS uImage. The initramfs
pipeline defaults to the same kernel recipe when
`CONFIG_TARGET_ROOTFS_INITRAMFS=y`. The intended result is
`*-initramfs-kernel.bin`, a legacy uImage for U-Boot `bootm` from RAM.

The uImage is **not** a flash image. It is not to be written to any MTD
partition. The 31.7 MiB firmware partition limit applies to a future,
separately reviewed persistent image, not to the RAM-boot test artifact.

## Build host

Use a supported Linux x86-64 build host with the standard OpenWrt build
prerequisites installed (compiler toolchain, make, git, ncurses, SSL, zlib,
Python, rsync, Perl, and the documented host utilities). Allow at least 30 GB
free disk space; more is useful for caches and parallel builds.

Run:

```sh
./scripts/build.sh "$HOME/build/yspeed-openwrt-25.12.6"
```

Override parallelism with `JOBS=4` or another value. The script modifies only the disposable OpenWrt source checkout: the upstream MT76x8 `DEFAULT_PACKAGES` line is narrowed from `kmod-mt7603 wpad-basic-mbedtls swconfig` to `swconfig`. This is necessary because the MT76x8 target defaults otherwise reintroduce wireless packages even though the YSPEED device profile is wired-only. `rpcd-mod-iwinfo` may remain as a LuCI RPC integration dependency; it does not enable a radio or load a wireless driver.

The script:

1. Checks out the signed OpenWrt `v25.12.6` tag.
2. Pins package and LuCI feeds to the exact revisions in the official release
   feed manifest.
3. Installs the YSPEED DTS and image profile.
4. Selects only `ramips/mt76x8` and the YSPEED profile.
5. Enables initramfs and installs GPSD (UART1), Chrony, PPS GPIO, I2C/RTC, USB host, and a small
   LuCI management set.
6. Removes the MT76x8 target default Wi-Fi packages (`kmod-mt7603` and `wpad-basic-mbedtls`) in the isolated build checkout while retaining `swconfig`; then verifies that no Wi-Fi driver stack, `iw`, regulatory database, or `wpad` package is selected. PCIe/WMAC remain disabled in the DTS.
7. Builds the DTB and initramfs uImage, decompiles the DTB for inspection,
   validates the uImage CRCs/architecture/compression/load/entry addresses,
   and refuses to publish a sysupgrade/factory image.

## Safety gates before RAM boot

Even after the script succeeds, do not boot the image until all of these are
reviewed:

- `build-manifest.txt`, `uimage-validation.txt`, and the decompiled DTB.
- U-Boot's **live** `printenv` output and actual TFTP/bootm prompts. The
  previously recovered binary strings are not a substitute for live env.
- Confirm that the TFTP load address does not overlap the decompression or
  initramfs destination in 128 MiB RAM.
- Confirm UART1/UART2, GPIO11 PPS, DS1307, Ethernet/switch and VLANs on the
  compiled DTB. Confirm the ST7789's actual SPI CS, D/C, reset, and backlight
  wiring before enabling its device-tree node or framebuffer driver.
- Confirm the factory MAC NVMEM cell uses offset `0x2e`, matching the captured
  WAN MAC `60:70:72:59:22:0b`; offset `0x28` contains the adjacent `...:0a`.
- Verify GPSD is configured for `/dev/ttyS1` at its existing 9600-baud host
  default; no receiver configuration or baud command is sent. GPSD is local-only.
- Verify Chrony UDP/123 is permitted from the upstream LAN, with NIST/Cloudflare
  selected and GPS/PPS still `noselect`.

No command in this project writes to the YSPEED device. No U-Boot, environment,
factory, or firmware MTD partition is modified. A later persistent-flash image
requires a separate review and explicit owner approval.


## Network contract (do not reinterpret vendor labels)

The network contract is intentionally unambiguous:

- **Use `eth0.2` only** for device access on the owner's network.
- The interface obtains its IPv4 address and DNS by DHCP.
- The OpenWrt UCI network name remains `wan` for compatibility with the
  existing firewall rules. This is a logical label, not a physical topology
  description.
- Do not create an `eth0.1` Layer-3 interface, static address, DHCP server,
  bridge, or fallback management network.
- Do not change the switch VLAN port map without confirming the physical
  connector-to-switch-port mapping.

## Display status

The ST7789 driver source exists in the captured Linux 6.12.112 tree, but the
project does not yet have a verified board-level ST7789 device-tree binding or
an OpenWrt kernel package selection for it. The stale SH1106 node must not be
mistaken for working display support. This is an explicit hardware-integration
gate, not a reason to guess GPIO assignments. The display is not considered
supported until the actual D/C, reset, backlight, and CS wiring is confirmed
and the resulting kernel driver is present in the built image.
