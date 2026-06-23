#include "UiScreen.h"

#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include "RgbLed.h"

#include <lvgl.h>

static const unsigned long SCREENSAVER_TIMEOUT_MS = 60UL * 1000;
static const int SCREENSAVER_COUNTDOWN_SEC =
    static_cast<int>(SCREENSAVER_TIMEOUT_MS / 1000UL);
static const int SLEEP_ARC_SIZE = 22;
static const unsigned long BLINK_INTERVAL_MS = 450;
static const uint8_t NORMAL_BACKLIGHT = 40;
static const int EDGE_THICK = 8;
static const int EDGE_THICK_CONFIRM = 12;
static const int EDGE_RADIUS = 18;
static const int FOOTER_METRICS_H = 98;

static CursorStatus last_status;
static unsigned long last_active_ms = 0;
static bool screensaver_active = false;
static bool blink_on = true;
static unsigned long last_blink_ms = 0;
static uint8_t applied_backlight = 255;
static lv_display_t *main_display = nullptr;

static lv_obj_t *frame_outer;
static lv_obj_t *panel_content;
static lv_obj_t *lbl_title;
static lv_obj_t *lbl_project;
static lv_obj_t *lbl_status_edge;
static lv_obj_t *panel_status_banner;
static lv_obj_t *lbl_status_banner;
static lv_obj_t *lbl_detail;
static lv_obj_t *lbl_model;
static lv_obj_t *bar_on_demand;
static lv_obj_t *lbl_on_demand;
static lv_obj_t *bar_tokens;
static lv_obj_t *lbl_tokens;
static lv_obj_t *dot_bridge;
static lv_obj_t *arc_sleep_countdown;
static unsigned long last_countdown_draw_ms = 0;
static int last_countdown_sec = -1;

struct StatusTheme {
  const char *label;
  lv_color_t edge;
  lv_color_t edge_alt;
  lv_color_t banner_bg;
  lv_color_t banner_bg_alt;
  lv_color_t text;
  bool blink;
};

static StatusTheme themeFor(const String &status) {
  if (status == "thinking") {
    return {"THINKING", lv_color_hex(0x22C55E), lv_color_hex(0x166534), lv_color_hex(0x14532D),
            lv_color_hex(0x166534), lv_color_hex(0xBBF7D0), false};
  }
  if (status == "running_tool") {
    return {"TOOL", lv_color_hex(0xEAB308), lv_color_hex(0x854D0E), lv_color_hex(0x713F12),
            lv_color_hex(0x854D0E), lv_color_hex(0xFEF08A), false};
  }
  if (status == "running") {
    return {"RUNNING", lv_color_hex(0x22C55E), lv_color_hex(0x15803D), lv_color_hex(0x166534),
            lv_color_hex(0x15803D), lv_color_hex(0xBBF7D0), false};
  }
  if (status == "awaiting_confirm") {
    return {"CONFIRM", lv_color_hex(0xF97316), lv_color_hex(0x9A3412), lv_color_hex(0xEA580C),
            lv_color_hex(0x9A3412), lv_color_hex(0xFFFFFF), true};
  }
  if (status == "error") {
    return {"ERROR", lv_color_hex(0xEF4444), lv_color_hex(0x7F1D1D), lv_color_hex(0xB91C1C),
            lv_color_hex(0x7F1D1D), lv_color_hex(0xFECACA), true};
  }
  if (status == "offline") {
    return {"OFFLINE", lv_color_hex(0x64748B), lv_color_hex(0x334155), lv_color_hex(0x334155),
            lv_color_hex(0x1E293B), lv_color_hex(0xCBD5E1), false};
  }
  if (status == "idle") {
    return {"IDLE", lv_color_hex(0x3B82F6), lv_color_hex(0x1E40AF), lv_color_hex(0x1E3A8A),
            lv_color_hex(0x1E40AF), lv_color_hex(0xBFDBFE), false};
  }
  return {"READY", lv_color_hex(0x475569), lv_color_hex(0x334155), lv_color_hex(0x1E293B),
          lv_color_hex(0x334155), lv_color_hex(0xE2E8F0), false};
}

static const int DEFAULT_CONTEXT_WINDOW = 200000;

