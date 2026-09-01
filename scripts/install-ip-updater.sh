#!/usr/bin/env bash
# Install a Desktop shortcut for updating Subscreen IP anytime (no Cursor needed).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/Update Subscreen IP.command"
DEST_DESKTOP="$HOME/Desktop/Update Subscreen IP.command"
DEST_APPS="$HOME/Applications/Update Subscreen IP.command"

chmod +x "$ROOT/scripts/update-bridge-host.sh" "$SRC"

mkdir -p "$HOME/Applications"
cp "$SRC" "$DEST_DESKTOP"
cp "$SRC" "$DEST_APPS"
chmod +x "$DEST_DESKTOP" "$DEST_APPS"

# Allow double-click without Gatekeeper prompt for local scripts when possible
xattr -d com.apple.quarantine "$DEST_DESKTOP" 2>/dev/null || true
xattr -d com.apple.quarantine "$DEST_APPS" 2>/dev/null || true

# Desktop shortcut must call the repo script with absolute paths
cat > "$DEST_DESKTOP" <<EOF
#!/bin/bash
# Subscreen IP Updater — Desktop launcher
echo "========================================"
echo "  Subscreen IP Updater"
echo "========================================"
echo

"$ROOT/scripts/update-bridge-host.sh" --upload
STATUS=\$?

echo
if [[ \$STATUS -eq 0 ]]; then
  echo "✓ Success"
else
  echo "✗ Failed (exit \$STATUS)"
fi
echo
read -r -p "Press Enter to close..."
exit \$STATUS
EOF
chmod +x "$DEST_DESKTOP"
cp "$DEST_DESKTOP" "$DEST_APPS"

echo "Installed:"
echo "  • Desktop: $DEST_DESKTOP"
echo "  • Applications: $DEST_APPS"
echo
echo "Usage: double-click「Update Subscreen IP」on Desktop."
echo "Or in Terminal:"
echo "  $ROOT/scripts/update-bridge-host.sh --upload"
