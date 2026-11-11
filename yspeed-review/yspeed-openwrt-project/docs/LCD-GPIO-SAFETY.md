# LCD GPIO mapping safety — regression gate

The display renderer translates vendor device-tree SoC offsets into legacy
sysfs global GPIO numbers by identifying the exact MT7628 GPIO bank. It must
never infer a GPIO number from the gpiochip directory suffix, use the first
controller found, or add a SoC offset to an already-resolved number.

| Function | SoC offset | Bank | Bank-local line |
|---|---:|---:|---:|
| LCD SPI chip-select | 6 | 0 | 6 |
| LCD reset | 14 | 0 | 14 |
| LCD D/C | 16 | 0 | 16 |
| LCD backlight | 19 | 0 | 19 |

The global sysfs number is gpiochip base plus bank-local line. The base is
runtime-discovered. With the historical vendor base 512, the example numbers
are 526/528/531; these are not constants for a modern kernel.

## Implemented safety requirements

- Match the SoC controller label and gpio-bankN suffix.
- Select bank SoC offset / 32; use line SoC offset % 32.
- Require exactly one matching bank and a valid line within ngpio.
- Refuse ambiguous, missing, partial, or duplicate bank mappings; no fallback
  to unrelated gpiochips.
- gpio_claim() may export only LCD offsets 6, 14, 16, and 19. GPIO6 is the
  vendor-mapped SPI_CS line; it is muxed to GPIO and driven manually while the
  SPI controller runs with SPI_NO_CS. Front-panel keys (18/23/26), PPS (11),
  reset button (38), and status LED (44) remain kernel owned.
- gpio_claim() uses the resolver's global number directly; it never adds the
  SoC offset again.
- --selftest is read-only with respect to GPIO exports and checks bank,
  bank-local line, base arithmetic, and duplicate resolved numbers.

## Hardware validation still required

The build-time source guard and compiler validate the implementation, but
cannot prove that the running kernel exposes the expected gpiochip labels or
that the panel wiring matches the device tree. On the unit, inspect every
/sys/class/gpio/gpiochip*/{base,ngpio,label} entry and run
yspeed-display --selftest. Confirm offsets 6/14/16/19 resolve to bank-0 lines
6/14/16/19 and that no key/PPS/reset/LED line is exported. Confirm the driver
reports SPI_NO_CS and software CS on GPIO6. Only then run the panel test. RAM
boot only; never write persistent flash.


## Automated host-side resolver tests

The build runs `scripts/test-gpio-resolver.sh` before the OpenWrt compilation.
It compiles the production C translation unit into a host test harness and
redirects GPIO sysfs discovery to disposable temporary directories. The tests
exercise both vendor-style base 512 and mainline-style base 0, exact bank
selection, wrong-bank rejection, duplicate candidates, incomplete metadata,
undersized banks, unsupported bank 3, and the LCD-only GPIO claim allowlist.
No real GPIO sysfs export or hardware line is touched by these tests.

The MT7628 platform constraint is named `GPIO_BANK_COUNT=3` (banks 0..2).
The resolver computes the expected bank from the SoC offset and fails closed
outside that platform range. This is a hardware-specific contract, not a
portable assumption about all GPIO controllers.
