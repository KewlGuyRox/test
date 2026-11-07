# Vendor firmware inventory

This is a recovery inventory of the original vendor feature set. Evidence
sources include the captured vendor root filesystem, DTB, UCI configuration,
runtime snapshots, and strings recovered from vendor binaries. A binary string
proves that text exists in a binary; it does not by itself prove the associated
feature works or establish exact runtime behavior.

Purpose: use this as the replacement feature checklist, not as a design to copy.
The target remains stock OpenWrt + LuCI + chrony. Do not reuse untrusted vendor
web/backend binaries or enable flash-writing features.

## Evidence sources and confidence

| Evidence | What it can establish | Limitation |
|---|---|---|
| Vendor DTB | declared devices, buses, pins, compatible strings | does not prove electrical function or runtime driver binding |
| Vendor UCI files | configured defaults and options | does not prove every option was active |
| Vendor init scripts | startup order and invoked services | inspect full script and runtime logs for behavior |
| Vendor binary strings | embedded labels, paths, messages, option names | strings alone do not prove control flow or feature behavior |
| Runtime `/proc`, `/sys`, process captures | state at capture time | may not represent every boot/configuration |

## Web UI pages

Recovered Vue SPA page/component names under `/www/ys-gps/assets/` include:

`Login`, `MainLayout`, `Dashboard`, `GpsDashboard`, `GpsSettings`,
`LanSettings`, `WanSettings`, `NetworkSettings`, `ClientList`,
`RadiusSettings`, `PasswordSettings`, and `Sysupgrade`.

These are inventory names, not a requirement to reproduce the vendor SPA. The
replacement should expose only the useful appliance functions through LuCI.

## Backend API

The vendor `/usr/bin/ysgps` binary is reported to expose JWT-authenticated
routes including:

| Route family | Reported function |
|---|---|
| `/api/auth/*` | login/password/RADIUS authentication |
| `/api/gps/*` | GNSS configuration, status, ports, restart, WebSocket |
| `/api/network/*` | WAN/LAN/wireless and switch-port information |
| `/api/clients` | DHCP client list |
| `/api/system/*` | system information, logs, sysupgrade |

Treat route names as recovered inventory; validate exact methods, payloads, and
authorization from source or controlled captures before implementing compatible
interfaces.

## Settings recovered from UCI

| Key | Reported meaning/default |
|---|---|
| `ys-gps.gps.serial_port` | selectable `/dev/ttyS0`, `ttyS1`, or `ttyS2`; captured configs disagree on the active UART |
| `ys-gps.gps.baudrate` | e.g. 9600 |
| `ys-gps.gps.timezone` | timezone written to system config |
| `ys-gps.gps.language` | LCD language; replacement must remain English-only |
| `ys-gps.gps.weblanguage` | web language |
| `ys-gps.gps.lcd_rotation` | 0–3; captured default reported as 3 |
| `ys-gps.gps.backlight_sleep_time` | seconds; captured default reported as 120 |
| `ys-gps.root.password` | vendor web password configuration |
| `ys-gps.radius.*` | RADIUS settings |

Do not infer the receiver UART from the UCI default: vendor GPS services and
display paths reportedly refer to different serial devices. Confirm with the
receiver connected.

## GPS status fields

Recovered vendor status names include:

`fix_quality`, `satellites_visible`, `satellites_strong`,
`satellites_gps`, `satellites_beidou`, `satellites_glonass`,
`satellites_galileo`, `rtc_time`, `rtc_date`, `timezone_offset`,
`serial_connected`, and `satellites[]`.

The replacement status producer is intended to expose fix state, per-satellite
PRN/constellation/SNR/used state, satellites-used count, and a quality metric.
Its exact JSON schema and quality calculation must be verified against the
current source and actual receiver output. The replacement's SNR-derived
quality value is not necessarily numerically equivalent to the vendor metric.

## LCD and controls

