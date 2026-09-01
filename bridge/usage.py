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


def _parse_plan_usage(plan: dict[str, Any]) -> dict[str, Any]:
    limit_cents = plan.get("limit")
    included_spend = plan.get("includedSpend")
    total_spend = plan.get("totalSpend")
    bonus_spend = plan.get("bonusSpend")
    remaining = plan.get("remaining")

    try:
        limit_cents = int(limit_cents) if limit_cents is not None else None
    except (TypeError, ValueError):
        limit_cents = None
    try:
        included_spend = int(included_spend) if included_spend is not None else None
    except (TypeError, ValueError):
        included_spend = None
    try:
        total_spend = int(total_spend) if total_spend is not None else None
    except (TypeError, ValueError):
        total_spend = None
    try:
        bonus_spend = int(bonus_spend) if bonus_spend is not None else None
    except (TypeError, ValueError):
        bonus_spend = None

    remaining_cents: int | None
    if remaining is not None:
        try:
            remaining_cents = int(remaining)
        except (TypeError, ValueError):
            remaining_cents = None
    elif limit_cents is not None and included_spend is not None:
        remaining_cents = max(0, limit_cents - included_spend)
    else:
        remaining_cents = None

    included_percent: float | None = None
    if limit_cents and limit_cents > 0 and included_spend is not None:
        included_percent = min(100.0, included_spend * 100.0 / limit_cents)

    total_percent: float | None = None
    for key in ("totalPercentUsed", "autoPercentUsed", "apiPercentUsed"):
        raw = plan.get(key)
        if raw is not None:
            try:
                total_percent = float(raw)
                break
            except (TypeError, ValueError):
                pass

    return {
        "plan_limit_cents": limit_cents,
        "plan_included_spend_cents": included_spend,
        "plan_total_spend_cents": total_spend,
        "plan_bonus_spend_cents": bonus_spend,
        "plan_remaining_cents": remaining_cents,
        "plan_percent_used": included_percent,
        "plan_total_percent_used": total_percent,
    }


def _parse_spend_limit(spend: dict[str, Any]) -> dict[str, Any]:
    limit_type = str(spend.get("limitType") or "")

    pooled_limit = spend.get("pooledLimit")
    pooled_used = spend.get("pooledUsed")
    pooled_remaining = spend.get("pooledRemaining")
    individual_limit = spend.get("individualLimit")
    individual_used = spend.get("individualUsed")
    individual_remaining = spend.get("individualRemaining")
    total_spend = spend.get("totalSpend")

    def _int_or_none(value: Any) -> int | None:
        if value is None:
            return None
        try:
            return int(value)
        except (TypeError, ValueError):
            return None

    pooled_limit = _int_or_none(pooled_limit)
    pooled_used = _int_or_none(pooled_used)
    pooled_remaining = _int_or_none(pooled_remaining)
    individual_limit = _int_or_none(individual_limit)
    individual_used = _int_or_none(individual_used)
    individual_remaining = _int_or_none(individual_remaining)
    total_spend = _int_or_none(total_spend)

    # Prefer team pool when present; fall back to individual cap.
    if pooled_limit is not None and pooled_limit > 0:
        used_cents = pooled_used
        limit_cents = pooled_limit
        remaining_cents = pooled_remaining
        if used_cents is None and limit_cents is not None and remaining_cents is not None:
            used_cents = max(0, limit_cents - remaining_cents)
        if not limit_type:
            limit_type = "team_pool"
    elif individual_limit is not None and individual_limit > 0:
        used_cents = individual_used
        limit_cents = individual_limit
        remaining_cents = individual_remaining
        if used_cents is None and limit_cents is not None and remaining_cents is not None:
            used_cents = max(0, limit_cents - remaining_cents)
        if not limit_type:
            limit_type = "individual"
    else:
        used_cents = total_spend
        limit_cents = None
        remaining_cents = None

    unlimited = limit_cents is None or limit_cents <= 0

    return {
        "enabled": True,
        "remaining_cents": remaining_cents,
        "limit_cents": limit_cents,
        "used_cents": used_cents,
        "remaining_usd": _cents_to_dollars(remaining_cents),
        "limit_usd": _cents_to_dollars(limit_cents),
        "used_usd": _cents_to_dollars(used_cents),
        "unlimited": unlimited,
        "limit_type": limit_type,
    }


def _pick_usage(payload: dict[str, Any]) -> dict[str, Any]:
    plan = payload.get("planUsage") or {}
    spend = payload.get("spendLimitUsage") or {}

    parsed = _parse_plan_usage(plan if isinstance(plan, dict) else {})
    od = _parse_spend_limit(spend if isinstance(spend, dict) else {})

    enabled = bool(payload.get("enabled", True))
    if parsed.get("plan_limit_cents"):
        enabled = True
    if od.get("limit_cents"):
        enabled = enabled or True

    result = {
        "enabled": enabled,
        **parsed,
        **od,
        "display_message": str(payload.get("displayMessage") or ""),
        "auto_display_message": str(payload.get("autoModelSelectedDisplayMessage") or ""),
    }
    return result


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
        "plan_limit_cents": None,
        "plan_included_spend_cents": None,
        "plan_total_spend_cents": None,
        "plan_bonus_spend_cents": None,
        "plan_remaining_cents": None,
        "plan_percent_used": None,
        "plan_total_percent_used": None,
        "display_message": "",
        "auto_display_message": "",
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

    parsed = _pick_usage(payload)
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
