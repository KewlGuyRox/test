# Decisions — do not revert these without new evidence

Each entry below was established from evidence in this repository (vendor DTB,
vendor rootfs, flash dump, OpenWrt sources, or CI logs). Several were
accidentally reverted once already. **Before changing any of these, quote the
contradicting evidence.** If you have none, leave it alone.

Branch note: the working code is on `build/final`. `docs/` also exists on the
PR branch. Verify which branch you are editing.

---

## 1. gpsd uses `list devices`, not `option device`

```uci
config gpsd core
        list devices '/dev/ttyS1'
```

**Why:** OpenWrt 25.12's `utils/gpsd/files/gpsd.init` declares
`'devices:list(string)'` and iterates `for device in $devices`. A singular
`option device` fails UCI validation and leaves gpsd with **no serial port**, so
GNSS dies silently.

**Was reverted once** to `option device` on the belief that it was the vendor
form. The vendor runs an older gpsd; this tree does not.

## 2. Native SPI CS1, no `cs-gpios`

`pinctrl-0 = <&spi_pins>, <&spi_cs1_pins>;` with `st7789@1 { reg = <1>; }`.

**Why:** vendor `yspeed-original.dtb` claims the `spi cs1` **function**
(`groups = "spi cs1"; function = "spi cs1";`), declares `reg = <1>`, contains
**no `cs-gpios` property**, and the captured `/proc/modules` shows `spi_gpio`
with **0 users**. GPIO6 *is* the CS1 pin; the only question is which function it
takes, and those are mutually exclusive. Adding `cs-gpios` alongside the pinmux
would give one pin two owners.

**Status:** GPIO6/software-CS remains a *documented fallback experiment* only,
to be tried if native CS1 tests fail on hardware. Do not add it to the design.

## 3. PPS refclocks are enabled; `noselect` was removed deliberately

```conf
refclock SHM 0 refid GPS precision 1e-1 poll 0
refclock PPS /dev/pps0 refid PPS lock GPS precision 1e-7 poll 0 prefer
```

**Why:** PPS was physically verified on 2026-11-05 — `ppstest /dev/pps0`
returned consecutive assertions ~1 s apart (sequences 3140-3144 at
`1793860737.002265851`, …, `1793860741.002268005`; later 3370-3381) on IRQ40,
while `/dev/pps1` timed out. The captured `/proc/interrupts` agrees:
`40: 49145 10000600.gpio 11 pps.-1`.

`lock GPS` keeps PPS subordinate to a valid SHM second, so an unlatched receiver
cannot move the clock on its own.

**Was reverted to `noselect` once.** Regression test on 6.12 is still
outstanding; that is a test, not a reason to disable the feature.

## 4. WAN takes DHCP default route and DNS

```uci
option proto 'dhcp'
option defaultroute '1'
option peerdns '1'
```

**Why:** with both `0`, the lease yields an address but no gateway and no
resolver, so the NIST/Cloudflare fallback can never work. This appliance does
not forward, NAT or proxy DNS (firewall `forward` is `REJECT`, no LAN zone,
dnsmasq `localservice`), so its own routing table cannot affect another host on
the same segment.

## 5. The package is `kmod-spi-dev`

```make
define KernelPackage/spi-dev
  KCONFIG:=CONFIG_SPI_SPIDEV CONFIG_SPI=y CONFIG_SPI_MASTER=y
```

There is **no `kmod-spi-spidev`** in 25.12. The kernel resolves
`CONFIG_SPI_SPIDEV` to `=m`, so `/dev/spidev0.1` exists only when `kmod-spi-dev`
is in `DEVICE_PACKAGES`. It was missing; without it the LCD can never work.

## 6. The spidev OF match is a quilt patch

`patches/999-spidev-yspeed-match.patch` is copied into
`target/linux/ramips/patches-6.12/` before `make target/linux/prepare`.

