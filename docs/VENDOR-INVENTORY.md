# Vendor firmware inventory

Recovered from `yspeed-live-system.tar.gz` (captured vendor rootfs), the
`/usr/bin/ysgps` backend binary strings, and `/etc/config/*`.

Purpose: this is the **feature list**, not a design to copy. The replacement is
stock OpenWrt + LuCI + chrony. No vendor SPA, no vendor backend binary, no JWT,
no RADIUS.

## Web UI pages (Vue SPA under `/www/ys-gps/assets/`)

`Login`, `MainLayout`, `Dashboard`, `GpsDashboard`, `GpsSettings`,
`LanSettings`, `WanSettings`, `NetworkSettings`, `ClientList`,
`RadiusSettings`, `PasswordSettings`, `Sysupgrade`.

## Backend API (`/usr/bin/ysgps`, C, JWT-authenticated)

| Route | Function |
|---|---|
| `/api/auth/login`, `/api/auth/chgpass`, `/api/auth/radius` | authentication |
| `/api/gps/config`, `/api/gps/status`, `/api/gps/ports`, `/api/gps/restart` | GNSS control |
| `/api/gps/ws` | live GPS push over WebSocket |
| `/api/network/wan`, `/api/network/lan`, `/api/network/wireless` | addressing |
| `/api/network/ports`, `/api/network/ports/zone`, `/api/network/info` | switch ports |
| `/api/clients` | DHCP client list |
| `/api/system/info`, `/api/system/syslog` | status |
| `/api/system/sysupgrade/{upload,verify,flash,reset,status}` | firmware update |

## Settings actually stored (`uci ys-gps.*`)

| Key | Meaning |
|---|---|
| `ys-gps.gps.serial_port` | `/dev/ttyS0`, `/dev/ttyS1`, `/dev/ttyS2` — user selectable |
| `ys-gps.gps.baudrate` | e.g. 9600 |
| `ys-gps.gps.timezone` | POSIX TZ, also written to `system.@system[0].timezone` |
| `ys-gps.gps.language` | LCD language (`chinese` / `english`) |
| `ys-gps.gps.weblanguage` | web language |
| `ys-gps.gps.lcd_rotation` | 0–3 (shipped `3`) |
| `ys-gps.gps.backlight_sleep_time` | seconds (shipped `120`) |
| `ys-gps.root.password` | web password |
| `ys-gps.radius.{enabled,server,port,secret,timeout}` | RADIUS auth |

## GPS status fields (vendor `/api/gps/status`)

`fix_quality`, `satellites_visible`, `satellites_strong`, `satellites_gps`,
`satellites_beidou`, `satellites_glonass`, `satellites_galileo`, `rtc_time`,
`rtc_date`, `timezone_offset`, `serial_connected`, `satellites[]`.

The replacement `yspeed-status` covers fix, per-satellite `prn`/`gnssid`/`snr`/
`used`, `satellites_used`, and `quality`.

## LCD menu (from vendor binary strings)

```
Key functions: KEY1/KEY3=Select option in settings,
KEY3 long press 1s=Enter settings,
KEY1 long press 1s=Return to GPS,
KEY2 press=Apply
```

| Physical | GPIO | Kernel code | Short press | 1 s long press |
|---|---|---|---|---|
| KEY1 | 18 | `KEY_UP` | previous page | back to dashboard / exit settings |
| KEY2 | 23 | `KEY_ENTER` | — | (vendor: configure/apply) |
| KEY3 | 26 | `KEY_DOWN` | next page | enter settings menu |

Vendor settings menu covered: language, network mode (lan/wan), rotation,
factory reset. Prompts seen: `Right:Apply Config`, `Hold KEY1:Back to Dashboard`,
`KEY1/KEY3:Select`.

## Non-web services

- `/etc/init.d/usemode` → `usemode --iot-mode`; first boot runs `ethmode dhcpwan`
  and touches `/etc/.network_configured`
- `/etc/init.d/gps-timesync` → gpsd-driven time sync (`pps_enabled 0`)
- `/etc/init.d/ys-ntp` → `/usr/sbin/ys-ntp`, custom NTP server, opens UDP/123
- `/etc/init.d/rtc-boot`, `flash-id-password`, `bootcount`, `openwrt-allinone`

## Deliberately not reproduced

| Vendor feature | Why |
|---|---|
| `sysupgrade` / factory reset pages | this project is RAM-boot only; flash writes are forbidden |
| RADIUS authentication | unnecessary for an NTP appliance; LuCI sessions suffice |
| JWT auth | LuCI + `rpcd` ACLs are standard and audited |
| Wireless pages | hardware is wired-only; `wmac` disabled in the DTB |
| Custom `ysgps` backend | unmaintained vendor binary; replaced by `yspeed-status` + `rpcd` |
