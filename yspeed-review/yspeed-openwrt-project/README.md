# YSPEED YS-GPS-12832 OpenWrt RAM-boot build project

## Status

This repository contains the YS-GPS-12832 RAM-boot build project. Build #73
completed, but its artifact predates the corrected GPIO chip-select path and
is not a release candidate. Build #74 failed before compilation because the
ramips target's global package defaults reselected
`kmod-gpio-button-hotplug`, which conflicts with the intended native
`gpio-keys`/evdev driver. The `yspeed/blocker-resolution` branch removes that
default, routes ST7789 chip select through the vendor GPIO6 mapping, and
expands the LCD telemetry pages. This branch has deliberately not been built
or hardware-validated. The build script pins OpenWrt 25.12.6 and the
package/LuCI feed commits recorded in the official 25.12.6 `feeds.buildinfo`.

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
- The `wan` logical interface binds to `eth0.2` and uses DHCP for its IPv4
  address. The current bench DHCP service intentionally supplies no gateway
  or DNS; do not add either merely to make bench testing appear online. No
  static management address is embedded.
- `eth0.1` is the vendor-labeled LAN VLAN and is **not** a management or
  service interface in this project. It has no OpenWrt Layer-3 interface,
  address, DHCP server, bridge, or default route. The captured switch VLAN
  membership is retained pending physical-port verification; VLAN membership
  alone does not create an IP interface.
- Switch map retained from the captured vendor configuration: VLAN 1 ports
  1-4 plus CPU port 6 tagged; VLAN 2 port 0 plus CPU port 6 tagged. These are
  hardware switch VLAN definitions, not a statement that both VLANs are used
  for IP networking.
- The firewall explicitly permits UDP/123 from the logical `wan` zone
  (bound to `eth0.2`) so NTP clients on the owner's existing network can query
  Chrony. The rule is in the checked-in firewall configuration; no separate
  first-boot UCI-defaults script is required.
- No Wi-Fi functionality: no radio nodes enabled and no Wi-Fi packages added.
  USB 2.0/OHCI host support is retained because it is present in the vendor DTB.
- GNSS UART1 remains at its existing 9600-baud setting. No receiver commands
  or baud-rate changes are performed.
- The GPS antenna is currently disconnected; no satellite fix is expected
  during bench testing. Receiver serial output and PPS electrical activity
  remain separate checks and must not be conflated with satellite reception.
- The vendor display package is a stripped LVGL 8.3.5 userspace application.
  It opens `/dev/spidev0.1`, uses sysfs GPIO control, and its configuration
  identifies GPIO6 as SPI_CS, GPIO16 as D/C, GPIO14 as reset, GPIO19 as
  backlight, and GPIO18/23/26 as the three keys. The recovered
  `/etc/spilcd/wangzhi` file contains a URL; it is not a GPIO map. The vendor
  GPS UI backend uses `/dev/ttyS2` and `/gps_data_shm`; the separate vendor
  time-sync path uses `/dev/ttyS1`. The current GPSD/Chrony path stays on
  `/dev/ttyS1`; do not silently move it to ttyS2.
- PPS is GPIO11 active-high. GPS/PPS Chrony refclocks remain `noselect` until
  UTC-second association is verified.
- DS1307 RTC is at I2C `0x68`. The ribbon-connected display is a 240x320
  ST7789V SPI TFT. The display child remains SPI0 device 1, but the DTS now
  routes that chip select through `cs-gpios = <0>, <&gpio 6 GPIO_ACTIVE_LOW>`
  rather than the native SPI CS1 pinmux. This matches the vendor's explicit
  GPIO6 SPI_CS assignment. D/C is GPIO16, reset GPIO14, and backlight GPIO19.
  The three front-panel keys are GPIO18/23/26 and use standard Linux
  `gpio-keys` plus evdev. The ramips target's legacy
  `kmod-gpio-button-hotplug` default is removed from the isolated build
  checkout because it claims the same DT node and rejects KEY_UP/KEY_DOWN.
  LCD text and menus are English-only. The framebuffer dashboard starts at
  boot and presents GNSS position, UTC, satellite count, DOP, speed/course,
  PPS counters, RTC, Chrony state/offsets, and network addressing from the
  same read-only telemetry JSON as the web page. Panel initialization, reset
  and backlight polarity, rotation, and visible pixels still require one
  controlled RAM-only hardware validation.

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

Override parallelism with `JOBS=4` or another value. The script modifies only the disposable OpenWrt source checkout: it narrows
the MT76x8 default package set to remove wireless packages and removes
`kmod-gpio-button-hotplug` from the ramips global `DEFAULT_PACKAGES` line.
That second change is required because the global default otherwise
reintroduces the legacy button driver even when the YSPEED device profile
excludes it. `rpcd-mod-iwinfo` may remain as a LuCI RPC integration dependency;
it does not enable a radio or load a wireless driver.

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

## Rack-side NTP service and telemetry

