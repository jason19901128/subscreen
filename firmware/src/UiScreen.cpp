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
static const int PANEL_PAD = 5;
static const int PANEL_ROW_GAP = 3;
static const int HEADER_H = 24;
static const int PROJECT_H = 20;
static const int BANNER_H = 50;
static const int DETAIL_LINE_H = 20;
static const int DETAIL_MAX_LINES = 4;
static const int DETAIL_MAX_H = DETAIL_LINE_H * DETAIL_MAX_LINES;
static const int FOOTER_METRICS_H = 78;
static const int FOOTER_LABEL_H = 20;
static const int FOOTER_BAR_H = 5;
static const int FOOTER_CTX_H = 20;

static const lv_font_t *FONT_BODY = &lv_font_montserrat_16;
static const lv_font_t *FONT_BANNER = &lv_font_montserrat_24;
static const lv_font_t *FONT_EDGE = &lv_font_montserrat_14;

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
static lv_obj_t *bar_on_demand;
static lv_obj_t *bar_od;
static lv_obj_t *lbl_usage_plan;
static lv_obj_t *lbl_usage_od;
static lv_obj_t *bar_tokens;
static lv_obj_t *lbl_tokens;
static lv_obj_t *dot_bridge;
static lv_obj_t *arc_sleep_countdown;
static unsigned long last_countdown_draw_ms = 0;
static int last_countdown_sec = -1;
static String scroll_text_project;
static String scroll_text_detail;

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

