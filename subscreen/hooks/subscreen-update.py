#!/usr/bin/env python3
"""Cursor hook handler: forwards hook events to the subscreen bridge."""

from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request

BRIDGE_URL = "http://127.0.0.1:8765/update/hook"
TIMEOUT_SEC = 2.5


def main() -> int:
    raw = sys.stdin.read()
    if not raw.strip():
        return 0

    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        return 0

    if not isinstance(payload, dict):
        return 0

    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BRIDGE_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_SEC) as resp:
            resp.read()
    except (urllib.error.URLError, TimeoutError, OSError):
        pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
