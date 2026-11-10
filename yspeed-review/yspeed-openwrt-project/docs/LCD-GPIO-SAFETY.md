# LCD GPIO mapping safety — regression gate

The display renderer translates vendor device-tree SoC offsets into legacy
sysfs global GPIO numbers by identifying the exact MT7628 GPIO bank. It must
never infer a GPIO number from the gpiochip directory suffix, use the first
controller found, or add a SoC offset to an already-resolved number.

| Function | SoC offset | Bank | Bank-local line |
|---|---:|---:|---:|
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
- gpio_claim() may export only LCD offsets 14, 16, and 19. Front-panel keys
  (18/23/26), PPS (11), reset button (38), and status LED (44) remain kernel
  owned.
- gpio_claim() uses the resolver's global number directly; it never adds the
  SoC offset again.
- --selftest is read-only with respect to GPIO exports and checks bank,
  bank-local line, base arithmetic, and duplicate resolved numbers.

## Hardware validation still required

The build-time source guard and compiler validate the implementation, but
cannot prove that the running kernel exposes the expected gpiochip labels or
that the panel wiring matches the device tree. On the unit, inspect every
/sys/class/gpio/gpiochip*/{base,ngpio,label} entry and run
yspeed-display --selftest. Confirm offsets 14/16/19 resolve to bank-0 lines
14/16/19 and that no key/PPS/reset/LED line is exported. Only then run the
panel test. RAM boot only; never write persistent flash.
