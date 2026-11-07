# HANDOFF — YSPEED YS-GPS-12832 NTP appliance

**Read this file first.** This is the continuation brief for engineers and AI
assistants collaborating on the replacement firmware. It separates evidence,
implementation state, unresolved questions, and safe next actions. Do not treat
a claim as verified merely because it appears in this document.

**Collaboration protocol:** this branch and PR are the shared engineering handoff for ChatGPT and DeepSeek. Before editing, re-read the current branch head and relevant files; preserve evidence with dates and raw outputs; label facts as historical hardware verification, source inspection, or current-build validation. Do not silently overwrite another collaborator's work. The shared PR discussion is the coordination channel for review findings and disagreements.

Companion documents:

- `VENDOR-INVENTORY.md` — recovered vendor functions and feature-parity gaps.
- `TEST-RAMBOOT.md` — RAM-boot test procedure. The U-Boot load sequence is
  intentionally **blocked pending memory-layout validation**.

---

## 1. Goal and non-negotiable constraints

Replace the vendor firmware on the YSPEED YS-GPS-12832 (MediaTek MT7628AN) with
OpenWrt while retaining the useful GNSS, NTP, LCD, and management functions.

- **RAM boot only.** No flash writes, `sysupgrade`, `saveenv`, erase, or factory
  image operations. Do not write persistent storage.
- **Single Ethernet uplink:** WAN port `eth0.2`; DHCP supplies the address only. Do not accept a DHCP default route or DNS (`defaultroute '0'`, `peerdns '0'`). LuCI can configure static IPv4.
- No LAN bridge, no `192.168.88.1/24`, and no wireless packages/radio.
- Keep all UI and system messages in English.

## 2. Hardware facts and evidence quality

The following are reported from the vendor DTB, vendor configuration, flash
capture, or runtime snapshots. Re-check the underlying evidence before making
hardware changes.

| Item | Reported value | Evidence / qualification |
|---|---|---|
| SoC / target | MT7628AN, `ramips/mt76x8` | Vendor DTB |
| Flash layout | u-boot 0x0; env 0x30000; factory 0x40000; firmware 0x50000 | Flash dump; never write these partitions |
| Console | `ttyS0`, 57600 8N1 | Vendor chosen bootargs / U-Boot environment |
| WAN | `eth0.2`, DHCP address only | Project requirement: do not install a DHCP-provided default route or DNS; static WAN remains available in LuCI |
| Switch VLANs | VLAN1 `1 2 3 4 6t`; VLAN2 `0 6t` | Vendor DTB/config; verify on-device |
| MAC cells | `…:09` at 0x04, `…:0a` at 0x28, `…:0b` at 0x2e | Flash factory-partition analysis |
| Deployed WAN MAC | `…:0b` from offset 0x2e | Vendor UCI override; confirm replacement uses this value |
| LCD | ST7789, 240x320, hardware SPI0 CS1, 40 MHz, `/dev/spidev0.1` | Vendor DTB + vendor binary strings |
| LCD control pins | D/C GPIO16, reset GPIO14, backlight GPIO19 | Vendor config comments |
| Keys | GPIO18/23/26 | Vendor DTB/config; kernel input ownership is intended |
| PPS | GPIO11 active-high | Vendor DTB plus November 4, 2026 bench evidence: recurring ~1 Hz assertions captured by `ppstest /dev/pps0`; replacement-build regression still pending |
| RTC | DS1307 at I2C 0x68 | Vendor DTB |
| Status LED | GPIO44 active-low | Vendor DTB |
| Reset button | GPIO38 active-low | Vendor DTB |

### GPIO numbering: do not assume a base

The vendor kernel used global sysfs GPIO numbers around 480 (for example,
SoC offset 16 appeared as global GPIO496). That is **not evidence** that every
modern kernel uses base 0. Global sysfs numbering is controller base plus line
offset, and the base must be read from the running device's
`/sys/class/gpio/gpiochip*/base` and matched to the correct MT7628 GPIO
controller. Do not infer it from the kernel version.

Before hardware testing, verify the selected chip against live gpiochip labels and device relationships.

GPIO ownership must be exclusive:
- LCD reset/D-C/backlight: renderer, if the final pin mapping is confirmed.
- LCD keys GPIO18/23/26: kernel `gpio-keys`; renderer reads evdev only.
- PPS GPIO11, reset GPIO38, and status LED GPIO44: kernel drivers.
Never export a line already owned by a kernel driver.

### PPS status

Vendor configuration reportedly shipped with `pps_enabled '0'`, but that setting must not be confused with the observed hardware behavior. On **November 4, 2026**, with the GNSS antenna connected and receiver healthy (3D fix, 18 visible satellites, `ANTENNA OK`), `ppstest /dev/pps0` captured consecutive assertions `1793832431.508000420#245936`, `1793832432.507999592#245937`, and `1793832433.508000219#245938`; a later sysfs read was `1793832435.508000293#245940`. This establishes recurring ~1 Hz electrical edges reaching the Linux GPIO/PPS path. It is not an oscilloscope voltage/pulse-shape measurement, and UTC edge alignment was not established. The current replacement build still needs a PPS regression test and Chrony source/lock validation. The antenna is disconnected during current bench work, so no new fix/PPS events are expected now.

## 3. Data architecture

Intended architecture:

```
/usr/sbin/yspeed-status ──┬──▶ LuCI status pages
                          └──▶ LCD renderer
```

Use one status producer for GNSS/fix/satellite/time fields so the web UI and
LCD do not independently calculate conflicting values. The renderer currently
parses a small JSON subset; validate its schema and error handling against
actual `yspeed-status` output before relying on it.

