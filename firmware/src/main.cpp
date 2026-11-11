#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_rom_sys.h>

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
static CursorStatus g_usb_latest;
static bool g_has_cached_status = false;
static bool g_has_usb_latest = false;
static uint8_t g_bridge_fail_streak = 0;
static unsigned long last_fetch_ms = 0;
static unsigned long last_usb_ok_ms = 0;
static const unsigned long FETCH_INTERVAL_BUSY_MS = 500;
static const unsigned long FETCH_INTERVAL_IDLE_MS = 1500;
static const unsigned long FETCH_INTERVAL_SLEEP_MS = 3000;
static const unsigned long USB_FRESH_MS = 4000;
static const unsigned long LOOP_DELAY_AWAKE_MS = 30;
static const unsigned long LOOP_DELAY_SLEEP_MS = 200;
static const uint8_t BRIDGE_FAIL_OFFLINE_THRESHOLD = 5;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;

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

static bool usb_is_fresh(void) {
  return g_has_usb_latest && last_usb_ok_ms != 0 &&
         (millis() - last_usb_ok_ms) < USB_FRESH_MS;
}

static void connect_wifi(void) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(200);
    // Keep LVGL alive while associating so the boot UI stays visible.
    if (!ui_is_screensaver()) {
      Lvgl_Loop();
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_rom_printf("wifi ok\n");
  } else {
    esp_rom_printf("wifi timeout\n");
  }
}

static void apply_status_success(const CursorStatus &fetched) {
  g_bridge_fail_streak = 0;
  g_cached_status = fetched;
  g_has_cached_status = true;
  g_status = fetched;
}

static void publish_status(void) {
  ui_update(g_status);
  if (!ui_is_screensaver()) {
    rgb_set_status(g_status.agentStatus, g_status.bridgeReachable);
  }
}

static void drain_usb_status(void) {
  CursorStatus usb_fetched;
  if (!pollUsbCursorStatus(usb_fetched)) {
    return;
  }
  g_usb_latest = usb_fetched;
  g_has_usb_latest = true;
  last_usb_ok_ms = millis();
}

static void poll_status(void) {
  drain_usb_status();

  const unsigned long now = millis();
  const unsigned long interval = fetch_interval_ms();
  if (now - last_fetch_ms < interval) {
    return;
  }
  last_fetch_ms = now;

  const bool wifi_up = WiFi.status() == WL_CONNECTED;

  if (wifi_up) {
    CursorStatus fetched;
    if (fetchCursorStatus(fetched)) {
      apply_status_success(fetched);
      publish_status();
      return;
    }
  }

  if (usb_is_fresh()) {
    apply_status_success(g_usb_latest);
    publish_status();
    return;
  }

  g_bridge_fail_streak++;
  if (ui_is_screensaver() && g_bridge_fail_streak < BRIDGE_FAIL_OFFLINE_THRESHOLD) {
    return;
  }
  if (g_has_cached_status && g_bridge_fail_streak < BRIDGE_FAIL_OFFLINE_THRESHOLD) {
    g_status = g_cached_status;
    g_status.bridgeReachable = false;
    g_status.bridgeOnline = false;
    g_status.agentDetail = "Bridge reconnecting...";
  } else {
    g_status.bridgeReachable = false;
    g_status.bridgeOnline = false;
    g_status.cursorOnline = false;
    g_status.agentStatus = "offline";
    g_status.agentDetail = wifi_up ? "Bridge timeout" : "WiFi/USB offline";
    g_status.transport = "";
  }
  publish_status();
}

void setup() {
  // Keep full clock for stable SPI/LCD; WiFi modem sleep still saves power later.
  setCpuFrequencyMhz(160);
  Serial.begin(115200);
  delay(50);
  esp_rom_printf("boot setup\n");

  LCD_Init();
  esp_rom_printf("lcd ok\n");
  rgb_init();

  // Paint UI before WiFi so the panel never sits on uninitialized GRAM snow.
  Lvgl_Init(ui_init);
  g_status.agentStatus = "idle";
  g_status.agentDetail = "Connecting WiFi...";
  ui_update(g_status);
  Lvgl_Loop();
  esp_rom_printf("ui ok\n");

  connect_wifi();

  if (WiFi.status() == WL_CONNECTED) {
    g_status.agentDetail = "WiFi connected";
  } else {
    g_status.agentDetail = "USB/WiFi waiting...";
  }
  ui_update(g_status);
  esp_rom_printf("setup done\n");
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
