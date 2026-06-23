#!/usr/bin/env python3
"""Cycle all subscreen agent statuses via Bridge hooks (for display testing)."""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request

BRIDGE_URL = "http://127.0.0.1:8765/update/hook"
HOLD_SEC = 4


def post_hook(payload: dict) -> None:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BRIDGE_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=2) as resp:
        resp.read()


def show(label: str, payload: dict) -> None:
    print(f"  -> {label}", flush=True)
    post_hook(payload)
    time.sleep(HOLD_SEC)
    st = urllib.request.urlopen("http://127.0.0.1:8765/status", timeout=2)
    data = json.loads(st.read())
    print(f"     screen: {data.get('agent_status')} | {data.get('agent_detail', '')[:50]}", flush=True)


def main() -> int:
    base = {
        "workspace_roots": ["/Users/test/subscreen"],
        "cwd": "/Users/test/subscreen",
        "model": "default",
    }

    steps = [
        ("Thinking (session)", {**base, "hook_event_name": "sessionStart", "composer_mode": "agent"}),
        ("Thinking", {**base, "hook_event_name": "beforeSubmitPrompt", "prompt": "Test thinking status"}),
        ("Thinking (thought)", {**base, "hook_event_name": "afterAgentThought"}),
        ("Tool", {
            **base,
            "hook_event_name": "preToolUse",
            "tool_name": "Read",
            "tool_input": {"path": "README.md"},
        }),
        ("Running", {**base, "hook_event_name": "postToolUse"}),
        ("Running (response)", {**base, "hook_event_name": "afterAgentResponse", "text": "Done."}),
        ("Confirm (AskQuestion)", {
            **base,
            "hook_event_name": "preToolUse",
            "tool_name": "AskQuestion",
            "tool_input": {"questions": [{"prompt": "Pick an option?"}]},
        }),
        ("Confirm (Shell)", {
            **base,
            "hook_event_name": "beforeShellExecution",
            "command": "echo hello",
        }),
        ("Confirm (Keep All)", {
            **base,
            "hook_event_name": "afterFileEdit",
            "file_path": "/tmp/a.txt",
        }),
        ("Confirm (Keep All stop)", {**base, "hook_event_name": "stop", "status": "completed"}),
        ("Error", {**base, "hook_event_name": "postToolUseFailure", "error_message": "Test error"}),
        ("Idle (completed)", {**base, "hook_event_name": "stop", "status": "completed"}),
        ("Idle (session end)", {**base, "hook_event_name": "sessionEnd"}),
    ]

    print(f"Subscreen status test ({HOLD_SEC}s each). Watch the device.\n", flush=True)
    try:
        urllib.request.urlopen("http://127.0.0.1:8765/health", timeout=2)
    except (urllib.error.URLError, OSError) as e:
        print(f"Bridge not reachable: {e}", file=sys.stderr)
        return 1

    for label, payload in steps:
        show(label, payload)

    print("\nDone. Offline/WiFi failed only appear when ESP32 loses network.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