- The current bench DHCP service provides the device address only. No default
  gateway or DNS is provided by design. SSH, the local web UI and local NTP
  service do not require either. External NTP pools require a separate routed
  network with DNS and are not expected to resolve on this isolated bench.
- Do not add a gateway or DNS to the bench DHCP configuration. If the device is
  later moved to a routed deployment network, configure that network's routing
  and DNS deliberately; do not hard-code either into the image.
- Chrony is the only NTP daemon. OpenWrt sysntpd is disabled at first boot.
  Chrony serves UDP/123 on eth0.2 to clients on any IPv4 subnet reaching that
  interface. The firewall has an explicit UDP/123 allow rule.
- NIST and Cloudflare are configured as independent network time pools. They
  can be selected when DNS and routing are available. An Ethernet-ifup hook
  restarts Chrony after DHCP/static interface configuration so DNS arriving
  after boot is picked up without hard-coding a resolver. GPSD SHM and PPS remain
  `noselect`; the current build does not claim precision GNSS discipline
  until UTC validity and PPS-to-second association are measured and verified.
- The live status page is `http://<device-address>/yspeed/`. It reads the
  device's read-only telemetry endpoint at `/cgi-bin/yspeed-status`. The
  endpoint reports GPSD fix mode and receiver UTC, Chrony tracking/source
  status, PPS sysfs counters, RTC readback, Ethernet address, gateway and DNS.
  Missing data is shown as unavailable; it is not synthesized.
- The LCD application consumes the same telemetry snapshot for fix, UTC, Chrony
  source/state and Ethernet address. The panel initialization now uses the
  vendor-confirmed 40 MHz SPI clock, a generic ST7789V gamma profile, and a
  software-reset delay. The physical panel remains a hardware validation item
  because the current framebuffer driver has not yet produced visible pixels.

## Rack-side patch updates (no flash)

Every successful build also publishes `yspeed-rack-update.tar.gz` and its
SHA-256 file. This bundle contains only the Chrony configuration, status API,
web UI, Ethernet-ifup Chrony hook and updater helper. It deliberately excludes `/etc/config/network` and
firewall configuration, so a patch cannot overwrite operator-specific static
IP, gateway, DNS or security settings. It is intended for quick software and
time-service patches on a running rack unit; it does not contain a kernel or
firmware image.

1. Copy the bundle to the appliance:
   `scp yspeed-rack-update.tar.gz root@<device-address>:/tmp/`
2. Read the expected SHA-256 from the accompanying
   `yspeed-rack-update.tar.gz.sha256` artifact.
3. Apply it interactively:
   `/usr/sbin/yspeed-apply-update /tmp/yspeed-rack-update.tar.gz <sha256>`
4. The updater validates the checksum and archive paths, lists the files,
   and requires typing `APPLY`. It overlays files and restarts affected
   services. It does **not** write flash. On the current initramfs image,
   these changes are temporary and disappear on reboot.

This overlay update path is for rapid userland/configuration iteration only.
Kernel, device-tree, driver and full-system updates require a complete image
update. A future persistent release should use a separately reviewed
sysupgrade image through LuCI **System → Backup / Flash Firmware** or the
standard `sysupgrade` path, after a verified recovery plan. No sysupgrade
image is currently produced, and no flash writes are authorized by this
project's RAM-only build.

## Safety gates before RAM boot

Even after the script succeeds, do not boot the image until all of these are
reviewed:

- `build-manifest.txt`, `uimage-validation.txt`, and the decompiled DTB.
- U-Boot's **live** `printenv` output and actual TFTP/bootm prompts. The
  previously recovered binary strings are not a substitute for live env.
- Confirm that the TFTP load address does not overlap the decompression or
  initramfs destination in 128 MiB RAM.
- Confirm UART1/UART2, GPIO11 PPS, DS1307, Ethernet/switch and VLANs on the
  compiled DTB. During RAM-only bring-up, verify the ST7789 GPIO6 chip-select
  waveform, panel orientation, reset polarity, backlight polarity and visible
  pixels; this branch has not been built or hardware-validated.
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
- The interface obtains only its IPv4 address by DHCP. Peer DNS and default
  route installation are disabled; this isolated management segment receives
  no gateway or DNS from DHCP.
- The OpenWrt UCI network name remains `wan` for compatibility with the
  existing firewall rules. This is a logical label, not a physical topology
  description.
- Do not create an `eth0.1` Layer-3 interface, static address, DHCP server,
  bridge, or fallback management network.
- Do not change the switch VLAN port map without confirming the physical
  connector-to-switch-port mapping.

## Display status

The ST7789V device-tree node, GPIO6 chip-select mapping, kernel module package,
and framebuffer dashboard are integrated into the blocker-resolution source
branch. The display application reports the same GNSS, PPS, RTC, NTP and
network telemetry fields as the web status endpoint. This is not yet a
hardware-validated image. The checked-in legacy uImage/DTB artifacts predate this integration; only a
fresh build can validate that the driver and application are actually present.
Panel rotation and reset/backlight polarity remain first-boot validation items.
