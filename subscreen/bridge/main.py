from __future__ import annotations

import argparse
from contextlib import asynccontextmanager
from typing import Any

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from context import ContextPoller
from state import store
from usage import UsagePoller

usage_poller: UsagePoller | None = None
context_poller: ContextPoller | None = None


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global usage_poller, context_poller
    usage_poller = UsagePoller(store.set_on_demand_usage)
    usage_poller.start()
    store.startup_sync_composer()
    context_poller = ContextPoller(
        store.snapshot,
        store.set_context_metrics,
        store.update_from_composer,
    )
    context_poller.start()
    yield
    if context_poller:
        context_poller.stop()
    if usage_poller:
        usage_poller.stop()


app = FastAPI(title="Subscreen Bridge", version="0.1.0", lifespan=lifespan)


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.get("/status")
def status() -> dict[str, Any]:
    return store.snapshot()


@app.post("/update/hook")
async def update_hook(request: Request) -> JSONResponse:
    payload = await request.json()
    if isinstance(payload, dict):
        store.apply_hook(payload)
    return JSONResponse({"ok": True})


@app.post("/refresh/usage")
def refresh_usage() -> dict[str, Any]:
    if usage_poller is None:
        return {"ok": False, "error": "poller_not_ready"}
    data = usage_poller.refresh_once()
    return {"ok": True, "on_demand_usage": data}


@app.post("/refresh/context")
def refresh_context() -> dict[str, Any]:
    if context_poller is None:
        return {"ok": False, "error": "poller_not_ready"}
    data = context_poller.refresh_once()
    return {"ok": True, "session_metrics": store.snapshot().get("session_metrics", {})}


def main() -> None:
    parser = argparse.ArgumentParser(description="Subscreen bridge for Cursor IDE")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