static int tokenBarPercent(int tokens, int windowSize) {
  if (tokens <= 0) {
    return 0;
  }
  const int scale = windowSize > 0 ? windowSize : DEFAULT_CONTEXT_WINDOW;
  long pct = (static_cast<long>(tokens) * 100L) / scale;
  if (pct > 100) {
    pct = 100;
  }
  return static_cast<int>(pct);
}

static void formatTokenCompact(int tokens, char *buf, size_t len) {
  if (tokens <= 0) {
    snprintf(buf, len, "0");
    return;
  }
  if (tokens < 1000) {
    snprintf(buf, len, "%d", tokens);
    return;
  }
  snprintf(buf, len, "%.1fK", static_cast<double>(tokens) / 1000.0);
}

static int onDemandUsedBarPercent(int usedCents, int limitCents) {
  if (limitCents <= 0 || usedCents < 0) {
    return 0;
  }
  long pct = (static_cast<long>(usedCents) * 100L) / limitCents;
  if (pct > 100) {
    pct = 100;
  }
  return static_cast<int>(pct);
}

static void formatUsdFromCents(char *buf, size_t len, int cents) {
  const int dollars = cents / 100;
  const int frac = cents >= 0 ? cents % 100 : (-cents) % 100;
  snprintf(buf, len, "$%d.%02d", dollars, frac);
}

static int innerCornerRadius(void) {
  const int r = EDGE_RADIUS - EDGE_THICK;
  return r > 4 ? r : 4;
}

