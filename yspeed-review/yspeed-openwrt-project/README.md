# YSPEED YS-GPS-12832 OpenWrt RAM-boot build

## Current implementation

The colour 240x320 ST7789 panel is driven from userspace over SPI0 chip-select 1.
The vendor firmware confirms the application interface is `/dev/spidev0.1`;
the old `wealth,sh1106` I2C node is unrelated/stale. The current project removes
the FBTFT/framebuffer binding and uses:

- SPI0 clock/data at up to 40 MHz; GPIO6 is muxed from the SPI-CS1 pad to GPIO
  and driven as an explicit active-low software chip-select. The spidev device
  remains /dev/spidev0.1, configured with SPI_NO_CS.
- A YSPEED-specific `yspeed,st7789-spidev` DT compatible, added to Linux 6.12's
  spidev OF match table during the build. A generic `linux,spidev` compatible
  is not accepted by modern Linux.
- `CONFIG_SPI_SPIDEV=y` and `CONFIG_GPIO_SYSFS=y`.
- GPIO offsets 6 (SPI CS), 16 (D/C), 14 (reset), and 19 (backlight), exported
  at runtime. The renderer asserts GPIO6 across each command/data transaction
  and across each complete framebuffer transfer.
- An English-only userspace ST7789 renderer. It shows system time, eth0.2
  address, GNSS status, receiver UTC, and Chrony status. Keys remain on the
  gpio-keys/evdev path.

The vendor's stripped LVGL executable was recovered from the supplied firmware
archive, but is not checked into this repository. GitHub Actions cannot read
conversation attachments. The checked-in renderer is a source-built
implementation of the confirmed SPI architecture; it is not claimed to be the
original LVGL executable or an exact reproduction of its screens.

## Board and network assumptions

- MediaTek MT7628AN, ramips/mt76x8; 128 MiB RAM.
- No build or test command writes to MTD, U-Boot environment, factory data, or
  persistent flash.
- `eth0.2` is the only operational Layer-3 interface. It uses ordinary DHCP,
  accepting the network-provided address, gateway and DNS. LuCI can change it
  to static IPv4 addressing for a rack network.
- No Layer-3 interface, static address, DHCP server, bridge, or fallback
  management path is created on `eth0.1`.
- Wired-only: PCIe/WMAC and wireless packages are disabled.
- GPSD is configured for `/dev/ttyS1` at 9600 baud; vendor evidence also
  mentions `/dev/ttyS2`, so the correct UART remains a hardware check. The
  antenna is disconnected, so no satellite fix is expected during bench work.
- PPS is GPIO11 active-high. Chrony combines GPS receiver time with PPS. NIST
  and Cloudflare remain active fallbacks. Verify PPS lock/source selection
  with the antenna connected before treating the appliance as a precision source.
- DS1307 RTC is at I2C address 0x68.

## Build

The build pins OpenWrt v25.12.6 and the stable package/LuCI feeds in
`feeds.conf`, including Chrony 4.8-r3. It validates the network contract, resolved kernel symbols,
compiled DTB, uImage structure/CRCs, and absence of sysupgrade/factory images.

Run on a supported Linux x86-64 host with at least 30 GB free disk:

```sh
JOBS=4 ./scripts/build.sh "$HOME/build/yspeed-openwrt-25.12.6"
```

The only intended kernel output is an initramfs legacy uImage for U-Boot RAM
boot. It is not a sysupgrade image and must not be written to flash.

## Validation still required

A successful compile does not prove the physical LCD is initialized correctly.
Before booting, review the build manifest, uImage validation, compiled DTB, and
live U-Boot load/decompression addresses. After a RAM-only boot, verify SSH,
DHCP on eth0.2, `/dev/spidev0.1`, GPIO export/direction, panel reset/backlight,
visible pixels, button input, RTC, serial GNSS data, and PPS electrical
activity. No satellite fix is expected until the antenna is connected.

For static addressing, use LuCI Network → Interfaces → WAN and select Static, then enter the rack network address, mask, gateway and DNS. No flash writes are authorized by this project.


## Panel test modes in the diagnostic image

`yspeed-display --white` fills the panel white and holds it until Ctrl-C, and
`--test` now ends holding a white frame for 15 s instead of leaving the screen
black. The daemon also paints one white frame at every start, before the
dashboard, so a power-on check needs no commands at all. A dark glass during any
of those holds means the bytes are not reaching the controller; a white glass
means the SPI path is good and the fault is in what the dashboard draws.