**Why:** it replaces an inline Python edit of `spidev.c`, is reviewable in
`git diff`, and applies with the other kernel patches. The patch uses
`.data = &spidev_of_check`, the same form as every upstream entry (that symbol
exists in 6.12). `build.sh` asserts the entry is present after prepare.

## 7. The status parser searches substrings, and that is correct

`json_string()` builds the needle `"key":"` and `strstr()`s the whole document.
Nesting is therefore irrelevant: `gnss.fix`, `ntp.state`, `ntp.reference` and
`network.ipv4` are all found. A review claimed these fall back to `UNAVAILABLE`;
that was tested with a port of the exact algorithm and is **false**.

Do not "fix" the parser by flattening the JSON or rewriting it to a tree walk.
If fields are wrong on hardware the cause is elsewhere — most likely `popen()`
failing; extend `--selftest` to print the raw status output first.

## 8. GPIO chip selection: label match, else a sole candidate, else refuse

The SoC GPIO block is exposed as **several 32-line banks**, not one chip:

```
gpiochip0  base 512  pins  0-31
gpiochip1  base 544  pins 32-63
gpiochip2  base 576  pins 64-95
```

So a pin is **not** `base + offset` for whichever bank happens to match. The
bank that owns the pin must be chosen first, then the offset applied relative to
that bank's first pin. Verified against `/sys/kernel/debug/gpio` on the device:
`pps-gps` = gpio523 (512+11), `LCD Up` = gpio530 (512+18), `reset` = gpio550
(544+6), `green:status` = gpio556 (544+12).

An earlier version matched any chip whose label contained `10000600` and added
the offset to that base, resolving pin 19 to 576+19 = 595 instead of 512+19 =
531. The renderer then drove pins 78/80/83, which are not the panel, and the
display stayed dark. Do not reintroduce "first label match wins".

`--selftest` prints the chip label and the rule that fired. Do not add a blind
"first chip wide enough" fallback.

## 9. Kernel symbol assertions are warnings, not gates

They previously aborted builds **after image assembly had completed**, because
`find */linux-*/.config` selected a file that was not the kernel config (it
reported `CONFIG_SPI` as absent while SPI was compiling). The authoritative
gates are artifact-level: initramfs present and 20-30 MiB decompressed, DTB
contains `st7789@1` and the three LCD keys.

## 10. Compiler cache lives in `dl/.ccache`

`rules.mk` **overrides** `CCACHE_DIR`, and its assignment is **indented** — an
`^export` anchor silently fails. `ccache` is also not installed by the workflow.
`build.sh` therefore installs ccache, rewrites the indented line to point at
`$SRC/dl/.ccache` (already cached by CI), passes `CONFIG_CCACHE=y` on the make
command line, and prints `compiler cache : enabled at ...` in the first minute.

Do not replace this with a workflow-only change; that requires the OAuth
`workflow` scope, which is unavailable here.

## 11. Builds are dispatched from `build/final`, one at a time

For `pull_request`, GitHub evaluates the **whole PR diff** against the workflow's
`paths` filter, so *any* push to the PR branch restarts the build — including a
docs-only commit. Builds are therefore dispatched manually from `build/final`,
which has its own concurrency group. Batch changes; do not push mid-build.

## 12. `gpio_chip_base_for()` returns a GPIO number, not a base

Despite the name, the function returns the **final global GPIO number**: it
selects the bank that owns the pin and adds the offset within that bank. Callers
must use the value directly.

Two callers previously did `returned + offset`, giving:

| Signal | Correct | Double-added | That pin actually is |
|---|---|---|---|
| Reset | 526 | 540 | pin 28 |
| D/C | 528 | 544 | pin 32 |
| Backlight | 531 | **550** | **the front-panel reset button** |

Driving 550 as an output would have toggled the hardware reset line of the
appliance while trying to switch on the backlight.

The rule is now stated at both call sites with a "never reintroduce
resolved + offset" comment. If the function is ever refactored to return a base,
both call sites and the `--selftest` printer must change together — and
`--selftest` must show 526 / 528 / 531 for reset / D/C / backlight.