static void style_metrics_footer(lv_obj_t *obj) {
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void apply_status_theme(const StatusTheme &theme, bool is_confirm) {
  const lv_color_t edge = (theme.blink && !blink_on) ? theme.edge_alt : theme.edge;
  const lv_color_t banner = (theme.blink && !blink_on) ? theme.banner_bg_alt : theme.banner_bg;

  lv_obj_set_style_bg_color(frame_outer, edge, 0);
  lv_obj_set_style_bg_color(panel_status_banner, banner, 0);

  const int pad_bottom = is_confirm ? EDGE_THICK_CONFIRM : EDGE_THICK;
  lv_obj_set_style_pad_top(frame_outer, EDGE_THICK, 0);
  lv_obj_set_style_pad_left(frame_outer, EDGE_THICK, 0);
  lv_obj_set_style_pad_right(frame_outer, EDGE_THICK, 0);
  lv_obj_set_style_pad_bottom(frame_outer, pad_bottom, 0);

  lv_label_set_text(lbl_status_edge, theme.label);
  lv_label_set_text(lbl_status_banner, theme.label);
  lv_obj_set_style_text_color(lbl_status_edge, theme.text, 0);
  lv_obj_set_style_text_color(lbl_status_banner, theme.text, 0);
}

static bool status_blocks_screensaver(const CursorStatus &status) {
  const String &st = status.agentStatus;
  return st == "thinking" || st == "running_tool" || st == "awaiting_confirm" ||
         st == "error";
}

static bool status_should_wake_screen(const CursorStatus &next, const CursorStatus &prev) {
  if (next.agentStatus != prev.agentStatus) {
    return true;
  }
  if (next.bridgeReachable != prev.bridgeReachable) {
    return true;
  }
  if (status_blocks_screensaver(next)) {
    if (next.agentDetail != prev.agentDetail) {
      return true;
    }
    if (next.cursorOnline != prev.cursorOnline) {
      return true;
    }
  }
  return false;
}

static void apply_backlight(uint8_t level) {
  if (applied_backlight == level) {
    return;
  }
  applied_backlight = level;
  Set_Backlight(level);
}

static void update_detail_scroll_mode(const CursorStatus &status) {
  if (status_blocks_screensaver(status)) {
    lv_label_set_long_mode(lbl_detail, LV_LABEL_LONG_SCROLL_CIRCULAR);
  } else {
    lv_label_set_long_mode(lbl_detail, LV_LABEL_LONG_CLIP);
  }
}

static bool status_visual_changed(const CursorStatus &next, const CursorStatus &prev) {
  return next.agentStatus != prev.agentStatus || next.agentDetail != prev.agentDetail ||
         next.project != prev.project || next.model != prev.model ||
         next.bridgeReachable != prev.bridgeReachable || next.cursorOnline != prev.cursorOnline ||
         next.contextTokens != prev.contextTokens || next.contextWindowSize != prev.contextWindowSize ||
         next.onDemandUsedCents != prev.onDemandUsedCents ||
         next.onDemandLimitCents != prev.onDemandLimitCents ||
         next.onDemandEnabled != prev.onDemandEnabled ||
         next.onDemandUnlimited != prev.onDemandUnlimited || next.onDemandError != prev.onDemandError;
}

static void style_sleep_arc_indicator(int remain_sec) {
  lv_color_t color = lv_color_hex(0x64748B);
  if (remain_sec <= 10) {
    color = lv_color_hex(0xF59E0B);
  }
  if (remain_sec <= 5) {
    color = lv_color_hex(0xEF4444);
  }
  lv_obj_set_style_arc_color(arc_sleep_countdown, color, LV_PART_INDICATOR);
}

static void update_sleep_countdown(void) {
  if (screensaver_active || status_blocks_screensaver(last_status)) {
    lv_obj_add_flag(arc_sleep_countdown, LV_OBJ_FLAG_HIDDEN);
    last_countdown_sec = -1;
    return;
  }

  const unsigned long elapsed = millis() - last_active_ms;
  if (elapsed >= SCREENSAVER_TIMEOUT_MS) {
    lv_obj_add_flag(arc_sleep_countdown, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  const unsigned long remain_ms = SCREENSAVER_TIMEOUT_MS - elapsed;
  const int remain_sec =
      static_cast<int>((remain_ms + 999) / 1000);
  if (remain_sec != last_countdown_sec) {
    lv_arc_set_value(arc_sleep_countdown, remain_sec);
    style_sleep_arc_indicator(remain_sec);
    last_countdown_sec = remain_sec;
  }
  lv_obj_remove_flag(arc_sleep_countdown, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(arc_sleep_countdown);
}

static void screensaver_enter(void) {
  if (screensaver_active) {
    return;
  }
  screensaver_active = true;
  lv_obj_add_flag(arc_sleep_countdown, LV_OBJ_FLAG_HIDDEN);
  last_countdown_sec = -1;
  Lvgl_SetPaused(true);
  lv_label_set_long_mode(lbl_detail, LV_LABEL_LONG_CLIP);
  lv_obj_add_flag(frame_outer, LV_OBJ_FLAG_HIDDEN);
  if (main_display) {
    lv_display_enable_invalidation(main_display, false);
  }
  applied_backlight = 255;
  apply_backlight(0);
  rgb_sleep();
}

static void screensaver_exit(void) {
  last_active_ms = millis();
  if (!screensaver_active) {
    return;
  }
  screensaver_active = false;
  Lvgl_SetPaused(false);
  if (main_display) {
    lv_display_enable_invalidation(main_display, true);
  }
  lv_obj_remove_flag(frame_outer, LV_OBJ_FLAG_HIDDEN);
  update_detail_scroll_mode(last_status);
  applied_backlight = 255;
  apply_backlight(NORMAL_BACKLIGHT);
  rgb_wake();
}

void ui_init(void) {
  last_active_ms = millis();
  main_display = lv_display_get_default();
  apply_backlight(NORMAL_BACKLIGHT);
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x020617), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);

  frame_outer = lv_obj_create(scr);
  lv_obj_set_size(frame_outer, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_align(frame_outer, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(frame_outer, EDGE_RADIUS, 0);
  lv_obj_set_style_border_width(frame_outer, 0, 0);
  lv_obj_set_style_pad_all(frame_outer, EDGE_THICK, 0);
  lv_obj_set_style_clip_corner(frame_outer, true, 0);
  lv_obj_clear_flag(frame_outer, LV_OBJ_FLAG_SCROLLABLE);

  lbl_status_edge = lv_label_create(frame_outer);
  lv_label_set_text(lbl_status_edge, "BOOT");
  lv_obj_set_style_text_color(lbl_status_edge, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_status_edge, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_status_edge, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_move_foreground(lbl_status_edge);

  panel_content = lv_obj_create(frame_outer);
  lv_obj_set_size(panel_content, LV_PCT(100), LV_PCT(100));
  lv_obj_align(panel_content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(panel_content, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_radius(panel_content, innerCornerRadius(), 0);
  lv_obj_set_style_border_width(panel_content, 0, 0);
  lv_obj_set_style_pad_all(panel_content, 6, 0);
  lv_obj_set_style_clip_corner(panel_content, true, 0);
  lv_obj_clear_flag(panel_content, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *header = lv_obj_create(panel_content);
  lv_obj_set_size(header, LV_PCT(100), 24);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  dot_bridge = lv_obj_create(header);
  lv_obj_set_size(dot_bridge, 10, 10);
  lv_obj_align(dot_bridge, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(dot_bridge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot_bridge, lv_color_hex(0x64748B), 0);
  lv_obj_set_style_border_width(dot_bridge, 0, 0);

  lbl_title = lv_label_create(header);
  lv_label_set_text(lbl_title, "Cursor");
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xF8FAFC), 0);
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 14, 0);

  arc_sleep_countdown = lv_arc_create(header);
  lv_obj_set_size(arc_sleep_countdown, SLEEP_ARC_SIZE, SLEEP_ARC_SIZE);
  lv_obj_align(arc_sleep_countdown, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_arc_set_range(arc_sleep_countdown, 0, SCREENSAVER_COUNTDOWN_SEC);
  lv_arc_set_value(arc_sleep_countdown, SCREENSAVER_COUNTDOWN_SEC);
  lv_arc_set_rotation(arc_sleep_countdown, 270);
  lv_arc_set_bg_angles(arc_sleep_countdown, 0, 360);
  lv_obj_remove_flag(arc_sleep_countdown, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc_sleep_countdown, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc_sleep_countdown, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc_sleep_countdown, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc_sleep_countdown, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc_sleep_countdown, 3, LV_PART_INDICATOR);
  lv_obj_set_style_opa(arc_sleep_countdown, LV_OPA_TRANSP, LV_PART_KNOB);
  style_sleep_arc_indicator(SCREENSAVER_COUNTDOWN_SEC);
  lv_obj_add_flag(arc_sleep_countdown, LV_OBJ_FLAG_HIDDEN);

  lbl_project = lv_label_create(panel_content);
  lv_label_set_text(lbl_project, "-");
  lv_obj_set_style_text_color(lbl_project, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_style_text_font(lbl_project, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_project, LV_ALIGN_TOP_LEFT, 2, 28);
  lv_obj_set_width(lbl_project, LV_PCT(98));

  panel_status_banner = lv_obj_create(panel_content);
  lv_obj_set_size(panel_status_banner, LV_PCT(96), 40);
  lv_obj_align(panel_status_banner, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_set_style_radius(panel_status_banner, 8, 0);
  lv_obj_set_style_border_width(panel_status_banner, 2, 0);
  lv_obj_set_style_border_color(panel_status_banner, lv_color_hex(0xF8FAFC), 0);
  lv_obj_set_style_border_opa(panel_status_banner, LV_OPA_40, 0);
  lv_obj_set_style_pad_all(panel_status_banner, 0, 0);
  lv_obj_clear_flag(panel_status_banner, LV_OBJ_FLAG_SCROLLABLE);

  lbl_status_banner = lv_label_create(panel_status_banner);
  lv_label_set_text(lbl_status_banner, "BOOT");
  lv_obj_set_style_text_color(lbl_status_banner, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_status_banner, &lv_font_montserrat_20, 0);
  lv_obj_center(lbl_status_banner);

  lbl_detail = lv_label_create(panel_content);
  lv_label_set_text(lbl_detail, "Connecting...");
  lv_obj_set_style_text_color(lbl_detail, lv_color_hex(0xE2E8F0), 0);
  lv_obj_set_style_text_font(lbl_detail, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_detail, LV_ALIGN_TOP_LEFT, 2, 96);
  lv_obj_set_width(lbl_detail, LV_PCT(98));
  lv_obj_set_height(lbl_detail, LCD_HEIGHT - EDGE_THICK * 2 - 12 - 96 - FOOTER_METRICS_H - 22);
  lv_label_set_long_mode(lbl_detail, LV_LABEL_LONG_CLIP);

  lbl_model = lv_label_create(panel_content);
  lv_label_set_text(lbl_model, "Model: -");
  lv_obj_set_style_text_color(lbl_model, lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_font(lbl_model, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_model, LV_ALIGN_BOTTOM_LEFT, 2, -(FOOTER_METRICS_H + 2));

  lv_obj_t *footer_metrics = lv_obj_create(panel_content);
  lv_obj_set_size(footer_metrics, LV_PCT(100), FOOTER_METRICS_H);
  lv_obj_align(footer_metrics, LV_ALIGN_BOTTOM_MID, 0, 0);
  style_metrics_footer(footer_metrics);
  lv_obj_set_flex_flow(footer_metrics, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(footer_metrics, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(footer_metrics, 3, 0);
  lv_obj_set_style_pad_left(footer_metrics, 2, 0);

  lv_obj_t *od_title = lv_label_create(footer_metrics);
  lv_label_set_text(od_title, "On-Demand");
  lv_obj_set_style_text_color(od_title, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_style_text_font(od_title, &lv_font_montserrat_14, 0);
  lv_obj_set_width(od_title, LV_PCT(100));

  lbl_on_demand = lv_label_create(footer_metrics);
  lv_label_set_text(lbl_on_demand, "--");
  lv_obj_set_style_text_color(lbl_on_demand, lv_color_hex(0xFDE68A), 0);
  lv_obj_set_style_text_font(lbl_on_demand, &lv_font_montserrat_14, 0);
  lv_obj_set_width(lbl_on_demand, LV_PCT(100));

  bar_on_demand = lv_bar_create(footer_metrics);
  lv_obj_set_size(bar_on_demand, LV_PCT(98), 5);
  lv_bar_set_range(bar_on_demand, 0, 100);
  lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);

  lv_obj_t *metrics_spacer = lv_obj_create(footer_metrics);
  lv_obj_set_size(metrics_spacer, LV_PCT(100), 4);
  style_metrics_footer(metrics_spacer);

  lv_obj_t *token_title = lv_label_create(footer_metrics);
  lv_label_set_text(token_title, "Context");
  lv_obj_set_style_text_color(token_title, lv_color_hex(0x64748B), 0);
  lv_obj_set_style_text_font(token_title, &lv_font_montserrat_14, 0);
  lv_obj_set_width(token_title, LV_PCT(100));

  bar_tokens = lv_bar_create(footer_metrics);
  lv_obj_set_size(bar_tokens, LV_PCT(98), 5);
  lv_bar_set_range(bar_tokens, 0, 100);
  lv_bar_set_value(bar_tokens, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_tokens, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_tokens, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);

  lbl_tokens = lv_label_create(footer_metrics);
  lv_label_set_text(lbl_tokens, "0");
  lv_obj_set_style_text_color(lbl_tokens, lv_color_hex(0xE2E8F0), 0);
  lv_obj_set_style_text_font(lbl_tokens, &lv_font_montserrat_14, 0);
  lv_obj_set_width(lbl_tokens, LV_PCT(100));
  lv_label_set_long_mode(lbl_tokens, LV_LABEL_LONG_DOT);
}

static void update_on_demand_ui(const CursorStatus &status) {
  if (status.onDemandError.length() && status.onDemandError != "not_fetched") {
    lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
    lv_label_set_text(lbl_on_demand, "OD: unavailable");
    lv_obj_set_style_text_color(lbl_on_demand, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0x475569), LV_PART_INDICATOR);
    return;
  }

  if (!status.onDemandEnabled && status.onDemandUsedCents < 0 && status.onDemandLimitCents < 0) {
    lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
    lv_label_set_text(lbl_on_demand, "OD: loading...");
    lv_obj_set_style_text_color(lbl_on_demand, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    return;
  }

  int usedCents = status.onDemandUsedCents;
  const int limitCents = status.onDemandLimitCents;
  if (usedCents < 0 && limitCents > 0 && status.onDemandRemainingCents >= 0) {
    usedCents = limitCents - status.onDemandRemainingCents;
    if (usedCents < 0) {
      usedCents = 0;
    }
  }

  char odLine[40];
  if (status.onDemandUnlimited || limitCents <= 0) {
    if (usedCents >= 0) {
      char usd[16];
      formatUsdFromCents(usd, sizeof(usd), usedCents);
      snprintf(odLine, sizeof(odLine), "%s used", usd);
    } else {
      snprintf(odLine, sizeof(odLine), "Unlimited");
    }
    lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
  } else {
    char used[16];
    char limit[16];
    formatUsdFromCents(used, sizeof(used), usedCents >= 0 ? usedCents : 0);
    formatUsdFromCents(limit, sizeof(limit), limitCents);
    snprintf(odLine, sizeof(odLine), "%s / %s", used, limit);
    const int pct = onDemandUsedBarPercent(usedCents >= 0 ? usedCents : 0, limitCents);
    lv_bar_set_value(bar_on_demand, pct, LV_ANIM_OFF);
  }

  lv_label_set_text(lbl_on_demand, odLine);
  lv_color_t od_text = lv_color_hex(0xFDE68A);
  lv_color_t od_bar = lv_color_hex(0xF59E0B);
  if (!status.onDemandUnlimited && limitCents > 0 && usedCents >= 0 &&
      usedCents * 100 / limitCents >= 80) {
    od_text = lv_color_hex(0xFCA5A5);
    od_bar = lv_color_hex(0xEF4444);
  }
  lv_obj_set_style_text_color(lbl_on_demand, od_text, 0);
  lv_obj_set_style_bg_color(bar_on_demand, od_bar, LV_PART_INDICATOR);
}

void ui_update(const CursorStatus &status) {
  const bool wake_screen =
      screensaver_active &&
      (status_should_wake_screen(status, last_status) || status_blocks_screensaver(status));

  if (wake_screen) {
    screensaver_exit();
  }
  if (status_blocks_screensaver(status)) {
    screensaver_exit();
  }

  const bool visual_changed = status_visual_changed(status, last_status);
  last_status = status;

  if (screensaver_active) {
    return;
  }
  if (!visual_changed) {
    return;
  }

  update_detail_scroll_mode(status);

  lv_color_t dot = lv_color_hex(0xEF4444);
  if (status.bridgeReachable) {
    dot = status.cursorOnline ? lv_color_hex(0x22C55E) : lv_color_hex(0xEAB308);
  }
  lv_obj_set_style_bg_color(dot_bridge, dot, 0);

  StatusTheme theme = themeFor(status.agentStatus);
  if (status.agentStatus == "offline") {
    theme = themeFor("offline");
  }

  const bool is_confirm =
      status.agentStatus == "awaiting_confirm" && status.bridgeReachable;
  apply_status_theme(theme, is_confirm);

  if (is_confirm) {
    lv_obj_set_style_text_color(lbl_detail, lv_color_hex(0xFED7AA), 0);
  } else {
    lv_obj_set_style_text_color(lbl_detail, lv_color_hex(0xE2E8F0), 0);
  }

  lv_label_set_text(lbl_project, status.project.c_str());
  lv_label_set_text(lbl_detail, status.agentDetail.c_str());

  String modelName = status.model.length() ? status.model : String("-");
  if (modelName.equalsIgnoreCase("default")) {
    modelName = "Auto";
  }
  String modelLine = String("Model: ") + modelName;
  lv_label_set_text(lbl_model, modelLine.c_str());

  update_on_demand_ui(status);

  const int pct = tokenBarPercent(status.contextTokens, status.contextWindowSize);
  lv_bar_set_value(bar_tokens, pct, LV_ANIM_OFF);

  char tokenLine[24];
  formatTokenCompact(status.contextTokens, tokenLine, sizeof(tokenLine));
  lv_label_set_text(lbl_tokens, tokenLine);
}

void ui_blink_tick(void) {
  if (screensaver_active) {
    return;
  }

  StatusTheme theme = themeFor(last_status.agentStatus);
  if (last_status.agentStatus == "offline") {
    theme = themeFor("offline");
  }
  if (!theme.blink) {
    return;
  }

  const unsigned long now = millis();
  if (now - last_blink_ms < BLINK_INTERVAL_MS) {
    return;
  }
  last_blink_ms = now;
  blink_on = !blink_on;

  apply_status_theme(theme, last_status.agentStatus == "awaiting_confirm");
}

bool ui_is_screensaver(void) {
  return screensaver_active;
}

void ui_countdown_tick(void) {
  if (screensaver_active) {
    return;
  }
  const unsigned long now = millis();
  if (now - last_countdown_draw_ms < 1000) {
    return;
  }
  last_countdown_draw_ms = now;
  update_sleep_countdown();
}

void ui_screensaver_tick(void) {
  if (screensaver_active) {
    return;
  }
  if (status_blocks_screensaver(last_status)) {
    last_active_ms = millis();
    update_sleep_countdown();
    return;
  }
  update_sleep_countdown();
  if (millis() - last_active_ms >= SCREENSAVER_TIMEOUT_MS) {
    screensaver_enter();
  }
}
