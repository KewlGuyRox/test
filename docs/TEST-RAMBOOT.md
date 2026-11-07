# Testing a RAM-boot image without changing anything permanent

The image is an **initramfs kernel**: the entire rootfs lives in RAM. Nothing is
written to SPI flash, the U-Boot environment is not modified, and the factory
partition is never written. **Revert = power cycle.**

## Requirements

| Item | Detail |
|---|---|
| Serial console | 57600 8N1 on `ttyS0` (not 115200) |
| Network | Ethernet into the **WAN** port (`eth0.2`) |
| DHCP server | Any; the image takes address, route and DNS |
| TFTP server | Serving the `.bin`, same L2 segment |
| Image | `openwrt-ramips-mt76x8-yspeed_ys-gps-12832-initramfs-kernel.bin` |

## Never run these in U-Boot

```
saveenv   erase   cp.b   mw.b   sf   mtd   nand   sysupgrade
```

`setenv` without `saveenv` is RAM-only and safe.

## Boot

```
setenv ipaddr 10.10.10.2
setenv serverip 10.10.10.3
tftpboot 0x84000000 openwrt-ramips-mt76x8-yspeed_ys-gps-12832-initramfs-kernel.bin
bootm 0x84000000
```

### Why 0x84000000

The recorded environment has `fileaddr=80A00000`, only 10 MiB above the
`0x80000000` decompression start. A kernel-only image once fit there with ~376 KiB
of margin; a real 20–30 MiB initramfs will not, and decompression would overwrite
the compressed source mid-boot. `0x84000000` leaves the lower 64 MiB clear.
Check with `bdinfo` before booting.

## Expected

```
Console: 57600 8N1
[    0.000000] Linux version 6.12.112 ...
[    ...     ] Run /init as init process
```

A panic mentioning `VFS: Cannot open root device` means the image is kernel-only.
Stop and report it.

## Checks after boot

```sh
# single-uplink contract
ip -4 addr show dev eth0.2
ip route | grep default
cat /etc/resolv.conf
ip -4 addr show | grep -c '192.168.88'   # must print 0

# chrony owns NTP
ps | grep -c '[s]ysntpd'                 # must print 0
chronyc tracking
chronyc sources

# display
/usr/sbin/yspeed-display --selftest
/usr/sbin/yspeed-display --test
/etc/init.d/yspeed-display restart

# data the panel draws
/usr/sbin/yspeed-status | head -c 600
```

## Without an antenna

Everything must still work and the panel must say so honestly:

- `fix` = `NO_FIX`, `quality` = `0`, `satellites` = `[]`
- the clock keeps running
- `chronyc sources` shows the SHM/PPS refclocks unreachable and the upstream
  servers reachable if the bench has internet

A frozen clock, blank panel or malformed JSON in this state is a bug worth
reporting before the antenna is ever connected.
