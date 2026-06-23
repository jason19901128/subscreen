#include <WiFi.h>
#include <esp_wifi.h>

#include "LVGL_Driver.h"
#include "RgbLed.h"
#include "StatusClient.h"
#include "UiScreen.h"

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

static CursorStatus g_status;
static CursorStatus g_cached_status;
static bool g_has_cached_status = false;
static uint8_t g_bridge_fail_streak = 0;
static unsigned long last_fetch_ms = 0;
static const unsigned long FETCH_INTERVAL_BUSY_MS = 500;
static const unsigned long FETCH_INTERVAL_IDLE_MS = 1500;
static const unsigned long FETCH_INTERVAL_SLEEP_MS = 3000;
static const unsigned long LOOP_DELAY_AWAKE_MS = 30;
static const unsigned long LOOP_DELAY_SLEEP_MS = 200;
static const uint8_t BRIDGE_FAIL_OFFLINE_THRESHOLD = 5;

static bool agent_is_busy(const CursorStatus &status) {
  const String &st = status.agentStatus;
  return st == "thinking" || st == "running_tool" || st == "running" ||
         st == "awaiting_confirm" || st == "error";
}

static unsigned long fetch_interval_ms(void) {
  if (ui_is_screensaver()) {
    return FETCH_INTERVAL_SLEEP_MS;
  }
  if (g_has_cached_status && !agent_is_busy(g_status)) {
    return FETCH_INTERVAL_IDLE_MS;
  }
  return FETCH_INTERVAL_BUSY_MS;
}

static void connect_wifi(void) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  }
}

static void poll_status(void) {
  const unsigned long now = millis();
  const unsigned long interval = fetch_interval_ms();
  if (now - last_fetch_ms < interval) {
    return;
  }
  last_fetch_ms = now;

  CursorStatus fetched;
  if (fetchCursorStatus(fetched)) {
    g_bridge_fail_streak = 0;
    g_cached_status = fetched;
    g_has_cached_status = true;
    g_status = fetched;
  } else if (g_has_cached_status && g_bridge_fail_streak < BRIDGE_FAIL_OFFLINE_THRESHOLD) {
    g_bridge_fail_streak++;
    g_status = g_cached_status;
    g_status.bridgeReachable = false;
    g_status.bridgeOnline = false;
    g_status.agentDetail = "Bridge reconnecting...";
  } else {
    g_bridge_fail_streak++;
    g_status = fetched;
    g_status.bridgeReachable = false;
    g_status.bridgeOnline = false;
  }

  ui_update(g_status);
  if (!ui_is_screensaver()) {
    rgb_set_status(g_status.agentStatus, g_status.bridgeReachable);
  }
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(200);

  LCD_Init();
  rgb_init();
  connect_wifi();
  Lvgl_Init(ui_init);

  g_status.agentDetail = WiFi.status() == WL_CONNECTED ? "WiFi connected" : "WiFi failed";
  ui_update(g_status);
}

void loop() {
  poll_status();
  ui_screensaver_tick();
  ui_countdown_tick();
  if (!ui_is_screensaver()) {
    Lvgl_Loop();
    ui_blink_tick();
    rgb_blink_tick();
    delay(LOOP_DELAY_AWAKE_MS);
  } else {
    delay(LOOP_DELAY_SLEEP_MS);
  }
}
