#pragma once

#include "StatusClient.h"

void ui_init(void);
void ui_update(const CursorStatus &status);
void ui_blink_tick(void);
void ui_screensaver_tick(void);
void ui_countdown_tick(void);
bool ui_is_screensaver(void);
