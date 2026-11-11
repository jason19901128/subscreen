from __future__ import annotations

import base64
import hashlib
import json
import subprocess
import threading
import time
from datetime import date, datetime
from pathlib import Path
from typing import Any

USAGE_API = "https://api2.cursor.sh/aiserver.v1.DashboardService/GetCurrentPeriodUsage"
POLL_INTERVAL_SEC = 300
REQUEST_TIMEOUT_SEC = 12

CURSOR_DB = Path.home() / "Library/Application Support/Cursor/User/globalStorage/state.vscdb"
SAFE_STORAGE_PREFIX = "cursor-safe-storage:"
# Chromium/Electron OSCrypt on macOS (matches VS Code / Cursor Safe Storage).
_OSCRYPT_SALT = b"saltysalt"
_OSCRYPT_IV = b" " * 16
_OSCRYPT_ITERS = 1003
_OSCRYPT_KEY_LEN = 16


def _keychain_safe_storage_password() -> str | None:
    try:
        proc = subprocess.run(
            ["security", "find-generic-password", "-w", "-s", "Cursor Safe Storage"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    password = (proc.stdout or "").rstrip("\n")
    return password or None


def _pkcs7_unpad(data: bytes) -> bytes:
    if not data:
        raise ValueError("empty")
    pad = data[-1]
    if pad < 1 or pad > 16 or data[-pad:] != bytes([pad]) * pad:
        raise ValueError("bad padding")
    return data[:-pad]


def _aes128_cbc_decrypt(key: bytes, iv: bytes, ciphertext: bytes) -> bytes:
    try:
        from Crypto.Cipher import AES  # type: ignore
    except ImportError:
        AES = None  # type: ignore

    if AES is not None:
        return _pkcs7_unpad(AES.new(key, AES.MODE_CBC, iv).decrypt(ciphertext))

    # Fallback: OpenSSL CLI (always available on macOS).
    try:
        proc = subprocess.run(
            [
                "openssl",
                "enc",
                "-d",
                "-aes-128-cbc",
                "-K",
                key.hex(),
                "-iv",
                iv.hex(),
            ],
            input=ciphertext,
            capture_output=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise ValueError("openssl decrypt unavailable") from None
    if proc.returncode != 0:
        raise ValueError("openssl decrypt failed")
    return proc.stdout


def _decrypt_safe_storage_token(blob: str) -> str | None:
    """Decrypt Cursor's `cursor-safe-storage:v1:<base64(v10...)>` token wrapper."""
    if not blob.startswith(SAFE_STORAGE_PREFIX):
        return blob
    parts = blob.split(":", 2)
    if len(parts) != 3 or parts[1] != "v1":
        return None
    try:
        raw = base64.b64decode(parts[2])
    except Exception:
        return None
    if len(raw) < 19 or not raw.startswith(b"v10"):
        return None
    password = _keychain_safe_storage_password()
    if not password:
        return None
    key = hashlib.pbkdf2_hmac(
        "sha1",
        password.encode("utf-8"),
        _OSCRYPT_SALT,
        _OSCRYPT_ITERS,
        dklen=_OSCRYPT_KEY_LEN,
    )
    try:
        plain = _aes128_cbc_decrypt(key, _OSCRYPT_IV, raw[3:])
        token = plain.decode("utf-8").strip()
    except Exception:
        return None
    return token or None


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
    if not token:
        return None
    if token.startswith(SAFE_STORAGE_PREFIX):
        return _decrypt_safe_storage_token(token)
    return token


def _cents_to_dollars(cents: int | float | None) -> float | None:
    if cents is None:
        return None
    return float(cents) / 100.0


def _ms_timestamp_to_date(value: Any) -> date | None:
    if value is None:
        return None
    try:
        ms = int(value)
    except (TypeError, ValueError):
        return None
    # API returns epoch milliseconds as string/number.
    if ms > 10_000_000_000:
        seconds = ms / 1000.0
    else:
        seconds = float(ms)
    try:
        return datetime.fromtimestamp(seconds).date()
    except (OverflowError, OSError, ValueError):
        return None


def _parse_billing_cycle(payload: dict[str, Any]) -> dict[str, Any]:
    """Usage-limits reset progress from billingCycleStart → billingCycleEnd."""
    start_day = _ms_timestamp_to_date(payload.get("billingCycleStart"))
    end_day = _ms_timestamp_to_date(payload.get("billingCycleEnd"))
    empty = {
        "billing_cycle_start": None,
        "billing_cycle_end": None,
        "cycle_days": None,
        "cycle_elapsed_days": None,
        "cycle_remaining_days": None,
        "cycle_percent_used": None,
    }
    if start_day is None or end_day is None:
        return empty

    cycle_days = (end_day - start_day).days
    if cycle_days <= 0:
        return empty

    today = date.today()
    elapsed = (today - start_day).days
    remaining = (end_day - today).days
    if elapsed < 0:
        elapsed = 0
    if elapsed > cycle_days:
        elapsed = cycle_days
    if remaining < 0:
        remaining = 0
    if remaining > cycle_days:
        remaining = cycle_days

    # Bar current = cycle_days - remaining (= elapsed calendar days).
    percent = min(100.0, elapsed * 100.0 / cycle_days)

    return {
        "billing_cycle_start": start_day.isoformat(),
        "billing_cycle_end": end_day.isoformat(),
        "cycle_days": cycle_days,
        "cycle_elapsed_days": elapsed,
        "cycle_remaining_days": remaining,
        "cycle_percent_used": percent,
    }


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

    def _float_or_none(value: Any) -> float | None:
        if value is None:
            return None
        try:
            return float(value)
        except (TypeError, ValueError):
            return None

    # Matches Cursor Settings → Included in Team:
    #   Cursor Models → autoPercentUsed
    #   Other Models  → apiPercentUsed
    auto_percent = _float_or_none(plan.get("autoPercentUsed"))
    api_percent = _float_or_none(plan.get("apiPercentUsed"))
    total_percent = _float_or_none(plan.get("totalPercentUsed"))

    return {
        "plan_limit_cents": limit_cents,
        "plan_included_spend_cents": included_spend,
        "plan_total_spend_cents": total_spend,
        "plan_bonus_spend_cents": bonus_spend,
        "plan_remaining_cents": remaining_cents,
        "plan_percent_used": included_percent,
        "plan_total_percent_used": total_percent,
        "auto_percent_used": auto_percent,
        "api_percent_used": api_percent,
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
    cycle = _parse_billing_cycle(payload)

    enabled = bool(payload.get("enabled", True))
    if parsed.get("plan_limit_cents"):
        enabled = True
    if od.get("limit_cents"):
        enabled = enabled or True

    result = {
        "enabled": enabled,
        **parsed,
        **od,
        **cycle,
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
        "auto_percent_used": None,
        "api_percent_used": None,
        "billing_cycle_start": None,
        "billing_cycle_end": None,
        "cycle_days": None,
        "cycle_elapsed_days": None,
        "cycle_remaining_days": None,
        "cycle_percent_used": None,
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

    # Cursor returns ConnectRPC error JSON (e.g. unauthenticated) with HTTP 200.
    api_code = str(payload.get("code") or "")
    if api_code and "planUsage" not in payload and "spendLimitUsage" not in payload:
        if api_code in ("unauthenticated", "ERROR_NOT_LOGGED_IN"):
            base["error"] = "auth_failed"
        else:
            base["error"] = f"api_{api_code}"
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