static int usageBarPercent(int usedCents, int limitCents) {
  if (limitCents <= 0 || usedCents < 0) {
    return 0;
  }
  long pct = (static_cast<long>(usedCents) * 100L) / limitCents;
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

static void formatUsdFromCents(char *buf, size_t len, int cents) {
  const int dollars = cents / 100;
  const int frac = cents >= 0 ? cents % 100 : (-cents) % 100;
  if (frac == 0) {
    snprintf(buf, len, "$%d", dollars);
  } else {
    snprintf(buf, len, "$%d.%02d", dollars, frac);
  }
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

static void style_compact_label(lv_obj_t *label) {
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_set_style_text_line_space(label, 0, 0);
}

static bool label_text_overflows_x(lv_obj_t *label, const char *text) {
  if (label == nullptr || text == nullptr || text[0] == '\0') {
    return false;
  }
  const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  if (font == nullptr) {
    return false;
  }
  lv_point_t size;
  lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  const int32_t max_w = lv_obj_get_content_width(label);
  return max_w > 0 && size.x > max_w;
}

static void update_label_scroll_mode(lv_obj_t *label, const char *text, String &cache,
                                     bool allow_scroll) {
  if (label == nullptr) {
    return;
  }
  const String next_text = text ? String(text) : String("");
  const bool overflow = allow_scroll && label_text_overflows_x(label, text);
  const lv_label_long_mode_t want =
      overflow ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
  if (cache == next_text && lv_label_get_long_mode(label) == want) {
    return;
  }
  cache = next_text;

  if (lv_label_get_long_mode(label) != want) {
    lv_label_set_long_mode(label, want);
  }
  if (overflow) {
    lv_obj_set_style_anim_duration(label, 14000, LV_PART_MAIN);
  }
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

static bool detail_implies_idle(const String &detail) {
  return detail == "Waiting for Cursor" || detail == "Task completed" ||
         detail == "Task aborted" || detail == "Session ended" || detail == "Ready";
}

static bool status_is_idle_like(const String &status) {
  return status == "idle" || status == "offline";
}

static bool status_substantively_busy(const CursorStatus &status) {
  if (!status_blocks_screensaver(status)) {
    return false;
  }
  if ((status.agentStatus == "thinking" || status.agentStatus == "running") &&
      detail_implies_idle(status.agentDetail)) {
    return false;
  }
  return true;
}

static bool status_should_wake_screen(const CursorStatus &next, const CursorStatus &prev) {
  const bool next_busy = status_substantively_busy(next);
  const bool prev_busy = status_substantively_busy(prev);

  if (next_busy && !prev_busy) {
    return true;
  }
  if (next_busy) {
    if (next.agentStatus != prev.agentStatus) {
      return true;
    }
    if (next.agentDetail != prev.agentDetail) {
      return true;
    }
    if (next.cursorOnline != prev.cursorOnline) {
      return true;
    }
    return false;
  }

  if (next.agentStatus != prev.agentStatus) {
    if (status_is_idle_like(prev.agentStatus) && status_is_idle_like(next.agentStatus)) {
      return false;
    }
    return true;
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
  const int detail_h =
      status_blocks_screensaver(status) ? DETAIL_MAX_H : DETAIL_LINE_H * 2;
  lv_obj_set_height(lbl_detail, detail_h);
}

static bool status_visual_changed(const CursorStatus &next, const CursorStatus &prev) {
  return next.agentStatus != prev.agentStatus || next.agentDetail != prev.agentDetail ||
         next.project != prev.project || next.model != prev.model ||
         next.bridgeReachable != prev.bridgeReachable || next.cursorOnline != prev.cursorOnline ||
         next.contextTokens != prev.contextTokens || next.contextWindowSize != prev.contextWindowSize ||
         next.onDemandUsedCents != prev.onDemandUsedCents ||
         next.onDemandLimitCents != prev.onDemandLimitCents ||
         next.onDemandEnabled != prev.onDemandEnabled ||
         next.onDemandUnlimited != prev.onDemandUnlimited || next.onDemandError != prev.onDemandError ||
         next.planLimitCents != prev.planLimitCents ||
         next.planIncludedSpendCents != prev.planIncludedSpendCents ||
         next.planRemainingCents != prev.planRemainingCents ||
         next.planPercentUsed != prev.planPercentUsed;
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
  lv_obj_set_style_text_font(lbl_status_edge, FONT_EDGE, 0);
  lv_obj_align(lbl_status_edge, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_move_foreground(lbl_status_edge);

  panel_content = lv_obj_create(frame_outer);
  lv_obj_set_size(panel_content, LV_PCT(100), LV_PCT(100));
  lv_obj_align(panel_content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(panel_content, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_radius(panel_content, innerCornerRadius(), 0);
  lv_obj_set_style_border_width(panel_content, 0, 0);
  lv_obj_set_style_pad_all(panel_content, PANEL_PAD, 0);
  lv_obj_set_style_pad_row(panel_content, PANEL_ROW_GAP, 0);
  lv_obj_set_style_clip_corner(panel_content, true, 0);
  lv_obj_remove_flag(panel_content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_clear_flag(panel_content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(panel_content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t *header = lv_obj_create(panel_content);
  lv_obj_set_size(header, LV_PCT(100), HEADER_H);
  lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  dot_bridge = lv_obj_create(header);
  lv_obj_set_size(dot_bridge, 12, 12);
  lv_obj_align(dot_bridge, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(dot_bridge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot_bridge, lv_color_hex(0x64748B), 0);
  lv_obj_set_style_border_width(dot_bridge, 0, 0);

  lbl_title = lv_label_create(header);
  lv_label_set_text(lbl_title, "Cursor");
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xF8FAFC), 0);
  lv_obj_set_style_text_font(lbl_title, FONT_BODY, 0);
  lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 16, 0);

  arc_sleep_countdown = lv_arc_create(header);
  lv_obj_set_size(arc_sleep_countdown, 20, 20);
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
  lv_obj_set_style_text_font(lbl_project, FONT_BODY, 0);
  lv_obj_set_size(lbl_project, LV_PCT(100), PROJECT_H);
  style_compact_label(lbl_project);
  lv_label_set_long_mode(lbl_project, LV_LABEL_LONG_CLIP);

  panel_status_banner = lv_obj_create(panel_content);
  lv_obj_set_size(panel_status_banner, LV_PCT(96), BANNER_H);
  lv_obj_set_style_align(panel_status_banner, LV_ALIGN_CENTER, 0);
  lv_obj_set_style_radius(panel_status_banner, 8, 0);
  lv_obj_set_style_border_width(panel_status_banner, 2, 0);
  lv_obj_set_style_border_color(panel_status_banner, lv_color_hex(0xF8FAFC), 0);
  lv_obj_set_style_border_opa(panel_status_banner, LV_OPA_40, 0);
  lv_obj_set_style_pad_all(panel_status_banner, 0, 0);
  lv_obj_clear_flag(panel_status_banner, LV_OBJ_FLAG_SCROLLABLE);

  lbl_status_banner = lv_label_create(panel_status_banner);
  lv_label_set_text(lbl_status_banner, "BOOT");
  lv_obj_set_style_text_color(lbl_status_banner, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_status_banner, FONT_BANNER, 0);
  lv_obj_center(lbl_status_banner);

  lbl_detail = lv_label_create(panel_content);
  lv_label_set_text(lbl_detail, "Connecting...");
  lv_obj_set_style_text_color(lbl_detail, lv_color_hex(0xE2E8F0), 0);
  lv_obj_set_style_text_font(lbl_detail, FONT_BODY, 0);
  lv_obj_set_size(lbl_detail, LV_PCT(100), DETAIL_LINE_H * 2);
  style_compact_label(lbl_detail);
  lv_label_set_long_mode(lbl_detail, LV_LABEL_LONG_CLIP);

  lv_obj_t *panel_bottom_spacer = lv_obj_create(panel_content);
  lv_obj_set_width(panel_bottom_spacer, LV_PCT(100));
  lv_obj_set_flex_grow(panel_bottom_spacer, 1);
  style_metrics_footer(panel_bottom_spacer);

  lv_obj_t *footer_metrics = lv_obj_create(panel_content);
  lv_obj_set_size(footer_metrics, LV_PCT(100), FOOTER_METRICS_H);
  style_metrics_footer(footer_metrics);
  lv_obj_remove_flag(footer_metrics, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_flex_flow(footer_metrics, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(footer_metrics, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(footer_metrics, 2, 0);

  lbl_usage_plan = lv_label_create(footer_metrics);
  lv_label_set_text(lbl_usage_plan, "Plan: --");
  lv_obj_set_style_text_color(lbl_usage_plan, lv_color_hex(0xFDE68A), 0);
  lv_obj_set_style_text_font(lbl_usage_plan, FONT_BODY, 0);
  lv_obj_set_size(lbl_usage_plan, LV_PCT(100), FOOTER_LABEL_H);
  style_compact_label(lbl_usage_plan);
  lv_label_set_long_mode(lbl_usage_plan, LV_LABEL_LONG_DOT);

  bar_on_demand = lv_bar_create(footer_metrics);
  lv_obj_set_size(bar_on_demand, LV_PCT(100), FOOTER_BAR_H);
  lv_bar_set_range(bar_on_demand, 0, 100);
  lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar_on_demand, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(bar_on_demand, 1, LV_PART_INDICATOR);

  lbl_usage_od = lv_label_create(footer_metrics);
  lv_label_set_text(lbl_usage_od, "OD: --");
  lv_obj_set_style_text_color(lbl_usage_od, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_style_text_font(lbl_usage_od, FONT_BODY, 0);
  lv_obj_set_size(lbl_usage_od, LV_PCT(100), FOOTER_LABEL_H);
  style_compact_label(lbl_usage_od);
  lv_label_set_long_mode(lbl_usage_od, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(lbl_usage_od, LV_OBJ_FLAG_HIDDEN);

  bar_od = lv_bar_create(footer_metrics);
  lv_obj_set_size(bar_od, LV_PCT(100), FOOTER_BAR_H);
  lv_bar_set_range(bar_od, 0, 100);
  lv_bar_set_value(bar_od, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_od, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_od, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar_od, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(bar_od, 1, LV_PART_INDICATOR);
  lv_obj_add_flag(bar_od, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *ctx_row = lv_obj_create(footer_metrics);
  lv_obj_set_size(ctx_row, LV_PCT(100), FOOTER_CTX_H);
  style_metrics_footer(ctx_row);
  lv_obj_set_flex_flow(ctx_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctx_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ctx_row, 4, 0);

  bar_tokens = lv_bar_create(ctx_row);
  lv_obj_set_height(bar_tokens, FOOTER_BAR_H);
  lv_obj_set_flex_grow(bar_tokens, 1);
  lv_bar_set_range(bar_tokens, 0, 100);
  lv_bar_set_value(bar_tokens, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_tokens, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_tokens, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar_tokens, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(bar_tokens, 1, LV_PART_INDICATOR);

  lbl_tokens = lv_label_create(ctx_row);
  lv_label_set_text(lbl_tokens, "0");
  lv_obj_set_style_text_color(lbl_tokens, lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_font(lbl_tokens, FONT_BODY, 0);
  lv_obj_set_size(lbl_tokens, 44, FOOTER_CTX_H);
  style_compact_label(lbl_tokens);
  lv_label_set_long_mode(lbl_tokens, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(lbl_tokens, LV_TEXT_ALIGN_RIGHT, 0);
}

static void update_on_demand_ui(const CursorStatus &status) {
  lv_color_t plan_text = lv_color_hex(0xFDE68A);
  lv_color_t plan_bar = lv_color_hex(0xF59E0B);

  if (status.onDemandError.length() && status.onDemandError != "not_fetched") {
    lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
    lv_label_set_text(lbl_usage_plan, "Usage unavailable");
    lv_obj_set_style_text_color(lbl_usage_plan, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0x475569), LV_PART_INDICATOR);
    lv_obj_add_flag(lbl_usage_od, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar_od, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  const bool hasPlan = status.planLimitCents > 0;
  const bool hasOd = status.onDemandLimitCents > 0;

  if (!status.onDemandEnabled && !hasPlan && status.onDemandUsedCents < 0 &&
      status.onDemandLimitCents < 0) {
    lv_bar_set_value(bar_on_demand, 0, LV_ANIM_OFF);
    lv_label_set_text(lbl_usage_plan, "Loading usage...");
    lv_obj_set_style_text_color(lbl_usage_plan, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_bg_color(bar_on_demand, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    lv_obj_add_flag(lbl_usage_od, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar_od, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  char planLine[40] = "";
  char odLine[40] = "";
  int planBarPct = 0;
  int odBarPct = 0;
  bool showOd = false;
  bool showOdBar = false;

  if (hasPlan) {
    int planUsed = status.planIncludedSpendCents >= 0 ? status.planIncludedSpendCents : 0;
    if (planUsed <= 0 && status.planLimitCents > 0 && status.planRemainingCents >= 0) {
      planUsed = status.planLimitCents - status.planRemainingCents;
      if (planUsed < 0) {
        planUsed = 0;
      }
    }
    char used[16];
    char limit[16];
    formatUsdFromCents(used, sizeof(used), planUsed);
    formatUsdFromCents(limit, sizeof(limit), status.planLimitCents);
    snprintf(planLine, sizeof(planLine), "Plan: %s / %s", used, limit);
    planBarPct = usageBarPercent(planUsed, status.planLimitCents);
  }

  if (hasOd || status.onDemandUsedCents >= 0) {
    int usedCents = status.onDemandUsedCents;
    const int limitCents = status.onDemandLimitCents;
    if (usedCents < 0 && limitCents > 0 && status.onDemandRemainingCents >= 0) {
      usedCents = limitCents - status.onDemandRemainingCents;
      if (usedCents < 0) {
        usedCents = 0;
      }
    }
    if (status.onDemandUnlimited || limitCents <= 0) {
      if (usedCents >= 0) {
        char usd[16];
        formatUsdFromCents(usd, sizeof(usd), usedCents);
        snprintf(odLine, sizeof(odLine), "OD: %s used", usd);
      } else {
        snprintf(odLine, sizeof(odLine), "OD: unlimited");
      }
    } else {
      char used[16];
      char limit[16];
      formatUsdFromCents(used, sizeof(used), usedCents >= 0 ? usedCents : 0);
      formatUsdFromCents(limit, sizeof(limit), limitCents);
      snprintf(odLine, sizeof(odLine), "OD: %s / %s", used, limit);
      odBarPct = usageBarPercent(usedCents >= 0 ? usedCents : 0, limitCents);
      showOdBar = true;
    }
    showOd = true;
  }

  if (planLine[0] == '\0' && !showOd) {
    snprintf(planLine, sizeof(planLine), "Usage: --");
  } else if (planLine[0] == '\0' && showOd) {
    snprintf(planLine, sizeof(planLine), "Plan: n/a");
  }

  if (planBarPct >= 80) {
    plan_text = lv_color_hex(0xFCA5A5);
    plan_bar = lv_color_hex(0xEF4444);
  }

  lv_bar_set_value(bar_on_demand, planBarPct, LV_ANIM_OFF);
  lv_label_set_text(lbl_usage_plan, planLine);
  lv_obj_set_style_text_color(lbl_usage_plan, plan_text, 0);
  lv_obj_set_style_bg_color(bar_on_demand, plan_bar, LV_PART_INDICATOR);

  if (showOd) {
    lv_label_set_text(lbl_usage_od, odLine);
    lv_obj_remove_flag(lbl_usage_od, LV_OBJ_FLAG_HIDDEN);
    if (showOdBar) {
      lv_color_t od_bar = lv_color_hex(0x38BDF8);
      if (odBarPct >= 80) {
        od_bar = lv_color_hex(0xEF4444);
      }
      lv_bar_set_value(bar_od, odBarPct, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(bar_od, od_bar, LV_PART_INDICATOR);
      lv_obj_remove_flag(bar_od, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(bar_od, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_add_flag(lbl_usage_od, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar_od, LV_OBJ_FLAG_HIDDEN);
  }
}

void ui_update(const CursorStatus &status) {
  const bool wake_screen =
      screensaver_active &&
      (status_should_wake_screen(status, last_status) || status_substantively_busy(status));

  if (wake_screen) {
    screensaver_exit();
  }
  if (status_substantively_busy(status)) {
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

  String modelName = status.model.length() ? status.model : String("-");
  if (modelName.equalsIgnoreCase("default")) {
    modelName = "Auto";
  }
  String projectLine = status.project.length() ? status.project : String("-");
  if (modelName != "-") {
    projectLine += " · ";
    projectLine += modelName;
  }

  lv_label_set_text(lbl_project, projectLine.c_str());
  lv_label_set_text(lbl_detail, status.agentDetail.c_str());

  const bool detail_overflow = label_text_overflows_x(lbl_detail, status.agentDetail.c_str());
  update_label_scroll_mode(lbl_detail, status.agentDetail.c_str(), scroll_text_detail, true);
  update_label_scroll_mode(lbl_project, projectLine.c_str(), scroll_text_project, !detail_overflow);

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
  if (status_substantively_busy(last_status)) {
    last_active_ms = millis();
    update_sleep_countdown();
    return;
  }
  update_sleep_countdown();
  if (millis() - last_active_ms >= SCREENSAVER_TIMEOUT_MS) {
    screensaver_enter();
  }
}
