# HANDOFF — YSPEED YS-GPS-12832 NTP appliance

**Read this file first.** It is the continuation brief: verified facts, current
state, known unknowns, and the order of work. It exists so a different engineer
(or a different AI assistant) can pick this up without re-deriving anything.

Companion documents in this directory:

- `VENDOR-INVENTORY.md` — what the original vendor firmware did, recovered from
  the captured rootfs. Use it as the feature list, not as a design to copy.
- `TEST-RAMBOOT.md` — how to test an image without changing anything permanent.

---

## 1. Goal

Replace the Chinese vendor firmware on a YS-GPS-12832 (MT7628AN) GNSS time
appliance with stock OpenWrt, keeping every useful *function*, and make it an
accurate GNSS/PPS-disciplined NTP server.

Constraints that are not negotiable:

- **RAM boot only.** No flash writes, no `sysupgrade`, no factory image.
- **Single Ethernet uplink.** The WAN port (`eth0.2`) takes DHCP from the
  operator's LAN. There is no LAN interface, no bridge, no `192.168.88.1/24`.
- **Wired only.** No Wi-Fi packages, no radio.
- Image must not depend on the DHCP lease for correctness beyond addressing.

## 2. Verified hardware facts

All of this is measured, not assumed. Source noted per row.

| Fact | Value | Source |
|---|---|---|
| SoC / target | MT7628AN, `ramips/mt76x8` | vendor DTB |
| Flash | 32 MiB, `u-boot` 0x0, `u-boot-env` 0x30000, `factory` 0x40000, `firmware` 0x50000+0x1fb0000 | full flash dump |
| Console | `ttyS0` at **57600** 8N1 | `chosen/bootargs`, U-Boot env |
| Uplink | `eth0.2` = WAN port, DHCP | vendor `network` config |
| Switch | VLAN1 `1 2 3 4 6t`, VLAN2 `0 6t` | vendor DTB + config |
| MAC cells | `…:09`@0x04, `…:0a`@0x28, `…:0b`@0x2e | flash `factory` partition |
| MAC in use | `0x2e` (`…:0b`) — what the vendor actually deployed on WAN | vendor `network` config |
| Display | ST7789 240x320, hardware SPI **CS1**, **40 MHz** → `/dev/spidev0.1` | vendor DTB + binary |
| LCD D/C | GPIO16 | vendor config comments |
| LCD reset | GPIO14 | same |
| LCD backlight | GPIO19 | same |
| Keys | GPIO18 = up, GPIO23 = enter/select, GPIO26 = down | vendor DTB + binary |
| PPS | GPIO11 active-high, `pps-gpio` node | vendor DTB |
| RTC | DS1307 at I2C `0x68` | vendor DTB |
| Status LED | GPIO44 active-low | vendor DTB |
| Reset button | GPIO38 active-low | vendor DTB |

### The GPIO numbering trap (cost the project weeks)

The vendor kernel registered its gpiochip with **base 480**, so D/C was sysfs
`gpio496`. Mainline 6.12 uses **base 0**, so the same pin is `gpio16`. The vendor
LVGL binary hardcodes 494/496/499/486 and therefore cannot work on a modern
kernel at all. Never hardcode sysfs numbers; resolve `base` from
`/sys/class/gpio/gpiochipN/base` at runtime. `/usr/sbin/yspeed-display` does.

### PPS has never been used

`/etc/config/gps-timesync` in the vendor image ships `pps_enabled '0'`, and the
vendor backend has no PPS references. The vendor served time from gpsd/NMEA —
millisecond grade. **Treat PPS as unvalidated new capability**, not as something
to restore. Keep the refclocks `noselect` until observed working.

## 3. Key design decision: one JSON, two consumers

```
/usr/sbin/yspeed-status  ──┬──▶ LuCI status view
   (single JSON document)  └──▶ /usr/sbin/yspeed-display (LCD)
```

The LCD and the web UI must never compute the same thing twice, or they will
disagree. `yspeed-status` is the only source of satellite/fix/time data. Any new
panel field must be added there first.

## 4. What is implemented

