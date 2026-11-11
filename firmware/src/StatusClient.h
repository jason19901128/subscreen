#pragma once

#include <ArduinoJson.h>

struct CursorStatus {
  String agentStatus = "boot";
  String agentDetail = "Starting...";
  String model = "-";
  String project = "-";
  int promptCount = 0;
  int toolCount = 0;
  int estimatedTokens = 0;
  int contextTokens = 0;
  int contextWindowSize = 200000;
  bool onDemandEnabled = false;
  bool onDemandUnlimited = false;
  int onDemandUsedCents = -1;
  int onDemandRemainingCents = -1;
  int onDemandLimitCents = -1;
  int planLimitCents = -1;
  int planIncludedSpendCents = -1;
  int planRemainingCents = -1;
  int planPercentUsed = -1;
  // Included-in-Team percents (match Cursor Settings UI).
  int autoPercentUsed = -1;  // Cursor Models
  int apiPercentUsed = -1;   // Other Models
  // Usage-limits reset cycle (billingCycleStart → End).
  int cycleDays = -1;
  int cycleElapsedDays = -1;
  int cycleRemainingDays = -1;
  int cyclePercentUsed = -1;
  String onDemandError = "";
  bool bridgeReachable = false;
  bool bridgeOnline = false;
  bool cursorOnline = false;
  // "wifi" | "usb" | "" — how the latest successful sample arrived.
  String transport = "";
};

bool parseCursorStatusJson(const char *json, CursorStatus &out);
bool fetchCursorStatus(CursorStatus &out);
bool pollUsbCursorStatus(CursorStatus &out);
