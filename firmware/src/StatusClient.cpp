#include "StatusClient.h"

#include <HTTPClient.h>
#include <WiFi.h>

#ifndef BRIDGE_HOST
#define BRIDGE_HOST "192.168.0.79"
#endif

#ifndef BRIDGE_PORT
#define BRIDGE_PORT 8765
#endif

static String statusUrl() {
  return String("http://") + BRIDGE_HOST + ":" + String(BRIDGE_PORT) + "/status";
}

bool fetchCursorStatus(CursorStatus &out) {
  out.bridgeReachable = false;

  if (WiFi.status() != WL_CONNECTED) {
    out.bridgeOnline = false;
    out.cursorOnline = false;
    out.agentStatus = "offline";
    out.agentDetail = "WiFi disconnected";
    return false;
  }

  HTTPClient http;
  http.setTimeout(4000);
  http.begin(statusUrl());

  const int code = http.GET();
  if (code != 200) {
    http.end();
    out.bridgeOnline = false;
    out.cursorOnline = false;
    out.agentStatus = "offline";
    out.agentDetail = code > 0 ? String("Bridge HTTP ") + code : "Bridge timeout";
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    out.bridgeOnline = false;
    out.cursorOnline = false;
    out.agentStatus = "offline";
    out.agentDetail = "Bad JSON";
    return false;
  }

  out.bridgeReachable = true;
  out.agentDetail = doc["agent_detail"] | "";
  out.agentStatus = doc["agent_status"] | "idle";
  if (out.agentStatus == "active") {
    out.agentStatus = "thinking";
  }
  const bool pendingConfirm = doc["pending_confirm"] | false;
  const bool composerBlocking = doc["composer_blocking_pending"] | false;
  const bool agentTurnActive = doc["agent_turn_active"] | false;
  const bool cursorOnline = doc["cursor_online"] | false;
  const bool detailConfirm =
      out.agentDetail.startsWith("Review:") || out.agentDetail.startsWith("Confirm");
  const bool awaitingConfirm = pendingConfirm || composerBlocking ||
                               out.agentStatus == "awaiting_confirm" || detailConfirm;
  if (awaitingConfirm) {
    out.agentStatus = "awaiting_confirm";
  } else if (out.agentDetail == "Task aborted" || out.agentDetail == "Task completed" ||
             out.agentDetail == "Session ended" || out.agentDetail == "Waiting for Cursor") {
    out.agentStatus = "idle";
  } else if (agentTurnActive && cursorOnline &&
             (out.agentStatus == "idle" || out.agentStatus == "running")) {
    out.agentStatus = "thinking";
  } else if (!cursorOnline &&
             (out.agentStatus == "thinking" || out.agentStatus == "running_tool" ||
              out.agentStatus == "running")) {
    out.agentStatus = "idle";
    if (out.agentDetail.length() == 0 || out.agentDetail == "Working...") {
      out.agentDetail = "Waiting for Cursor";
    }
  }
  out.model = doc["model"] | "-";
  out.project = doc["project"] | "-";
  out.bridgeOnline = doc["bridge_online"] | true;
  out.cursorOnline = doc["cursor_online"] | false;

  JsonObject metrics = doc["session_metrics"];
  out.promptCount = metrics["prompt_count"] | 0;
  out.toolCount = metrics["tool_count"] | 0;
  out.estimatedTokens = metrics["estimated_tokens"] | 0;
  out.contextTokens = metrics["context_tokens"] | 0;
  out.contextWindowSize = metrics["context_window_size"] | 200000;
  if (out.contextTokens <= 0 && out.estimatedTokens > 0) {
    out.contextTokens = out.estimatedTokens;
  }
  if (out.contextWindowSize <= 0) {
    out.contextWindowSize = 200000;
  }

  JsonObject od = doc["on_demand_usage"];
  out.onDemandEnabled = od["enabled"] | false;
  out.onDemandUnlimited = od["unlimited"] | false;
  if (!od["used_cents"].isNull()) {
    out.onDemandUsedCents = od["used_cents"].as<int>();
  } else {
    out.onDemandUsedCents = -1;
  }
  if (!od["remaining_cents"].isNull()) {
    out.onDemandRemainingCents = od["remaining_cents"].as<int>();
  } else {
    out.onDemandRemainingCents = -1;
  }
  if (!od["limit_cents"].isNull()) {
    out.onDemandLimitCents = od["limit_cents"].as<int>();
  } else {
    out.onDemandLimitCents = -1;
  }
  out.onDemandError = od["error"] | "";
  return true;
}