Recovered display facts:

- ST7789 panel, 240x320.
- Vendor DTB declares SPI0 chip-select 1; vendor display binary references
  `/dev/spidev0.1`.
- SPI maximum frequency reported as 40 MHz.
- Vendor configuration comments report LCD reset GPIO14, D/C GPIO16, and
  backlight GPIO19.
- Vendor DTB/config report three front-panel keys on GPIO18, GPIO23, GPIO26.

The vendor kernel's sysfs GPIO base was reportedly 480, producing global GPIO
numbers 494/496/499 for offsets 14/16/19. This does not establish the global
base on a different kernel. The replacement must identify the correct live
GPIO controller and compute base + offset; it must not assume base 0 or reuse
the vendor global numbers.

### Recovered key/menu descriptions

Vendor binary strings/configuration reportedly describe:

- KEY1/KEY3 select settings options.
- KEY3 held for approximately 1 second enters settings.
- KEY1 held for approximately 1 second returns to the GPS/dashboard view.
- KEY2 applies/activates a selection.

Reported mapping:

| Physical key | GPIO | Intended Linux input code | Recovered role |
|---|---:|---|---|
| KEY1 | 18 | `KEY_UP` | select/back; long press returns to dashboard |
| KEY2 | 23 | `KEY_ENTER` | apply/confirm |
| KEY3 | 26 | `KEY_DOWN` | select/enter settings on long press |

These mappings and timing behaviors require physical verification. The current
replacement renderer source has basic three-page navigation; it does **not**
yet implement the complete settings menu or the documented 1-second long-press
actions. Full satellite-list presentation is also a parity item, not a completed
feature.

Reported settings menu items: language, network mode (LAN/WAN), rotation, and
factory reset. The replacement must remain English-only and single-uplink.
Do not implement factory reset or any other persistent flash-writing action.

## Non-web services

Recovered service/init-script names include:

- `usemode` — vendor network mode setup, including a DHCP-WAN mode.
- `gps-timesync` — gpsd-driven time synchronization; captured config reportedly
  had PPS disabled.
- `ys-ntp` — vendor NTP daemon/service.
- `rtc-boot`, `flash-id-password`, `bootcount`, and
  `openwrt-allinone`.

Inspect scripts before treating their behavior as established. The replacement
uses chrony and must verify that it is the only intended NTP server.

## Deliberately not reproduced

| Vendor feature | Replacement decision |
|---|---|
| Sysupgrade / factory reset | excluded; project is RAM-boot-only and must not write flash |
| RADIUS authentication | not required for the appliance; use standard LuCI/rpcd access controls |
| Vendor JWT stack | replace with LuCI sessions and rpcd ACLs |
| Wireless pages/radio | wired-only target; disable radio and omit wireless packages |
| Vendor `ysgps` backend | do not reuse; replace with maintained OpenWrt services and a shared status producer |
| Vendor LAN mode/bridge | excluded; one WAN uplink only, with no DHCP-provided route or DNS per operator constraint |

## Replacement feature-parity checklist

- [ ] GNSS receiver UART and baud rate proven with receiver attached.
- [ ] GNSS status fields and satellite list verified against live receiver data.
- [ ] RTC read/write behavior validated without changing persistent flash.
- [ ] Chrony NTP service verified from an independent LAN client.
- [ ] PPS validated with antenna/fix; only then enable PPS discipline.
- [ ] LCD SPI0 CS1 and panel initialization validated on hardware.
- [ ] Correct GPIO controller/base and line ownership validated.
- [ ] Rotation 3 and English-only display verified.
- [ ] Three physical keys mapped and debounced correctly.
- [ ] Settings menu and 1-second long-press behavior implemented and tested.
- [ ] Backlight timeout behavior implemented and tested.
- [ ] WAN DHCP address works without installing a default route or DNS.
- [ ] LuCI status/configuration pages expose supported replacement functions.
