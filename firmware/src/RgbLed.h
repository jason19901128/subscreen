#pragma once

#include <Adafruit_NeoPixel.h>

#define RGB_PIN 8
#define RGB_COUNT 1

void rgb_init(void);
void rgb_set_status(const String &agentStatus, bool bridgeReachable);
void rgb_blink_tick(void);
void rgb_sleep(void);
void rgb_wake(void);
