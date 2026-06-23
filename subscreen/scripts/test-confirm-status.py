#!/usr/bin/env python3
"""Unit-test confirm / idle transitions in bridge/state.py (no hardware)."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bridge"))

import state as st  # noqa: E402


def snap(store: st.StateStore) -> tuple[str, bool, str]:
    s = store.snapshot()
    return (
        str(s.get("agent_status")),
        bool(s.get("pending_confirm")),
        str(s.get("agent_detail") or "")[:60],
    )


def assert_confirm(store: st.StateStore, label: str) -> None:
    status, pending, detail = snap(store)
    if status != "awaiting_confirm" or not pending:
        raise AssertionError(f"{label}: expected CONFIRM, got status={status} pending={pending} detail={detail}")


def assert_idle(store: st.StateStore, label: str) -> None:
    status, pending, detail = snap(store)
    if pending or status == "awaiting_confirm":
        raise AssertionError(f"{label}: expected not CONFIRM, got status={status} pending={pending} detail={detail}")


def set_composer_blocking(store: st.StateStore, blocking: bool) -> None:
    flags = {"has_blocking_pending_actions": blocking}
    store.update_from_composer(flags)
    store.update_from_composer(flags)


def run() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        st.STATE_FILE = Path(tmp) / "state.json"
        store = st.StateStore()
        base = {"workspace_roots": ["/tmp/proj"], "model": "default"}

        # AskQuestion: preToolUse -> confirm; unrelated postToolUse -> stay confirm
        store.apply_hook({**base, "hook_event_name": "preToolUse", "tool_name": "AskQuestion", "tool_input": {"questions": [{"prompt": "Pick?"}]}})
        assert_confirm(store, "AskQuestion preToolUse")
        store.apply_hook({**base, "hook_event_name": "postToolUse", "tool_name": "Read"})
        assert_confirm(store, "postToolUse Read while AskQuestion pending")
        store.apply_hook({**base, "hook_event_name": "postToolUse", "tool_name": "AskQuestion"})
        assert_idle(store, "postToolUse AskQuestion clears")

        # Shell approval path
        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "run shell"})
        store.apply_hook({**base, "hook_event_name": "beforeShellExecution", "command": "ls -la"})
        assert_confirm(store, "beforeShellExecution")
        store.apply_hook({**base, "hook_event_name": "afterAgentThought"})
        assert_confirm(store, "afterAgentThought must not clear Shell confirm")
        store.apply_hook({**base, "hook_event_name": "postToolUse", "tool_name": "Shell"})
        assert_confirm(store, "postToolUse Shell must not clear before afterShellExecution")
        store.apply_hook({**base, "hook_event_name": "preToolUse", "tool_name": "Read"})
        assert_confirm(store, "preToolUse Read must not clear Shell confirm")
        store.apply_hook({**base, "hook_event_name": "preToolUse", "tool_name": "Grep"})
        assert_confirm(store, "preToolUse Grep must not clear Shell confirm")
        store.apply_hook({**base, "hook_event_name": "stop", "status": "completed"})
        assert_confirm(store, "stop must not clear Shell confirm")
        store.apply_hook({**base, "hook_event_name": "afterShellExecution"})
        status, pending, _ = snap(store)
        if status == "idle":
            raise AssertionError("afterShellExecution should not idle during active turn")

        # Mid-turn postToolUse should stay busy (not idle)
        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "work"})
        store.apply_hook({**base, "hook_event_name": "postToolUse", "tool_name": "Read"})
        status, pending, _ = snap(store)
        if status == "idle":
            raise AssertionError("postToolUse Read during turn should not be idle")

        # Review (Keep/Undo): hooks alone stay busy until Composer reports blocking
        store.apply_hook(
            {
                **base,
                "hook_event_name": "postToolUse",
                "tool_name": "StrReplace",
                "tool_input": {},
            }
        )
        status, pending, _ = snap(store)
        if status == "awaiting_confirm" or pending:
            raise AssertionError("postToolUse StrReplace must not CONFIRM without composer blocking")

        store.apply_hook({**base, "hook_event_name": "afterFileEdit", "file_path": "/tmp/foo.py"})
        status, pending, _ = snap(store)
        if status == "awaiting_confirm" or pending:
            raise AssertionError("afterFileEdit must not CONFIRM without composer blocking")

        set_composer_blocking(store, True)
        assert_confirm(store, "composer blocking shows review confirm")

        store.apply_hook({**base, "hook_event_name": "afterAgentResponse", "text": "Done."})
        assert_confirm(store, "afterAgentResponse keeps review confirm while blocking")

        store.apply_hook({**base, "hook_event_name": "stop", "status": "completed"})
        assert_confirm(store, "stop with composer still blocking")

        # new prompt clears review
        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "next"})
        assert snap(store)[1] is False and snap(store)[0] == "thinking", "beforeSubmitPrompt clears review"

        # Stale persisted review should not survive load
        st.STATE_FILE = Path(tmp) / "stale.json"
        stale = st.StateStore()
        stale.apply_hook({**base, "hook_event_name": "postToolUse", "tool_name": "Write", "tool_input": {"path": "/tmp/old.py"}})
        with st.StateStore._lock if False else stale._lock:
            stale._state["updated_at"] = int(__import__("time").time()) - 500
        stale._persist_flush_force()
        fresh = st.StateStore()
        status, pending, _ = snap(fresh)
        if pending or status == "awaiting_confirm":
            raise AssertionError("stale review must be cleared on bridge load")

        # Composer DB: blocking cleared => dismiss review (Keep clicked)
        store.apply_hook({**base, "hook_event_name": "postToolUse", "tool_name": "StrReplace", "tool_input": {"path": "a.py"}})
        set_composer_blocking(store, True)
        assert_confirm(store, "composer blocking shows review")
        set_composer_blocking(store, False)
        status, pending, _ = snap(store)
        if pending:
            raise AssertionError("composer unblocking should clear review confirm")

        # Keep without composer ever blocking (hook-only review) still clears on poll
        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "x"})
        store.apply_hook(
            {
                **base,
                "hook_event_name": "postToolUse",
                "tool_name": "StrReplace",
                "tool_input": {"path": "z.py"},
            }
        )
        set_composer_blocking(store, True)
        assert_confirm(store, "hook review with composer blocking")
        set_composer_blocking(store, False)
        status, pending, _ = snap(store)
        if pending:
            raise AssertionError("hook-only review must clear when composer not blocking")

        # Thinking must not flash CONFIRM between composer polls
        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "work"})
        store.apply_hook({**base, "hook_event_name": "afterAgentThought"})
        status, pending, _ = snap(store)
        if status == "awaiting_confirm" or pending:
            raise AssertionError("afterAgentThought during turn must stay thinking, not confirm")

        # MCP path
        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "mcp"})
        store.apply_hook({**base, "hook_event_name": "beforeMCPExecution", "tool_name": "my_tool"})
        assert_confirm(store, "beforeMCPExecution")
        store.apply_hook({**base, "hook_event_name": "afterMCPExecution"})
        status, pending, _ = snap(store)
        if status == "idle" and not pending:
            pass
        elif status != "thinking":
            raise AssertionError(f"afterMCPExecution expected thinking during turn, got {status}")

        store.apply_hook({**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "x"})
        store.apply_hook({**base, "hook_event_name": "stop", "status": "aborted"})
        status, pending, detail = snap(store)
        if status == "thinking" or status == "awaiting_confirm":
            raise AssertionError(f"stop aborted must not be thinking, got {status} detail={detail}")

    print("All confirm-status checks passed.", flush=True)


if __name__ == "__main__":
    try:
        run()
    except AssertionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        raise SystemExit(1)
