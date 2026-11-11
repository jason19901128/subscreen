#include "StatusClient.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <cstring>

#ifndef BRIDGE_HOST
#define BRIDGE_HOST "192.168.0.79"
#endif

#ifndef BRIDGE_PORT
#define BRIDGE_PORT 8765
#endif

// USB serial framing from Mac Bridge: "SS1 {json}\n"
static const char USB_STATUS_PREFIX[] = "SS1 ";
static const size_t USB_LINE_MAX = 2048;
static char usb_line_buf[USB_LINE_MAX];
static size_t usb_line_len = 0;

static String statusUrl() {
  return String("http://") + BRIDGE_HOST + ":" + String(BRIDGE_PORT) + "/status";
}

static int odIntOrNeg(JsonObject od, const char *key) {
  if (od[key].isNull()) {
    return -1;
  }
  return od[key].as<int>();
}

bool parseCursorStatusJson(const char *json, CursorStatus &out) {
  out.bridgeReachable = false;

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
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
  const bool awaitingConfirm =
      pendingConfirm || composerBlocking || out.agentStatus == "awaiting_confirm";
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
  out.onDemandUsedCents = odIntOrNeg(od, "used_cents");
  out.onDemandRemainingCents = odIntOrNeg(od, "remaining_cents");
  out.onDemandLimitCents = odIntOrNeg(od, "limit_cents");
  out.planLimitCents = odIntOrNeg(od, "plan_limit_cents");
  out.planIncludedSpendCents = odIntOrNeg(od, "plan_included_spend_cents");
  out.planRemainingCents = odIntOrNeg(od, "plan_remaining_cents");
  if (!od["plan_percent_used"].isNull()) {
    out.planPercentUsed = static_cast<int>(od["plan_percent_used"].as<float>() + 0.5f);
  } else {
    out.planPercentUsed = -1;
  }
  // Cursor Settings floors these (2.78 → "2% used").
  if (!od["auto_percent_used"].isNull()) {
    out.autoPercentUsed = static_cast<int>(od["auto_percent_used"].as<float>());
  } else {
    out.autoPercentUsed = -1;
  }
  if (!od["api_percent_used"].isNull()) {
    out.apiPercentUsed = static_cast<int>(od["api_percent_used"].as<float>());
  } else {
    out.apiPercentUsed = -1;
  }
  out.cycleDays = odIntOrNeg(od, "cycle_days");
  out.cycleElapsedDays = odIntOrNeg(od, "cycle_elapsed_days");
  out.cycleRemainingDays = odIntOrNeg(od, "cycle_remaining_days");
  if (!od["cycle_percent_used"].isNull()) {
    out.cyclePercentUsed = static_cast<int>(od["cycle_percent_used"].as<float>() + 0.5f);
  } else {
    out.cyclePercentUsed = -1;
  }
  out.onDemandError = od["error"] | "";
  return true;
}

bool fetchCursorStatus(CursorStatus &out) {
  out.bridgeReachable = false;
  out.transport = "";

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

  if (!parseCursorStatusJson(body.c_str(), out)) {
    return false;
  }
  out.transport = "wifi";
  return true;
}

bool pollUsbCursorStatus(CursorStatus &out) {
  bool got = false;
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) {
      break;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      usb_line_buf[usb_line_len] = '\0';
      if (usb_line_len > 4 && strncmp(usb_line_buf, USB_STATUS_PREFIX, 4) == 0) {
        CursorStatus parsed;
        if (parseCursorStatusJson(usb_line_buf + 4, parsed)) {
          out = parsed;
          out.transport = "usb";
          got = true;
        }
      }
      usb_line_len = 0;
      continue;
    }
    if (usb_line_len + 1 < USB_LINE_MAX) {
      usb_line_buf[usb_line_len++] = static_cast<char>(ch);
    } else {
      // Overflow — drop line.
      usb_line_len = 0;
    }
  }
  return got;
}
