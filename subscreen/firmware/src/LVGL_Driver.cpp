#include "LVGL_Driver.h"

#include <esp_timer.h>

static lv_color_t buf1[LVGL_BUF_LEN];
static lv_color_t buf2[LVGL_BUF_LEN];
static void (*ui_init_cb)(void) = nullptr;
static esp_timer_handle_t lvgl_tick_timer = nullptr;
static bool lvgl_paused = false;

static void Lvgl_Display_LCD(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t *)px_map);
  lv_display_flush_ready(disp);
}

static void Lvgl_Touchpad_Read(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  data->state = LV_INDEV_STATE_RELEASED;
}

static void example_increase_lvgl_tick(void *arg) {
  (void)arg;
  if (!lvgl_paused) {
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
  }
}

void Lvgl_Init(void (*ui_init)(void)) {
  ui_init_cb = ui_init;
  lv_init();

  lv_display_t *disp = lv_display_create(LVGL_WIDTH, LVGL_HEIGHT);
  lv_display_set_flush_cb(disp, Lvgl_Display_LCD);
  lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, Lvgl_Touchpad_Read);

  if (ui_init_cb) {
    ui_init_cb();
  }

  const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &example_increase_lvgl_tick,
      .name = "lvgl_tick",
  };
  esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);
}

void Lvgl_SetPaused(bool paused) {
  lvgl_paused = paused;
}

void Lvgl_Loop(void) {
  if (lvgl_paused) {
    return;
  }
  lv_timer_handler();
}
