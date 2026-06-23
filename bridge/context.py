from __future__ import annotations

import json
import subprocess
import threading
import time
from pathlib import Path
from typing import Any

DEFAULT_CONTEXT_WINDOW = 200_000
POLL_INTERVAL_SEC = 1.0

CURSOR_DB = Path.home() / "Library/Application Support/Cursor/User/globalStorage/state.vscdb"


def _read_composer_headers() -> list[dict[str, Any]]:
    if not CURSOR_DB.exists():
        return []
    db_uri = f"file:{CURSOR_DB}?mode=ro"
    try:
        proc = subprocess.run(
            [
                "sqlite3",
                db_uri,
                "SELECT value FROM ItemTable WHERE key='composer.composerHeaders';",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if proc.returncode != 0 or not (proc.stdout or "").strip():
        return []
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return []
    composers = data.get("allComposers")
    return composers if isinstance(composers, list) else []


def _workspace_name(composer: dict[str, Any]) -> str:
    wid = composer.get("workspaceIdentifier") or {}
    config = wid.get("configPath") or {}
    fs_path = config.get("fsPath") or config.get("path") or ""
    if fs_path:
        return Path(str(fs_path)).stem.replace(".code-workspace", "")
    return ""


def _pick_composer(
    composers: list[dict[str, Any]],
    conversation_id: str | None,
    project: str | None,
) -> dict[str, Any] | None:
    if conversation_id:
        for composer in composers:
            if str(composer.get("composerId") or "") == conversation_id:
                return composer

    if project:
        matches = [c for c in composers if _workspace_name(c) == project]
        if matches:
            return max(matches, key=lambda c: int(c.get("lastUpdatedAt") or 0))

    if composers:
        return max(composers, key=lambda c: int(c.get("lastUpdatedAt") or 0))
    return None


def _tokens_from_composer(
    composer: dict[str, Any],
    window_size: int = DEFAULT_CONTEXT_WINDOW,
) -> dict[str, Any]:
    window = window_size
    raw_window = composer.get("contextWindowSize") or composer.get("context_window_size")
    if raw_window is not None:
        try:
            window = int(raw_window)
        except (TypeError, ValueError):
            window = window_size

    raw_tokens = composer.get("contextTokens") or composer.get("context_tokens")
    if raw_tokens is not None:
        try:
            tokens = int(raw_tokens)
            pct = (tokens * 100.0 / window) if window > 0 else 0.0
            return {
                "context_tokens": tokens,
                "context_usage_percent": round(pct, 3),
                "context_window_size": window,
            }
        except (TypeError, ValueError):
            pass

    pct = composer.get("contextUsagePercent")
    if pct is None:
        return {}
    try:
        usage_pct = float(pct)
    except (TypeError, ValueError):
        return {}

    tokens = int(round(usage_pct * window / 100.0))
    return {
        "context_tokens": tokens,
        "context_usage_percent": usage_pct,
        "context_window_size": window,
    }


def fetch_composer_flags(
    conversation_id: str | None,
    project: str | None,
) -> dict[str, Any]:
    composers = _read_composer_headers()
    composer = _pick_composer(composers, conversation_id, project)
    if not composer:
        return {"has_blocking_pending_actions": False}
    return {
        "has_blocking_pending_actions": bool(composer.get("hasBlockingPendingActions")),
    }


def fetch_context_metrics(
    conversation_id: str | None,
    project: str | None,
) -> dict[str, Any]:
    composers = _read_composer_headers()
    composer = _pick_composer(composers, conversation_id, project)
    if not composer:
        return {"context_error": "no_composer"}

    metrics = _tokens_from_composer(composer)
    if not metrics:
        return {"context_error": "no_usage"}
    metrics["context_error"] = ""
    metrics["context_updated_at"] = int(time.time())
    return metrics


class ContextPoller:
    def __init__(self, snapshot_fn, on_context_update, on_composer_update=None) -> None:
        self._snapshot_fn = snapshot_fn
        self._on_context_update = on_context_update
        self._on_composer_update = on_composer_update
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="subscreen-context", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def refresh_once(self) -> dict[str, Any]:
        snap = self._snapshot_fn()
        conversation_id = snap.get("conversation_id") or None
        project = snap.get("project") or None
        data = fetch_context_metrics(conversation_id, project)
        self._on_context_update(data)
        if self._on_composer_update:
            flags = fetch_composer_flags(conversation_id, project)
            self._on_composer_update(flags)
        return data

    def _run(self) -> None:
        self.refresh_once()
        while not self._stop.wait(POLL_INTERVAL_SEC):
            self.refresh_once()
