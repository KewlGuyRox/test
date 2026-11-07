# RAM-boot testing — non-persistent procedure

This procedure is for an OpenWrt initramfs image. The intended test must run
entirely from RAM. A power cycle should revert to the existing firmware,
provided no persistent command or flash write was issued.

**Current status: RAM-boot command sequence is BLOCKED.** Do not copy/paste a
TFTP/boot command until the U-Boot memory-layout gate below has been completed
for the exact image being tested.

## Non-negotiable safety rules

Never run or invoke any flash/persistent operation:

```text
saveenv, erase, cp.b to flash, mw.b to flash, sf, mtd, nand, sysupgrade
```

Do not modify U-Boot environment persistently. A transient `setenv` is volatile,
but even transient changes should be limited to a reviewed boot procedure.
Do not issue commands from an unverified address recommendation.

## Requirements

| Item | Detail |
|---|---|
| Serial console | 57600 8N1 on `ttyS0` (verify against the actual unit) |
| Network | Ethernet connected to WAN port (`eth0.2`) |
| DHCP | Address acquisition only; do not accept a default gateway or DNS |
| TFTP server | Same L2 segment, serving the exact validated image |
| Image | `openwrt-ramips-mt76x8-yspeed_ys-gps-12832-initramfs-kernel.bin` |
| Recovery | Power cycle only; no flash writes |

## Mandatory pre-boot memory validation

Capture and review `bdinfo` from the actual U-Boot prompt. Determine:

- Physical RAM start/end and any memory banks.
- U-Boot relocation address and reserved relocation/malloc/stack regions.
- Image source buffer range (TFTP destination plus exact file size).
- uImage header load and entry addresses.
- Decompressed kernel + initramfs destination range and actual end address.
- All overlap relationships among source, destination, and U-Boot reserved
  memory.

The reported environment value `fileaddr=0x80A00000` and decompression start
`0x80000000` suggest a possible overlap, but they do not establish a safe
replacement address. `0x84000000` is only a candidate proposed in earlier
analysis; it is **not validated**. With 128 MiB RAM, it may still conflict with
the image footprint or U-Boot-reserved memory. Do not use it until the full
ranges are calculated.

Record the exact image size and header before calculating. The safe TFTP
source address must be selected from the measured map, not guessed. If the
map cannot be established, stop and do not boot.

## Boot command

**Intentionally omitted pending memory-map approval.** Once the pre-boot
validation is complete, add the reviewed commands here with the exact source
address, filename, and `bootm` address. The command must not write flash or
save environment state.

## Expected boot evidence

Capture the complete serial log from the first boot message through userspace.
Expected indicators include:

- Kernel version and command line match the artifact.
- Initramfs mounts and `/init` starts.
- SSH becomes available on the intended WAN-side address.
- No kernel panic, rootfs error, or reboot loop.

A `VFS: Cannot open root device` panic indicates an image/rootfs problem.
Stop and retain the complete log.

## Checks after boot

First verify the network contract. The device should obtain its DHCP address
but must **not** install a DHCP-provided default route or DNS server.

```sh
ip -4 addr show dev eth0.2
ip route
cat /tmp/resolv.conf.d/resolv.conf.auto 2>/dev/null
cat /etc/resolv.conf
ip -4 addr show | grep -c '192.168.88'   # must print 0
```

Expected: the WAN interface has its leased IPv4 address; no default route is
learned from DHCP; no DHCP-provided nameserver appears in the resolver
configuration. Do not use public DNS or upstream route reachability as a
pass/fail test for this isolated bench configuration.

Then verify services:

```sh
# chrony is the NTP service; no competing sysntpd listener
ps | grep -c '[s]ysntpd'                 # must print 0
ss -lunp | grep ':123'
chronyc tracking
chronyc sources

# display preflight first; do not start renderer until GPIO ownership is checked
/usr/sbin/yspeed-display --selftest

# status producer
/usr/sbin/yspeed-status | head -c 600
```

Do not run the LCD color test until the selected GPIO chip, offsets, line
ownership, SPI node, and panel wiring have been verified. The renderer's
self-test is not a substitute for confirming that it selected the correct
GPIO controller.

## Antenna disconnected

The GNSS antenna is currently disconnected. In this state:

- No satellite fix or PPS lock is expected.
- The status display must report no fix honestly; it must not invent satellite
  data or imply GNSS synchronization.
- Chrony may remain unsynchronized if no approved upstream source is configured
  and reachable under the network restrictions.
- Do not use lack of GNSS fix/PPS as a firmware failure while the antenna is
  disconnected.

## Test record

For every attempt, record:

- Git commit SHA and CI run URL.
- Artifact filename, byte size, and validator output.
- U-Boot `bdinfo`, image header, and calculated memory ranges.
- Full serial boot log.
- Runtime IP, routes, resolver contents, SSH result.
- SPI/GPIO inventory and display test observations.
- Chrony listener and client query results.

No test is complete based on a successful build alone.
