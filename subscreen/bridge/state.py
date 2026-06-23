from __future__ import annotations

import json
import threading
import time
from copy import deepcopy
from pathlib import Path
from typing import Any

STATE_FILE = Path.home() / ".cursor" / "subscreen" / "state.json"
HOOK_DEBUG_LOG = Path.home() / ".cursor" / "subscreen" / "hook-debug.jsonl"
REVIEW_FALLBACK_DETAIL = "Review: Keep / Undo in Cursor"
STALE_REVIEW_SEC = 90
STALE_AGENT_SEC = 300
STALE_TURN_SEC = 45
TERMINAL_IDLE_DETAILS = frozenset(
    {
        "Task aborted",
        "Task completed",
        "Session ended",
        "Waiting for Cursor",
        "Ready",
    }
)

def _normalize_model(model: str) -> str:
    m = model.strip()
    if not m:
        return ""
    if m.lower() == "default":
        return "Auto"
    return m


DEFAULT_CONTEXT_WINDOW = 200_000


def _apply_context_from_payload(metrics: dict[str, Any], payload: dict[str, Any]) -> None:
    tokens = payload.get("context_tokens")
    pct = payload.get("context_usage_percent")
    window = payload.get("context_window_size")

    if window is not None:
        try:
            metrics["context_window_size"] = int(window)
        except (TypeError, ValueError):
            pass

    if tokens is not None:
        try:
            metrics["context_tokens"] = int(tokens)
        except (TypeError, ValueError):
            pass

    if pct is not None:
        try:
            metrics["context_usage_percent"] = float(pct)
        except (TypeError, ValueError):
            pass

    if metrics.get("context_tokens") is None and metrics.get("context_usage_percent") is not None:
        win = int(metrics.get("context_window_size") or DEFAULT_CONTEXT_WINDOW)
        metrics["context_tokens"] = int(
            round(float(metrics["context_usage_percent"]) * win / 100.0)
        )


DEFAULT_STATE: dict[str, Any] = {
    "agent_status": "idle",
    "agent_detail": "Waiting for Cursor",
    "model": "",
    "project": "",
    "conversation_id": "",
    "composer_mode": "",
    "pending_confirm": False,
    "confirm_since": 0,
    "pending_review_files": [],
    "review_session_active": False,
    "composer_blocking_pending": False,
    "agent_turn_active": False,
    "session_metrics": {
        "prompt_count": 0,
        "tool_count": 0,
        "response_chars": 0,
        "estimated_tokens": 0,
        "context_tokens": 0,
        "context_usage_percent": 0.0,
        "context_window_size": DEFAULT_CONTEXT_WINDOW,
        "context_error": "",
        "context_updated_at": 0,
    },
    "bridge_online": True,
    "cursor_online": False,
    "updated_at": 0,
    "on_demand_usage": {
        "enabled": False,
        "remaining_cents": None,
        "limit_cents": None,
        "used_cents": None,
        "remaining_usd": None,
        "limit_usd": None,
        "used_usd": None,
        "unlimited": False,
        "limit_type": "",
        "updated_at": 0,
        "error": "not_fetched",
    },
}


class StateStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._state = deepcopy(DEFAULT_STATE)
        self._state["updated_at"] = int(time.time())
        self._persist_dirty = False
        self._persist_timer: threading.Timer | None = None
        self._composer_raw_blocking = False
        self._composer_stable_reads = 0
        self._load()
        self._sanitize_after_load()

    def _load(self) -> None:
        if not STATE_FILE.exists():
            return
        try:
            data = json.loads(STATE_FILE.read_text(encoding="utf-8"))
            with self._lock:
                self._state.update(data)
        except (OSError, json.JSONDecodeError):
            pass

    def _sanitize_after_load(self) -> None:
        with self._lock:
            _clear_review_state(self._state)
            self._state["agent_turn_active"] = False
            self._state["composer_blocking_pending"] = False
            if _state_age_sec(self._state) > STALE_AGENT_SEC or self._state.get(
                "pending_confirm"
            ):
                _clear_confirm(self._state)
            self._state["agent_status"] = "idle"
            self._state["agent_detail"] = "Waiting for Cursor"
            _prune_stale_review(self._state, int(time.time()))
        self._persist_flush_force()

    def startup_sync_composer(self) -> None:
        try:
            from context import fetch_composer_flags
        except ImportError:
            return
        with self._lock:
            conversation_id = self._state.get("conversation_id") or None
            project = self._state.get("project") or None
        flags = fetch_composer_flags(conversation_id, project)
        self.update_from_composer(flags)
        if not flags.get("has_blocking_pending_actions"):
            with self._lock:
                if _has_review_artifacts(self._state) or self._state.get("pending_confirm"):
                    _clear_review_state(self._state)
                    self._state["agent_turn_active"] = False
                    self._state["agent_status"] = "idle"
                    self._state["agent_detail"] = "Waiting for Cursor"
            self._persist_flush_force()

    def _persist_flush_force(self) -> None:
        with self._lock:
            snapshot = deepcopy(self._state)
            self._persist_dirty = False
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(
            json.dumps(snapshot, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

    def _persist_flush(self) -> None:
        with self._lock:
            if not self._persist_dirty:
                return
            snapshot = deepcopy(self._state)
            self._persist_dirty = False
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(
            json.dumps(snapshot, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

    def _schedule_persist(self) -> None:
        with self._lock:
            self._persist_dirty = True
            if self._persist_timer and self._persist_timer.is_alive():
                return
            self._persist_timer = threading.Timer(0.2, self._persist_flush)
            self._persist_timer.daemon = True
            self._persist_timer.start()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            state = deepcopy(self._state)
        now = int(time.time())
        state["bridge_online"] = True
        state["cursor_online"] = (now - state.get("updated_at", 0)) < STALE_AGENT_SEC
        _elevate_snapshot_from_composer(state)
        _prune_stale_review(state, now)
        _expire_stale_confirm(state, now)
        _expire_stale_turn(state, now)
        _normalize_agent_status(state)
        if state.get("model"):
            state["model"] = _normalize_model(str(state["model"]))
        return state

    def update_from_composer(self, flags: dict[str, Any]) -> None:
        if not flags:
            return
        raw_blocking = bool(flags.get("has_blocking_pending_actions"))
        with self._lock:
            if raw_blocking == self._composer_raw_blocking:
                self._composer_stable_reads += 1
            else:
                self._composer_raw_blocking = raw_blocking
                self._composer_stable_reads = 1
            if self._composer_stable_reads < 2:
                return
            blocking = raw_blocking
            prev = bool(self._state.get("composer_blocking_pending"))
            if blocking == prev:
                return
            self._state["updated_at"] = int(time.time())
            self._state["composer_blocking_pending"] = blocking
            if blocking:
                _mark_review_session(self._state)
                _apply_review_confirm(self._state)
            else:
                _dismiss_review_when_unblocked(self._state)
                _apply_turn_busy_guard(self._state)
        self._schedule_persist()

    def set_on_demand_usage(self, usage: dict[str, Any]) -> None:
        with self._lock:
            self._state["on_demand_usage"] = usage
        self._schedule_persist()

    def set_context_metrics(self, ctx: dict[str, Any]) -> None:
        if not ctx:
            return
        with self._lock:
            metrics = self._state.setdefault("session_metrics", {})
            metrics.update(ctx)
        self._schedule_persist()

    def apply_hook(self, payload: dict[str, Any]) -> None:
        event = payload.get("hook_event_name", "")
        now = int(time.time())
        _log_hook_event(event, payload)

        with self._lock:
            self._state["updated_at"] = now
            self._state["cursor_online"] = True

            if payload.get("model"):
                self._state["model"] = _normalize_model(str(payload["model"]))

            roots = payload.get("workspace_roots") or []
            if roots:
                self._state["project"] = Path(str(roots[0])).name

            if payload.get("cwd"):
                self._state["project"] = Path(str(payload["cwd"])).name

            if payload.get("conversation_id"):
                self._state["conversation_id"] = str(payload["conversation_id"])

            metrics = self._state["session_metrics"]
            _apply_context_from_payload(metrics, payload)

            if event == "sessionStart":
                _clear_review_state(self._state)
                self._state["agent_turn_active"] = True
                self._state["agent_status"] = "thinking"
                self._state["agent_detail"] = "Session started"
                self._state["composer_mode"] = str(payload.get("composer_mode") or "")
                metrics.update(
                    {
                        "prompt_count": 0,
                        "tool_count": 0,
                        "response_chars": 0,
                        "estimated_tokens": 0,
                        "context_tokens": 0,
                        "context_usage_percent": 0.0,
                        "context_window_size": DEFAULT_CONTEXT_WINDOW,
                        "context_error": "",
                        "context_updated_at": 0,
                    }
                )

            elif event == "preCompact":
                _apply_context_from_payload(metrics, payload)
                self._state["agent_detail"] = "Compacting context..."

            elif event == "sessionEnd":
                _clear_review_state(self._state)
                self._state["agent_turn_active"] = False
                self._state["agent_status"] = "idle"
                self._state["agent_detail"] = "Session ended"

            elif event == "beforeSubmitPrompt":
                _clear_review_state(self._state)
                self._state["agent_turn_active"] = True
                self._state["agent_status"] = "thinking"
                prompt = str(payload.get("prompt") or "")
                preview = prompt.replace("\n", " ")[:48]
                self._state["agent_detail"] = preview or "Processing prompt"
                metrics["prompt_count"] = int(metrics.get("prompt_count", 0)) + 1
                metrics["estimated_tokens"] = int(metrics.get("estimated_tokens", 0)) + max(
                    1, len(prompt) // 4
                )

            elif event == "afterAgentThought":
                if _reconcile_review_status(self._state):
                    pass
                elif not self._state.get("pending_confirm"):
                    _clear_confirm(self._state)
                    _apply_working_or_idle(self._state, detail_working="Thinking...")

            elif event == "preToolUse":
                tool_name = str(payload.get("tool_name") or "Tool")
                tool_input = _coerce_tool_input(payload.get("tool_input"))
                _dismiss_review_when_unblocked(self._state)
                if _is_hook_action_confirm_detail(self._state):
                    _sync_confirm_flags(self._state)
                elif _tool_needs_confirm(tool_name):
                    _set_confirm(self._state, _format_confirm_detail(tool_name, tool_input))
                else:
                    if not self._state.get("pending_confirm"):
                        _clear_confirm(self._state)
                        detail = _format_tool_detail(tool_name, tool_input)
                        self._state["agent_status"] = "running_tool"
                        self._state["agent_detail"] = detail
                metrics["tool_count"] = int(metrics.get("tool_count", 0)) + 1

            elif event == "postToolUse":
                tool_name = str(payload.get("tool_name") or "")
                tool_input = _coerce_tool_input(payload.get("tool_input"))
                if _is_file_modify_tool(tool_name):
                    _mark_review_session(self._state)
                    _track_review_from_tool(self._state, tool_input)
                if _reconcile_review_status(self._state):
                    pass
                elif self._state.get("pending_confirm") and _tool_needs_confirm(tool_name):
                    if not _is_file_modify_tool(tool_name):
                        _clear_confirm(self._state)
                        _apply_working_or_idle(self._state)
                else:
                    _apply_working_or_idle(self._state, detail_idle="Tool finished")

            elif event == "afterAgentResponse":
                text = str(payload.get("text") or "")
                metrics["response_chars"] = int(metrics.get("response_chars", 0)) + len(text)
                metrics["estimated_tokens"] = int(metrics.get("estimated_tokens", 0)) + max(
                    1, len(text) // 4
                )
                if _reconcile_review_status(self._state):
                    pass
                else:
                    _apply_working_or_idle(self._state)

            elif event == "stop":
                status = str(payload.get("status") or "completed")
                if status == "completed":
                    if _user_confirm_pending(self._state):
                        _sync_confirm_flags(self._state)
                    elif _composer_blocking_live(self._state):
                        self._state["composer_blocking_pending"] = True
                        _apply_review_confirm(self._state)
                    elif _reconcile_review_status(self._state):
                        pass
                    else:
                        self._state["agent_turn_active"] = False
                        _clear_confirm(self._state)
                        self._state["agent_status"] = "idle"
                        self._state["agent_detail"] = "Task completed"
                else:
                    _clear_review_state(self._state)
                    self._state["agent_turn_active"] = False
                    _clear_confirm(self._state)
                    if status == "aborted":
                        self._state["agent_status"] = "idle"
                        self._state["agent_detail"] = "Task aborted"
                    else:
                        self._state["agent_status"] = "error"
                        self._state["agent_detail"] = "Task error"

            elif event == "afterFileEdit":
                file_path = str(payload.get("file_path") or "")
                _mark_review_session(self._state)
                _track_review_file(self._state, file_path)
                _reconcile_review_status(self._state)

            elif event == "postToolUseFailure":
                _clear_confirm(self._state)
                self._state["agent_status"] = "error"
                self._state["agent_detail"] = str(payload.get("error_message") or "Tool failed")

            elif event == "beforeShellExecution":
                command = str(payload.get("command") or "")
                preview = command.replace("\n", " ")[:44]
                _set_confirm(self._state, f"Confirm Shell: {preview or 'command'}")

            elif event == "afterShellExecution":
                _clear_confirm(self._state)
                self._state["composer_blocking_pending"] = False
                _apply_working_or_idle(self._state, detail_idle="Shell finished")

            elif event == "beforeMCPExecution":
                tool_name = str(payload.get("tool_name") or payload.get("mcp_tool") or "MCP")
                _set_confirm(self._state, f"Confirm MCP: {tool_name[:40]}")

            elif event == "afterMCPExecution":
                _clear_confirm(self._state)
                self._state["composer_blocking_pending"] = False
                _apply_working_or_idle(self._state, detail_idle="MCP finished")

            elif event == "subagentStart":
                subagent_type = str(payload.get("subagent_type") or "subagent")
                _set_confirm(self._state, f"Confirm subagent: {subagent_type[:32]}")

            elif event == "subagentStop":
                _clear_confirm(self._state)
                _apply_working_or_idle(self._state, detail_idle="Subagent finished")

        self._schedule_persist()

    def touch(self) -> None:
        with self._lock:
            self._state["updated_at"] = int(time.time())
            self._state["cursor_online"] = True
        self._schedule_persist()


FILE_MODIFY_TOOLS = frozenset(
    {
        "Write",
        "StrReplace",
        "Delete",
        "EditNotebook",
        "ApplyPatch",
        "Apply_patch",
        "SearchReplace",
        "MultiEdit",
        "NotebookEdit",
    }
)


def _log_hook_event(event: str, payload: dict[str, Any]) -> None:
    if not event:
        return
    try:
        HOOK_DEBUG_LOG.parent.mkdir(parents=True, exist_ok=True)
        entry = {
            "ts": int(time.time()),
            "event": event,
            "tool_name": payload.get("tool_name"),
            "file_path": payload.get("file_path"),
        }
        with HOOK_DEBUG_LOG.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass


def _coerce_tool_input(raw: Any) -> dict[str, Any]:
    if isinstance(raw, dict):
        return raw
    if isinstance(raw, str) and raw.strip():
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, dict):
                return parsed
        except json.JSONDecodeError:
            pass
    return {}


def _is_file_modify_tool(tool_name: str) -> bool:
    name = tool_name.strip()
    if not name:
        return False
    if name in FILE_MODIFY_TOOLS:
        return True
    lower = name.lower()
    if lower in {t.lower() for t in FILE_MODIFY_TOOLS}:
        return True
    return any(token in lower for token in ("write", "replace", "delete", "patch", "edit"))


def _state_age_sec(state: dict[str, Any], now: int | None = None) -> int:
    updated = int(state.get("updated_at") or 0)
    if updated <= 0:
        return 999999
    return int(now or time.time()) - updated


def _has_pending_review(state: dict[str, Any]) -> bool:
    return bool(state.get("pending_review_files"))


def _review_is_active(state: dict[str, Any], now: int | None = None) -> bool:
    return bool(state.get("composer_blocking_pending"))


def _sanitize_loaded_state(state: dict[str, Any]) -> None:
    now = int(time.time())
    _prune_stale_review(state, now)
    if _state_age_sec(state, now) > STALE_AGENT_SEC:
        state["agent_turn_active"] = False
        if not state.get("pending_confirm"):
            state["agent_status"] = "idle"
            state["agent_detail"] = "Waiting for Cursor"


def _has_review_artifacts(state: dict[str, Any]) -> bool:
    detail = str(state.get("agent_detail") or "")
    return bool(
        state.get("review_session_active")
        or state.get("composer_blocking_pending")
        or _has_pending_review(state)
        or detail.startswith("Review:")
    )


def _is_review_confirm_detail(state: dict[str, Any]) -> bool:
    detail = str(state.get("agent_detail") or "")
    return detail.startswith("Review:") or detail.startswith("Confirm: Keep")


def _detail_implies_confirm(state: dict[str, Any]) -> bool:
    detail = str(state.get("agent_detail") or "")
    if not detail:
        return False
    if detail.startswith("Review:"):
        return bool(state.get("composer_blocking_pending"))
    return detail.startswith(
        ("Confirm Shell:", "Confirm MCP:", "Confirm subagent:", "Confirm:", "Confirm ")
    )


def _user_confirm_pending(state: dict[str, Any]) -> bool:
    return bool(
        state.get("pending_confirm")
        or state.get("composer_blocking_pending")
        or _detail_implies_confirm(state)
    )


def _sync_confirm_flags(state: dict[str, Any]) -> None:
    if not _user_confirm_pending(state):
        return
    state["pending_confirm"] = True
    state["agent_status"] = "awaiting_confirm"


def _composer_blocking_live(state: dict[str, Any]) -> bool:
    try:
        from context import fetch_composer_flags
    except ImportError:
        return False
    flags = fetch_composer_flags(
        state.get("conversation_id") or None,
        state.get("project") or None,
    )
    return bool(flags.get("has_blocking_pending_actions"))


def _elevate_snapshot_from_composer(state: dict[str, Any]) -> None:
    if _composer_blocking_live(state):
        state["composer_blocking_pending"] = True
        _apply_review_confirm(state)


def _dismiss_review_when_unblocked(state: dict[str, Any]) -> bool:
    if state.get("composer_blocking_pending"):
        return False
    if not (_has_review_artifacts(state) or _is_review_confirm_detail(state)):
        return False
    _clear_review_state(state)
    _apply_working_or_idle(state, detail_working="Working...")
    return True


def _prune_stale_review(state: dict[str, Any], now: int) -> None:
    if state.get("composer_blocking_pending") or _user_confirm_pending(state):
        return
    if not _has_review_artifacts(state):
        return
    if _state_age_sec(state, now) <= STALE_REVIEW_SEC and _review_is_active(state, now):
        return
    _clear_review_state(state)
    if _user_confirm_pending(state):
        return
    if not state.get("agent_turn_active"):
        state["agent_status"] = "idle"
        state["agent_detail"] = "Waiting for Cursor"


def _mark_review_session(state: dict[str, Any]) -> None:
    state["review_session_active"] = True


def _set_confirm(state: dict[str, Any], detail: str) -> None:
    state["pending_confirm"] = True
    state["agent_status"] = "awaiting_confirm"
    state["agent_detail"] = detail
    state["confirm_since"] = int(time.time())


def _clear_confirm(state: dict[str, Any]) -> None:
    state["pending_confirm"] = False
    state["confirm_since"] = 0
    if state.get("agent_status") == "awaiting_confirm":
        state["agent_status"] = "running"
        detail = str(state.get("agent_detail") or "")
        if detail.startswith("Confirm"):
            state["agent_detail"] = "Continuing"


def _clear_review_state(state: dict[str, Any]) -> None:
    state["pending_review_files"] = []
    state["review_session_active"] = False
    state["composer_blocking_pending"] = False
    if _is_review_confirm_detail(state):
        _clear_confirm(state)


def _is_terminal_idle(state: dict[str, Any]) -> bool:
    if state.get("agent_status") not in {"idle", "error"}:
        return False
    detail = str(state.get("agent_detail") or "")
    return detail in TERMINAL_IDLE_DETAILS or detail.startswith("Task ")


def _expire_stale_turn(state: dict[str, Any], now: int) -> None:
    if not state.get("agent_turn_active"):
        return
    if _user_confirm_pending(state):
        return
    busy = state.get("agent_status") in {"thinking", "running_tool", "running"}
    if not busy:
        return
    if _state_age_sec(state, now) <= STALE_TURN_SEC:
        return
    state["agent_turn_active"] = False
    state["agent_status"] = "idle"
    state["agent_detail"] = "Waiting for Cursor"


def _apply_turn_busy_guard(state: dict[str, Any]) -> None:
    if _is_terminal_idle(state):
        state["agent_turn_active"] = False
        return
    if not state.get("agent_turn_active"):
        return
    if state.get("pending_confirm") or _review_is_active(state):
        return
    if state.get("agent_status") in {"idle", "running"}:
        state["agent_status"] = "thinking"
        if not str(state.get("agent_detail") or "").strip():
            state["agent_detail"] = "Working..."


def _apply_working_or_idle(
    state: dict[str, Any],
    *,
    detail_idle: str = "Ready",
    detail_working: str = "Working...",
) -> None:
    if _user_confirm_pending(state):
        _sync_confirm_flags(state)
        return
    if state.get("composer_blocking_pending"):
        _apply_review_confirm(state)
        return
    if _reconcile_review_status(state):
        return
    if state.get("pending_confirm"):
        return
    if state.get("agent_turn_active"):
        state["agent_status"] = "thinking"
        state["agent_detail"] = detail_working
        return
    _clear_confirm(state)
    state["agent_status"] = "idle"
    state["agent_detail"] = detail_idle


def _expire_stale_confirm(state: dict[str, Any], now: int) -> None:
    if not _user_confirm_pending(state):
        return
    if state.get("composer_blocking_pending"):
        return
    if _is_review_confirm_detail(state):
        return
    if _is_action_confirm_detail(state):
        since = int(state.get("confirm_since") or 0)
        if since > 0 and (now - since) > 120:
            _clear_confirm(state)


def _reconcile_review_status(state: dict[str, Any]) -> bool:
    if not state.get("composer_blocking_pending"):
        return False
    return _apply_review_confirm(state)


def _normalize_agent_status(state: dict[str, Any]) -> None:
    if state.get("agent_status") == "active":
        state["agent_status"] = "thinking"
    if state.get("composer_blocking_pending"):
        _apply_review_confirm(state)
    if _user_confirm_pending(state):
        _sync_confirm_flags(state)
        return
    _apply_turn_busy_guard(state)


def _is_hook_action_confirm_detail(state: dict[str, Any]) -> bool:
    """Shell / MCP / subagent hooks waiting for user approval in Cursor."""
    detail = str(state.get("agent_detail") or "")
    return detail.startswith(
        ("Confirm Shell:", "Confirm MCP:", "Confirm subagent:")
    )


def _is_action_confirm_detail(state: dict[str, Any]) -> bool:
    detail = str(state.get("agent_detail") or "")
    return detail.startswith(
        ("Confirm Shell:", "Confirm MCP:", "Confirm subagent:", "Confirm:", "Confirm ")
    )


def _track_review_from_tool(state: dict[str, Any], tool_input: dict[str, Any]) -> None:
    for path in _paths_from_tool_input(tool_input):
        _track_review_file(state, path)


def _paths_from_tool_input(tool_input: dict[str, Any]) -> list[str]:
    paths: list[str] = []
    for key in ("path", "file_path", "target_file", "target_notebook", "notebook_path"):
        value = tool_input.get(key)
        if isinstance(value, str) and value.strip():
            paths.append(value.strip())
    return paths


def _track_review_file(state: dict[str, Any], file_path: str) -> None:
    if not file_path:
        return
    name = Path(file_path).name
    if not name:
        return
    files = state.setdefault("pending_review_files", [])
    if name not in files:
        files.append(name)
    state["pending_review_files"] = files[-8:]


def _apply_review_confirm(state: dict[str, Any]) -> bool:
    if not state.get("composer_blocking_pending"):
        return False
    files = state.get("pending_review_files") or []
    if files:
        count = len(files)
        if count == 1:
            detail = f"Review: Keep / Undo ({files[0]})"
        else:
            detail = f"Review: Keep / Undo ({count} files)"
        _set_confirm(state, detail)
        return True
    _set_confirm(state, REVIEW_FALLBACK_DETAIL)
    return True


def _tool_needs_confirm(tool_name: str) -> bool:
    return tool_name in {"AskQuestion", "SwitchMode"}


def _format_confirm_detail(tool_name: str, tool_input: dict[str, Any]) -> str:
    if tool_name == "AskQuestion":
        questions = tool_input.get("questions") or []
        if questions and isinstance(questions[0], dict):
            prompt = str(questions[0].get("prompt") or "Choose option")
            return f"Confirm: {prompt[:44]}"
        return "Confirm: choose option"

    if tool_name == "SwitchMode":
        target = str(tool_input.get("target_mode_id") or "plan")
        return f"Confirm: switch to {target}"

    if tool_name in {"Write", "StrReplace", "Delete"}:
        path = str(tool_input.get("path") or tool_input.get("target_file") or "")
        name = Path(path).name if path else "file"
        return f"Confirm {tool_name}: {name[:32]}"

    return f"Confirm: {tool_name}"


def _format_tool_detail(tool_name: str, tool_input: dict[str, Any]) -> str:
    if tool_name == "Shell":
        command = str(tool_input.get("command") or "")
        return f"Shell: {command[:40]}"
    if tool_name in {"Read", "Write", "StrReplace", "Delete"}:
        path = str(tool_input.get("path") or tool_input.get("target_file") or "")
        name = Path(path).name if path else "file"
        return f"{tool_name}: {name[:32]}"
    if tool_name == "Grep":
        pattern = str(tool_input.get("pattern") or "")
        return f"Grep: {pattern[:32]}"
    if tool_name == "Task":
        return "Task: subagent"
    return f"{tool_name}"


store = StateStore()
