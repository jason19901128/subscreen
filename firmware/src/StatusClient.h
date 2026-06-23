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
  String onDemandError = "";
  bool bridgeReachable = false;
  bool bridgeOnline = false;
  bool cursorOnline = false;
};

bool fetchCursorStatus(CursorStatus &out);