| Area | State |
|---|---|
| Device tree | `board/mt7628an_yspeed_ys-gps-12832.dts` — single uplink, USB, LED, keys, PPS, ST7789 node on CS1 |
| Renderer | `package/yspeed-display/src/yspeed-display.c` — runtime GPIO base, rotation via MADCTL, config parsing, backlight timeout, `--selftest`, `--test` |
| Status JSON | `files/usr/sbin/yspeed-status` — includes `satellites[]`, `satellites_used`, `quality` |
| Time | `files/etc/chrony/chrony.conf` — GNSS/PPS `noselect`, NIST + Cloudflare by literal IP, private-range `allow` |
| Network | `files/etc/config/network` — WAN DHCP on `eth0.2`, `defaultroute`/`peerdns` **enabled** |
| Firewall | `files/etc/config/firewall` — stock + `Allow-NTP` on wan |
| Build gate | `scripts/validate-initramfs.py` called from `build.sh`; refuses a kernel-only image |
| CI | `.github/workflows/yspeed-ramboot-build.yml` (see §7 — needs `workflow` scope to update) |

## 5. Build and validate

CI builds on `ubuntu-24.04`. Local build needs a Linux host with the OpenWrt
prerequisites and ~30 GB:

```sh
bash scripts/build.sh "$HOME/build/yspeed-openwrt-25.12.6"
```

Artifact: `openwrt-ramips-mt76x8-yspeed_ys-gps-12832-initramfs-kernel.bin`

Two independent validators run in the pipeline:

```sh
python3 scripts/validate-initramfs.py <image>   # decompresses, walks newc cpio
python3 scripts/validate-uimage.py   <image>    # CRCs, arch, compression, load
```

`validate-initramfs.py` is the important one: a kernel-only image passes every
CRC check and is still named `*-initramfs-kernel.bin`, but panics at boot with
`VFS: Cannot open root device`. This project has already shipped one of those.

**A real initramfs image decompresses to 20–30 MiB. 9.6 MiB means rootfs was
skipped.**

## 6. Known unknowns — do not guess these

| Unknown | How to settle it |
|---|---|
| Which UART carries the GNSS receiver | The vendor UI lists `ttyS0/1/2` as a user choice; its gpsd path shipped `ttyS1`, its own app used `ttyS2`. Only `cat /dev/ttyS1` / `ttyS2` at 9600 with a receiver attached settles it |
| Whether the PPS trace works | Needs antenna + fix; then `cat /sys/class/pps/pps0/assert` should increment once per second |
| Whether the panel lights up | Needs hardware; `yspeed-display --selftest` then `--test` |
| Whether the 3 keys behave | Needs hardware; short press and 1 s long press |
| Vendor's `quality` percentage algorithm | Unknown; this project defines it as mean SNR of used satellites normalised to 50 dB-Hz. It will not match the vendor number exactly |
| Achievable accuracy | GNSS + PPS ≈ tens of µs. Anything claiming better needs PTP hardware that this SoC lacks |

## 7. Standing hazards

1. **U-Boot load address.** `tftpboot` defaults to `0x80A00000`, only 10 MiB above
   the `0x80000000` decompression start. Use `tftpboot 0x84000000` and
   `bootm 0x84000000`.
2. **Line endings.** `.sh`, `.init`, `.py`, `.dts` must be LF. `.gitattributes`
   enforces it; a CRLF shebang fails on OpenWrt and in CI.
3. **`workflow` OAuth scope.** Pushing `.github/workflows/*` needs it; `gh`'s
   default token does not have it. Use `gh auth refresh -s workflow` or edit in
   the web UI.
4. **Every push restarts CI.** The concurrency fix that prevents this is in the
   staged workflow file, blocked by hazard 3.
5. **Runner disk.** ~14 GB free versus the ~30 GB a full build wants.
6. **Never flash.** See `TEST-RAMBOOT.md`.

## 8. Order of work from here

1. Get one green build. Until that exists nothing else is provable.
2. Confirm the artifact decompresses to 20–30 MiB and its DTB contains
   `st7789@1`, `spi_cs1`, and the three LCD keys.
3. RAM-boot it, run `yspeed-display --selftest` and `--test`.
4. Verify the single-uplink contract on the device (`ip addr`, no `192.168.88`).
5. Verify chrony serves a client on the bench LAN.
6. Build `luci-app-yspeed` (Status / GNSS / Time / Display pages) per
   `VENDOR-INVENTORY.md`; keep `yspeed-status` as the single data source.
7. Add the LCD satellite page and 1 s long-press buttons at vendor parity.
8. **Only with the antenna attached:** verify fix, then PPS, then promote the
   refclocks from `noselect` to `prefer`.
