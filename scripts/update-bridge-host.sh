#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INI="$ROOT/firmware/platformio.ini"

IP="$(ipconfig getifaddr en0 2>/dev/null || true)"
if [[ -z "$IP" ]]; then
  IP="$(ipconfig getifaddr en1 2>/dev/null || true)"
fi
if [[ -z "$IP" ]]; then
  echo "error: no LAN IP (en0/en1). Connect Wi-Fi or Ethernet first." >&2
  exit 1
fi

if grep -q "BRIDGE_HOST" "$INI"; then
  sed -i '' "s/-D BRIDGE_HOST=\\\\\"[0-9.]*\\\\\"/-D BRIDGE_HOST=\\\\\"${IP}\\\\\"/" "$INI"
else
  echo "error: BRIDGE_HOST not found in $INI" >&2
  exit 1
fi

echo "BRIDGE_HOST -> $IP ($INI)"

if [[ "${1:-}" == "--upload" ]]; then
  PIO="${PIO:-}"
  for candidate in "$PIO" pio "$HOME/Library/Python/3.14/bin/pio" "$HOME/.platformio/penv/bin/pio"; do
    [[ -n "$candidate" && -x "$candidate" ]] && PIO="$candidate" && break
  done
  if [[ -z "${PIO:-}" ]]; then
    echo "error: pio not found. Install PlatformIO or set PIO=/path/to/pio" >&2
    exit 1
  fi
  "$PIO" run -t upload -d "$ROOT/firmware"
fi