## 4. Implementation status — source inspection, not hardware validation

| Area | Current source state | Qualification |
|---|---|---|
| Device tree | `board/mt7628an_yspeed_ys-gps-12832.dts` | Contains SPI CS1 node, keys, PPS, RTC, switch and MAC declarations; compile and inspect final DTB |
| LCD renderer | `package/yspeed-display/src/yspeed-display.c` | Source includes satellite page and settings key behavior; panel/GPIO operation still needs hardware validation |
| Status JSON | `files/usr/sbin/yspeed-status` | Source claims satellite/fix fields; test actual output and schema |
| Chrony | `files/etc/chrony/chrony.conf` | Configuration is not proof of synchronization; test with network and receiver |
| Network | `files/etc/config/network` | **Known blocker:** current source has `defaultroute '1'` and `peerdns '1'`, violating the address-only DHCP requirement; change both to `0` and validate runtime routes/resolvers |
| Firewall | `files/etc/config/firewall` | Inspect final rules and verify UDP/123 exposure is intentional |
| Build gate | `scripts/validate-initramfs.py`, `scripts/validate-uimage.py` | Validators must pass on the exact artifact |
| CI | `.github/workflows/yspeed-ramboot-build.yml` | Build success and artifact publication are separate states; check run logs/artifacts |

The renderer now has a four-page dashboard including shared satellite data, plus a basic display setup menu: hold Down for settings, Up/Down to navigate, Enter to apply, and hold Up to return. Physical keys and panel rendering still require hardware validation. Factory reset and vendor network-mode options are intentionally excluded.

## 5. Build validation

CI builds the OpenWrt tree and should publish a validated initramfs uImage.
A green workflow is necessary but not sufficient: inspect the artifact itself.

Expected artifact name:
`openwrt-ramips-mt76x8-yspeed_ys-gps-12832-initramfs-kernel.bin`

Run both validators against the exact downloaded artifact:

```sh
python3 scripts/validate-initramfs.py <image>
python3 scripts/validate-uimage.py <image>
```

Confirm the decompressed initramfs contains a usable root filesystem and
`/init`; a valid uImage header/CRC alone does not prove that. Record the exact
commit SHA, workflow run, artifact name, size, validator output, and DTB
inspection results in the project notes.

Do not infer success from a running workflow, completed checkout, or a
successful kernel compile. A published artifact is not proof it boots.

## 6. Unresolved hardware questions

| Unknown | Required evidence |
|---|---|
| Which UART carries GNSS | Vendor paths disagree (`ttyS1` vs `ttyS2`). Test each with receiver attached and capture NMEA at supported baud rates; do not guess. |
| PPS replacement-build regression | Historical hardware path was verified November 4; repeat once-per-second `/dev/pps0` assertions on the replacement image, then separately validate Chrony PPS selection/lock |
| LCD GPIO controller mapping | Live gpiochip base/label/device inventory and verified offsets |
| LCD panel initialization | Compare vendor init sequence where recoverable; test reset, command/data, RGB565, offsets, rotation and backlight on the actual panel |
| LCD key mapping and long presses | Record each physical button's evdev code and timing behavior |
| WAN MAC | Inspect final runtime MAC and compare with factory offset 0x2e/vendor WAN override |
| Actual NTP service | Confirm chrony is the sole UDP/123 listener and a separate LAN client can query it |
| Time accuracy | Measure after GNSS fix/PPS; do not promise microsecond performance without measurements |

The GNSS antenna is disconnected during current bench work. An absent fix,
unreachable GNSS refclock, or missing PPS lock is expected in that condition.

## 7. U-Boot RAM address hazard — boot command is blocked

The captured environment reportedly has `fileaddr=0x80A00000`, while the
uImage load address/decompression destination has been reported as
`0x80000000`. This raises a potential source/destination overlap for a
large initramfs image. The proposed `0x84000000` TFTP address is **not yet
approved or proven safe**.

Before issuing any `tftpboot`/`bootm` command, inspect and record:
1. `bdinfo` output and actual installed RAM bounds.
2. U-Boot relocation address, malloc arena, stack, and reserved regions.
3. Exact compressed image size and uImage header load/entry addresses.
4. Decompressed kernel + initramfs footprint and end address.
5. Whether the candidate source buffer and destination ranges overlap any
   other live U-Boot region or one another.

Only then calculate a safe source buffer and destination. Do not run the
example sequence in `TEST-RAMBOOT.md` until this gate is explicitly cleared.
Never use `saveenv`.

## 8. Work order

1. Fix WAN DHCP to accept the assigned address only (`defaultroute '0'`, `peerdns '0'`); verify no DHCP default route or DNS is installed, and confirm LuCI can switch to static addressing.
2. Get a green CI build and capture the exact artifact/commit/run evidence.
3. Validate image format, rootfs contents, DTB, kernel config and package list.
4. Resolve U-Boot memory layout and approve a safe RAM-boot address.
5. RAM-boot only after steps 1–4; validate SSH and the single-uplink contract.
6. Validate SPI device and GPIO controller mapping before driving the panel.
7. Test LCD initialization, orientation, colors, keys, and backlight.
8. Verify chrony service and NTP client response on the bench network.
9. With antenna attached: identify UART, repeat the historically successful ~1 Hz `/dev/pps0` assertion test, and separately validate Chrony PPS source selection/lock against NIST/Cloudflare.
10. Verify LCD menu, long-press behavior and satellite list; keep `yspeed-status` as the shared data source.

**Safety rule:** no persistent flash writes under any circumstances unless the
user explicitly changes the project constraint.
