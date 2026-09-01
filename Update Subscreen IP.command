#!/bin/bash
# Double-click this file in Finder to update Subscreen IP and flash firmware.
cd "$(dirname "$0")" || exit 1

echo "========================================"
echo "  Subscreen IP Updater"
echo "========================================"
echo

./scripts/update-bridge-host.sh --upload
STATUS=$?

echo
if [[ $STATUS -eq 0 ]]; then
  echo "✓ Success"
else
  echo "✗ Failed (exit $STATUS)"
fi
echo
read -r -p "Press Enter to close..."
exit $STATUS
