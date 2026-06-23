#include "RgbLed.h"

static Adafruit_NeoPixel pixels(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);
static String last_agent_status = "";
static bool last_bridge_online = true;
static bool blink_on = true;
static bool rgb_asleep = false;
static unsigned long last_blink_ms = 0;

static const unsigned long RGB_BLINK_MS = 450;

void rgb_init(void) {
  pixels.begin();
  pixels.setBrightness(40);
  pixels.setPixelColor(0, pixels.Color(0, 0, 80));
  pixels.show();
}

void rgb_set_status(const String &agentStatus, bool bridgeReachable) {
  last_agent_status = agentStatus;
  last_bridge_online = bridgeReachable;
  if (!rgb_asleep) {
    rgb_blink_tick();
  }
}

void rgb_sleep(void) {
  rgb_asleep = true;
  pixels.setPixelColor(0, 0);
  pixels.show();
}

void rgb_wake(void) {
  rgb_asleep = false;
  rgb_blink_tick();
}

void rgb_blink_tick(void) {
  if (rgb_asleep) {
    return;
  }
  const String &agentStatus = last_agent_status;
  const bool bridgeOnline = last_bridge_online;

  const bool should_blink =
      bridgeOnline && (agentStatus == "awaiting_confirm" || agentStatus == "error");

  if (should_blink) {
    const unsigned long now = millis();
    if (now - last_blink_ms >= RGB_BLINK_MS) {
      last_blink_ms = now;
      blink_on = !blink_on;
    }
    if (!blink_on) {
      pixels.setPixelColor(0, pixels.Color(8, 8, 8));
      pixels.show();
      return;
    }
    pixels.setBrightness(agentStatus == "awaiting_confirm" ? 70 : 60);
  } else {
    pixels.setBrightness(40);
    blink_on = true;
  }

  uint32_t color;
  if (!bridgeOnline) {
    color = pixels.Color(90, 0, 0);
  } else if (agentStatus == "running_tool") {
    color = pixels.Color(90, 70, 0);
  } else if (agentStatus == "thinking" || agentStatus == "running") {
    color = pixels.Color(0, 90, 0);
  } else if (agentStatus == "awaiting_confirm") {
    color = pixels.Color(90, 40, 0);
  } else if (agentStatus == "error") {
    color = pixels.Color(90, 0, 0);
  } else if (agentStatus == "idle") {
    color = pixels.Color(0, 40, 90);
  } else {
    color = pixels.Color(0, 0, 90);
  }

  pixels.setPixelColor(0, color);
  pixels.show();
}
