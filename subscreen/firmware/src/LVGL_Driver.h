#pragma once

#ifndef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_INCLUDE_SIMPLE 1
#endif

#include <lvgl.h>
#include "Display_ST7789.h"

#define LVGL_WIDTH LCD_WIDTH
#define LVGL_HEIGHT LCD_HEIGHT
#define LVGL_BUF_LEN (LVGL_WIDTH * LVGL_HEIGHT / 20)
#define EXAMPLE_LVGL_TICK_PERIOD_MS 10

void Lvgl_Init(void (*ui_init)(void));
void Lvgl_Loop(void);
void Lvgl_SetPaused(bool paused);
