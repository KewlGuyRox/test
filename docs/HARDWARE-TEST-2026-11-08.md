# Hardware test — 2026-11-08 (RAM boot, appliance on the bench)

First confirmed RAM-boot of a build from this project. Recorded because it both
validates most of the stack and exposed a real display defect that no amount of
source review had caught.

## What was tested

```
DISTRIB_RELEASE  25.12.6
DISTRIB_REVISION r33311-981c978eff
DISTRIB_TARGET   ramips/mt76x8
kernel           6.12.112
root             tmpfs on / (initramfs, 60.3 MB) — nothing written to flash
RAM              123520 kB total
cmdline          console=ttyS0,57600 spidev.bufsiz=262144
```

## What works — verified, not inferred

| Claim | Evidence from the device |
|---|---|
| SPI device tree node binds | `readlink -f /sys/bus/spi/devices/spi0.1/driver` → `/sys/bus/spi/drivers/spidev` |
| spidev module present | `/dev/spidev0.1` is `crw------- 153,0` |
| Quilt patch effective | `modalias` = `spi:st7789-spidev`; dmesg shows the driver attaching to it |
| Renderer runs and holds the panel | PID 4516, `fd 3 -> /dev/spidev0.1` |
| Renderer reads the keys | `fd 4 -> /dev/input/event0` |
| CS1 pinmux is native | pinctrl: `pin 6 (io6): 10000b00.spi ... function spi cs1 group spi cs1` |
| Single-uplink network | `eth0.2` = 10.10.50.253, `defaultroute=1`, `peerdns=1`, udhcpc running |
| Services up | dnsmasq :53, dropbear :22, uhttpd :80/:443, gpsd :2947, chronyd running |
| Clean boot | `dmesg | grep -iE 'error|fail|panic|oops'` returned nothing |
| GPIO sysfs available | `/sys/class/gpio` with gpiochips 512 / 544 / 576 |

## The defect this test found: GPIO banks

`--selftest` reported:

```
LCD_RESET    : offset 14 -> sysfs gpio590  [chip label '10000600.gpio-bank2', label match]
LCD_DC       : offset 16 -> sysfs gpio592  [chip label '10000600.gpio-bank2', label match]
LCD_BACKLIGHT: offset 19 -> sysfs gpio595  [chip label '10000600.gpio-bank2', label match]
```

Those are pins **78 / 80 / 83**. The panel is on pins **14 / 16 / 19**, which are
**526 / 528 / 531**.

The SoC exposes its GPIO block as three 32-line banks:

```
gpiochip0  base 512  pins  0-31
gpiochip1  base 544  pins 32-63
gpiochip2  base 576  pins 64-95
```

The resolver matched the first chip whose label contained `10000600` — bank2 —
and added the offset to that bank's base: `576 + 19 = 595`.

The device's own `/sys/kernel/debug/gpio` gives the correct rule:

```
gpio-523  pps-gps       = 512 + 11   (bank0)
gpio-530  LCD Up        = 512 + 18   (bank0)
gpio-535  LCD Enter     = 512 + 23   (bank0)
gpio-538  LCD Down      = 512 + 26   (bank0)
gpio-550  reset         = 544 + 6    (bank1)
gpio-556  green:status  = 544 + 12   (bank1)
```

Pins 38 and 44 land in bank1; 14/16/19 land in bank0. The correct rule is: find
the bank whose pin range contains the offset, then add the offset relative to
that bank's first pin.

**Fixed in `4f140b2`.** The renderer was driving three pins that are not
connected to the panel, which is why the display stayed dark and why the
backlight read `0`.

## Secondary findings

- **`/etc/lvgl_st7789.conf` did not exist.** The init script's comment claimed it
  was generated from UCI, but nothing created it, so the renderer ran on compiled
  defaults. The init script now writes the documented defaults once.
- **`uci show ys-gps` → "Entry not found".** No `/etc/config/ys-gps` is shipped.
  The renderer does not read it, so this is harmless today but the vendor config
  contract is not reproduced.
- **`SPI driver spidev has no spi_device_id for yspeed,st7789-spidev`** is a
  benign warning. Binding is via the OF match table, which succeeded. Do not
  "fix" it by adding an `spi_device_id` unless something actually fails.
- Kernel build string reports `runnervm..., #0 Sat Oct 17` — the release build
  stamp, not evidence of a stale build.

## How to verify the next image

On the device, with the antenna disconnected (no fix expected):

```sh
/etc/init.d/yspeed-display stop
/usr/sbin/yspeed-display --selftest
# expect: LCD_RESET 526, LCD_DC 528, LCD_BACKLIGHT 531, /dev/spidev0.1 present
/usr/sbin/yspeed-display --test
# expect: red, green, blue, white, black, ~700 ms each
/etc/init.d/yspeed-display start
```

Interpretation:

| Symptom | Meaning |
|---|---|
| selftest shows 526/528/531 and `--test` cycles colours | LCD is fixed |
| selftest correct, `--test` shows black only | panel init sequence or backlight, not wiring |
| selftest correct, colours but rotated | MADCTL, set `rotation` in `/etc/lvgl_st7789.conf` |
| selftest shows 590/592/595 again | regression to the flat `base + offset` rule |

## Still unproven

- Pixels on the glass (until the image with `4f140b2` is booted).
- GNSS: gpsd is pointed at `/dev/ttyS1` and uart1 is pinmuxed on pins 45/46, but
  with no antenna there is no fix. The UART choice is still unconfirmed.
- PPS regression on 6.12: `/dev/pps0` and `irq/40-pps-gps` exist in the process
  list, but assertions were not observed in this session.
