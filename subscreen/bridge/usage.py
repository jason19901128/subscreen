from __future__ import annotations

import json
import subprocess
import threading
import time
from pathlib import Path
from typing import Any

USAGE_API = "https://api2.cursor.sh/aiserver.v1.DashboardService/GetCurrentPeriodUsage"
POLL_INTERVAL_SEC = 300
REQUEST_TIMEOUT_SEC = 12

CURSOR_DB = Path.home() / "Library/Application Support/Cursor/User/globalStorage/state.vscdb"


def _read_access_token() -> str | None:
    if not CURSOR_DB.exists():
        return None
    db_uri = f"file:{CURSOR_DB}?mode=ro"
    try:
        proc = subprocess.run(
            [
                "sqlite3",
                db_uri,
                "SELECT value FROM ItemTable WHERE key='cursorAuth/accessToken';",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    token = (proc.stdout or "").strip()
    return token or None


def _cents_to_dollars(cents: int | float | None) -> float | None:
    if cents is None:
        return None
    return float(cents) / 100.0


def _pick_on_demand(payload: dict[str, Any]) -> dict[str, Any]:
    spend = payload.get("spendLimitUsage") or {}
    individual = payload.get("individualUsage") or {}
    on_demand = individual.get("onDemand") or {}
    team = (payload.get("teamUsage") or {}).get("onDemand") or {}

    remaining_cents = spend.get("individualRemaining")
    limit_cents = spend.get("individualLimit")
    used_cents = spend.get("individualUsed")
    limit_type = spend.get("limitType") or ""

    if remaining_cents is None and spend.get("pooledRemaining") is not None:
        remaining_cents = spend.get("pooledRemaining")
        limit_cents = spend.get("pooledLimit")
        used_cents = spend.get("pooledUsed")
        if not limit_type:
            limit_type = "team_pool"

    if remaining_cents is None and on_demand.get("remaining") is not None:
        remaining_cents = on_demand.get("remaining")
        limit_cents = on_demand.get("limit")
        used_cents = on_demand.get("used")

    if remaining_cents is None and team.get("remaining") is not None:
        remaining_cents = team.get("remaining")
        limit_cents = team.get("limit")
        used_cents = team.get("used")
        limit_type = limit_type or "team"

    enabled = bool(payload.get("enabled", True))
    if on_demand:
        enabled = enabled or bool(on_demand.get("enabled"))
    if spend:
        enabled = True

    remaining_usd = _cents_to_dollars(remaining_cents)
    limit_usd = _cents_to_dollars(limit_cents)
    used_usd = _cents_to_dollars(used_cents)

    unlimited = limit_cents is None and limit_usd is None
    if unlimited and remaining_cents is None and used_cents is not None:
        remaining_usd = None

    return {
        "enabled": enabled,
        "remaining_cents": remaining_cents,
        "limit_cents": limit_cents,
        "used_cents": used_cents,
        "remaining_usd": remaining_usd,
        "limit_usd": limit_usd,
        "used_usd": used_usd,
        "unlimited": unlimited and remaining_cents is None,
        "limit_type": limit_type,
    }


def _fetch_usage_payload(token: str) -> dict[str, Any] | None:
    try:
        proc = subprocess.run(
            [
                "curl",
                "-sS",
                "--connect-timeout",
                "5",
                "--max-time",
                str(REQUEST_TIMEOUT_SEC),
                "-H",
                f"Authorization: Bearer {token}",
                "-H",
                "Content-Type: application/json",
                "-H",
                "Connect-Protocol-Version: 1",
                "-d",
                "{}",
                USAGE_API,
            ],
            capture_output=True,
            text=True,
            timeout=REQUEST_TIMEOUT_SEC + 3,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    try:
        payload = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None
    return payload if isinstance(payload, dict) else None


def fetch_on_demand_usage() -> dict[str, Any]:
    now = int(time.time())
    base: dict[str, Any] = {
        "enabled": False,
        "remaining_cents": None,
        "limit_cents": None,
        "used_cents": None,
        "remaining_usd": None,
        "limit_usd": None,
        "used_usd": None,
        "unlimited": False,
        "limit_type": "",
        "updated_at": now,
        "error": "",
    }

    token = _read_access_token()
    if not token:
        base["error"] = "no_token"
        return base

    payload = _fetch_usage_payload(token)
    if payload is None:
        base["error"] = "fetch_failed"
        return base

    parsed = _pick_on_demand(payload)
    parsed["updated_at"] = now
    parsed["error"] = ""
    return parsed


class UsagePoller:
    def __init__(self, on_update) -> None:
        self._on_update = on_update
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="subscreen-usage", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def refresh_once(self) -> dict[str, Any]:
        data = fetch_on_demand_usage()
        self._on_update(data)
        return data

    def _run(self) -> None:
        self.refresh_once()
        while not self._stop.wait(POLL_INTERVAL_SEC):
            self.refresh_once()
