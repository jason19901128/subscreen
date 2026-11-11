#!/usr/bin/env bash
# Update Subscreen BRIDGE_HOST to this Mac's LAN IP, optionally flash firmware.
# Usage:
#   ./scripts/update-bridge-host.sh           # only rewrite platformio.ini
#   ./scripts/update-bridge-host.sh --upload  # rewrite + flash device
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INI="$ROOT/firmware/platformio.ini"
DO_UPLOAD=0

for arg in "$@"; do
  case "$arg" in
    --upload|-u) DO_UPLOAD=1 ;;
    --help|-h)
      echo "Usage: $0 [--upload]"
      exit 0
      ;;
  esac
done

find_ip() {
  local ip=""
  for iface in en0 en1 en2; do
    ip="$(ipconfig getifaddr "$iface" 2>/dev/null || true)"
    if [[ -n "$ip" ]]; then
      echo "$ip"
      return 0
    fi
  done
  return 1
}

find_pio() {
  if command -v pio >/dev/null 2>&1; then
    command -v pio
    return 0
  fi
  for candidate in \
    "$HOME/Library/Python/3.14/bin/pio" \
    "$HOME/Library/Python/3.13/bin/pio" \
    "$HOME/Library/Python/3.12/bin/pio" \
    "$HOME/.platformio/penv/bin/pio" \
    /usr/local/bin/pio \
    /opt/homebrew/bin/pio
  do
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  if python3 -c "import platformio" >/dev/null 2>&1; then
    echo "python3 -m platformio"
    return 0
  fi
  return 1
}

find_usb_port() {
  local ports=()
  while IFS= read -r p; do
    [[ -n "$p" ]] && ports+=("$p")
  done < <(ls /dev/cu.usbmodem* 2>/dev/null || true)
  if ((${#ports[@]} == 0)); then
    return 1
  fi
  # Prefer the first usbmodem device; ESP32-C6 usually shows as cu.usbmodem*
  echo "${ports[0]}"
}

IP="$(find_ip || true)"
if [[ -z "$IP" ]]; then
  echo "error: no LAN IP (en0/en1/en2). Connect Wi-Fi or Ethernet first." >&2
  exit 1
fi

OLD_IP="$(grep -oE 'BRIDGE_HOST=\\"[0-9.]+\\"' "$INI" 2>/dev/null | head -1 | sed 's/.*\\"\([0-9.]*\)\\".*/\1/' || true)"

if ! grep -q "BRIDGE_HOST" "$INI"; then
  echo "error: BRIDGE_HOST not found in $INI" >&2
  exit 1
fi

sed -i '' "s/-D BRIDGE_HOST=\\\\\"[0-9.]*\\\\\"/-D BRIDGE_HOST=\\\\\"${IP}\\\\\"/" "$INI"

if [[ "$OLD_IP" == "$IP" ]]; then
  echo "BRIDGE_HOST already $IP"
else
  echo "BRIDGE_HOST: ${OLD_IP:-?} -> $IP"
fi

if curl -s --connect-timeout 2 "http://${IP}:8765/health" >/dev/null 2>&1; then
  echo "Bridge health: ok (http://${IP}:8765)"
else
  echo "warn: Bridge not reachable at http://${IP}:8765 (launchd may be down)"
fi

if [[ "$DO_UPLOAD" -ne 1 ]]; then
  echo "Done (config only). Re-run with --upload to flash the device."
  exit 0
fi

PORT="$(find_usb_port || true)"
if [[ -z "$PORT" ]]; then
  echo "error: no /dev/cu.usbmodem* found. Plug in the ESP32 USB cable." >&2
  exit 1
fi

sed -i '' "s|^upload_port = .*|upload_port = ${PORT}|" "$INI"
sed -i '' "s|^monitor_port = .*|monitor_port = ${PORT}|" "$INI"
echo "upload_port -> $PORT"

PIO_CMD="$(find_pio || true)"
if [[ -z "$PIO_CMD" ]]; then
  echo "error: PlatformIO not found. Install with: pip3 install platformio" >&2
  exit 1
fi

echo "Uploading firmware..."
PAUSE_FLAG="$HOME/.cursor/subscreen/usb_pause"
mkdir -p "$(dirname "$PAUSE_FLAG")"
# Release USB serial held by Bridge so esptool can flash.
touch "$PAUSE_FLAG"
cleanup_pause() { rm -f "$PAUSE_FLAG"; }
trap cleanup_pause EXIT
sleep 2
# shellcheck disable=SC2086
$PIO_CMD run -t upload -d "$ROOT/firmware"
cleanup_pause
trap - EXIT
echo "Done. If the screen still shows OFFLINE, press RESET once."
