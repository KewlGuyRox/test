#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:?Usage: build.sh <OpenWrt source directory>}"

# Default DHCP on eth0.2; static WAN is available in LuCI, and eth0.1 has no L3 interface.
GPSD_CONFIG="$PROJECT_DIR/files/etc/config/gpsd"
STATUS_REFRESH="$PROJECT_DIR/files/usr/sbin/yspeed-status-refresh"
python3 - "$GPSD_CONFIG" "$STATUS_REFRESH" <<'PYGPS'
import pathlib, sys
gpsd=pathlib.Path(sys.argv[1]).read_text()
status=pathlib.Path(sys.argv[2]).read_text()
if "list devices '/dev/ttyS1'" not in gpsd:
    raise SystemExit("GPSD UART contract failed: ttyS1 must be first for SHM0")
if "/dev/ttyS2" not in status:
    raise SystemExit("Status UART contract failed: ttyS2 must be second for UI")
print("GPS UART contract OK: ttyS1 first for SHM0; ttyS2 second for UI")
PYGPS

python3 - "$PROJECT_DIR/files/etc/config/network" <<'PYNET'
import pathlib, sys
text=pathlib.Path(sys.argv[1]).read_text()
sections=[]; current=None
for raw in text.splitlines():
    line=raw.strip()
    if not line or line.startswith("#"):
        continue
    parts=line.split()
    if parts[0]=="config" and len(parts)>=3:
        current={"type":parts[1],"name":parts[2].strip("'"),"options":{}}
        sections.append(current)
    elif current and parts[0] in ("option","list") and len(parts)>=3:
        current["options"][parts[1]]=parts[2].strip("'")
interfaces=[x for x in sections if x["type"]=="interface"]
wan=[x for x in interfaces if x["name"]=="wan"]
if len(wan)!=1 or wan[0]["options"].get("device")!="eth0.2" or wan[0]["options"].get("proto")!="dhcp":
    raise SystemExit("Factory default WAN must use DHCP on eth0.2")
if wan[0]["options"].get("defaultroute")!="1" or wan[0]["options"].get("peerdns")!="1":
    raise SystemExit("WAN DHCP must accept the DHCP-provided default route and peer DNS")
for item in interfaces:
    name=item["name"]; opts=item["options"]
    if name=="lan":
        raise SystemExit("LAN Layer-3 interface must not be configured")
    if name!="loopback" and opts.get("device")=="eth0.1":
        raise SystemExit(f"eth0.1 must not have a Layer-3 interface ({name})")
print("Network contract OK: ordinary DHCP eth0.2 with default route and peer DNS; no LAN or eth0.1 L3")
PYNET

make -C "$SRC" defconfig
