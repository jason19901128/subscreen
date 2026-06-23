#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOKS_SRC="$ROOT/hooks/subscreen-update.py"
HOOKS_DST="$HOME/.cursor/subscreen/subscreen-update.py"
BRIDGE_DST="$HOME/.cursor/subscreen/bridge"
PLIST="$HOME/Library/LaunchAgents/com.subscreen.bridge.plist"

echo "==> Installing subscreen hook script"
mkdir -p "$HOME/.cursor/subscreen"
cp "$HOOKS_SRC" "$HOOKS_DST"
chmod +x "$HOOKS_DST"

echo "==> Installing bridge into ~/.cursor/subscreen/bridge"
rm -rf "$BRIDGE_DST"
mkdir -p "$BRIDGE_DST"
cp "$ROOT/bridge/main.py" "$ROOT/bridge/state.py" "$ROOT/bridge/usage.py" "$ROOT/bridge/context.py" "$ROOT/bridge/requirements.txt" "$BRIDGE_DST/"

echo "==> Installing Python bridge dependencies"
python3 -m pip install --user -r "$BRIDGE_DST/requirements.txt"

echo "==> Merging hooks into ~/.cursor/hooks.json"
python3 - <<PY
import json
from pathlib import Path

root = Path("$ROOT")
home = Path.home()
target = home / ".cursor" / "hooks.json"
fragment = json.loads((root / "hooks" / "hooks.fragment.json").read_text())

if target.exists():
    data = json.loads(target.read_text())
else:
    data = {"version": 1, "hooks": {}}

data.setdefault("version", 1)
data.setdefault("hooks", {})

for event, entries in fragment.get("hooks", {}).items():
    existing = data["hooks"].get(event, [])
    commands = {e.get("command") for e in existing if isinstance(e, dict)}
    for entry in entries:
        if entry.get("command") not in commands:
            existing.append(entry)
    data["hooks"][event] = existing

target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(json.dumps(data, indent=2) + "\n")
print(f"Updated {target}")
PY

echo "==> Installing launchd service (auto-start bridge on login)"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.subscreen.bridge</string>
  <key>ProgramArguments</key>
  <array>
    <string>$(command -v python3)</string>
    <string>$BRIDGE_DST/main.py</string>
    <string>--host</string>
    <string>0.0.0.0</string>
    <string>--port</string>
    <string>8765</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>StandardOutPath</key>
  <string>$HOME/.cursor/subscreen/bridge.log</string>
  <key>StandardErrorPath</key>
  <string>$HOME/.cursor/subscreen/bridge.err.log</string>
</dict>
</plist>
PLIST

launchctl bootout "gui/$(id -u)/com.subscreen.bridge" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
launchctl enable "gui/$(id -u)/com.subscreen.bridge"
launchctl kickstart -k "gui/$(id -u)/com.subscreen.bridge"

echo
echo "Done."
echo "Bridge: http://127.0.0.1:8765/status"
echo "LAN:    http://$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '<your-mac-ip>'):8765/status"
echo
echo "Next: edit firmware/platformio.ini with your WiFi credentials, then run:"
echo "  cd firmware && pio run -t upload"
