// UI: "big number" screen + "graph" screen
#pragma once

#include <lvgl.h>
#include <math.h>
#include "glucose.h"

#ifndef UI_W
#define UI_W 800
#define UI_H 480
#endif

// Layout is designed at 800x480; S() scales it for bigger panels (1280x720 -> 1.5x).
#if UI_H >= 720
#define S(v) ((v) * 3 / 2)
LV_FONT_DECLARE(font_glucose_360);
LV_FONT_DECLARE(font_glucose_72);
#define FONT_HUGE  (&font_glucose_360)
// main number is a bit larger than a straight 1.5x scale, so nudge it left and the arrow right
#define BIG_VALUE_X (-110)
#define BIG_VALUE_Y (-50)
#define BIG_ARROW_X 370
#define FONT_VALUE (&font_glucose_72)      // digits / '-' only
#define FONT_14    (&lv_font_montserrat_22)
#define FONT_20    (&lv_font_montserrat_30)
#define FONT_24    (&lv_font_montserrat_36)
#define FONT_28    (&lv_font_montserrat_42)
#elif UI_H < 240
// Small panels (T-Display-S3, 320x170): own layout in build_big/build_graph below, not a scaled copy.
#define UI_COMPACT 1
#define S(v) ((v) * 2 / 5)
LV_FONT_DECLARE(font_glucose_112);
#define FONT_HUGE  (&font_glucose_112)
#define FONT_VALUE (&lv_font_montserrat_28)
#define FONT_14    (&lv_font_montserrat_14)
#define FONT_20    (&lv_font_montserrat_20)
#define FONT_24    (&lv_font_montserrat_20)
#define FONT_28    (&lv_font_montserrat_20)
#else
#define S(v) (v)
LV_FONT_DECLARE(font_glucose_200);
#define FONT_HUGE  (&font_glucose_200)
#define BIG_VALUE_X (-60)
#define BIG_VALUE_Y (-30)
#define BIG_ARROW_X 230
#define FONT_VALUE (&lv_font_montserrat_48)
#define FONT_14    (&lv_font_montserrat_14)
#define FONT_20    (&lv_font_montserrat_20)
#define FONT_24    (&lv_font_montserrat_24)
#define FONT_28    (&lv_font_montserrat_28)
#endif

LV_FONT_DECLARE(font_status_icons);   // Font Awesome wifi / lock / home, 22 px

namespace ui {

// ---------- palette ----------
static const lv_color_t C_BG      = lv_color_hex(0x0B0F14);
static const lv_color_t C_PANEL   = lv_color_hex(0x161C24);
static const lv_color_t C_TEXT    = lv_color_hex(0xE6EDF3);
static const lv_color_t C_MUTED   = lv_color_hex(0x7D8590);
static const lv_color_t C_OK      = lv_color_hex(0x3FB950);
static const lv_color_t C_LOW     = lv_color_hex(0xF85149);
static const lv_color_t C_HIGH    = lv_color_hex(0xF0883E);
static const lv_color_t C_STALE   = lv_color_hex(0x6E7681);
static const lv_color_t C_ACCENT  = lv_color_hex(0x58A6FF);

static lv_color_t color_for(int v, bool stale) {
  if (stale) return C_STALE;
  if (v < GLUCOSE_LOW) return C_LOW;
  if (v > GLUCOSE_HIGH) return C_HIGH;
  return C_OK;
}

// ---------- trend arrow ----------
// Points are rotated in software rather than with LVGL style transforms: a transformed object is
// rendered through an ARGB8888 off-screen layer (size^2 * 4 bytes each), which at 1.5x scale
// exhausts LV_MEM_SIZE and LVGL 9.1 then waits forever for the allocation.
struct Arrow {
  lv_obj_t *line[2] = {nullptr, nullptr};   // second arrow for double up/down
  lv_point_precise_t pts[2][5];
  int size = 0;
};

static lv_obj_t *make_arrow_line(lv_obj_t *parent, int size) {
  lv_obj_t *l = lv_line_create(parent);
  lv_obj_set_size(l, size, size);
  lv_obj_set_style_line_width(l, size / 9, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  return l;
}

static Arrow make_arrow(lv_obj_t *parent, int size) {
  Arrow a;
  a.size = size;
  a.line[0] = make_arrow_line(parent, size);
  a.line[1] = make_arrow_line(parent, size);
  return a;
}

// arrow pointing at `deg` (clockwise, 0 = right), centred in a size x size box
static void arrow_points(lv_point_precise_t *out, int size, int deg) {
  const float s = size, h = s / 2;
  const float base[5][2] = {
      {s * 0.10f, h}, {s * 0.90f, h},
      {s * 0.55f, h - s * 0.35f}, {s * 0.90f, h}, {s * 0.55f, h + s * 0.35f}};
  const float r = deg * 3.14159265f / 180.0f, c = cosf(r), sn = sinf(r);
  for (int i = 0; i < 5; i++) {
    const float x = base[i][0] - h, y = base[i][1] - h;
    out[i].x = (lv_value_precise_t)(h + x * c - y * sn);
    out[i].y = (lv_value_precise_t)(h + x * sn + y * c);
  }
}

static void arrow_set(Arrow &a, Trend t, lv_color_t color) {
  int angle = 0;  // degrees, clockwise; 0 = right
  bool dbl = false, show = true;
  switch (t) {
    case TREND_DOUBLE_UP:   angle = -90; dbl = true; break;
    case TREND_SINGLE_UP:   angle = -90; break;
    case TREND_45_UP:       angle = -45; break;
    case TREND_FLAT:        angle = 0;   break;
    case TREND_45_DOWN:     angle = 45;  break;
    case TREND_SINGLE_DOWN: angle = 90;  break;
    case TREND_DOUBLE_DOWN: angle = 90;  dbl = true; break;
    default: show = false;
  }
  for (int i = 0; i < 2; i++) {
    arrow_points(a.pts[i], a.size, angle);
    lv_line_set_points(a.line[i], a.pts[i], 5);
    lv_obj_set_style_line_color(a.line[i], color, 0);
  }
  if (!show) { lv_obj_add_flag(a.line[0], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(a.line[1], LV_OBJ_FLAG_HIDDEN); return; }
  lv_obj_remove_flag(a.line[0], LV_OBJ_FLAG_HIDDEN);
  if (dbl) {
    lv_obj_remove_flag(a.line[1], LV_OBJ_FLAG_HIDDEN);
    int off = a.size * 0.28f;  // stack two arrows vertically
    lv_obj_align(a.line[0], LV_ALIGN_CENTER, 0, -off);
    lv_obj_align(a.line[1], LV_ALIGN_CENTER, 0, off);
  } else {
    lv_obj_add_flag(a.line[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(a.line[0], LV_ALIGN_CENTER, 0, 0);
  }
}

// ---------- status icons (top-right): WiFi, VPN, Home Assistant ----------
#define ICON_WIFI "\xEF\x87\xAB"   // U+F1EB
#define ICON_LOCK "\xEF\x80\xA3"   // U+F023
#define ICON_HOME "\xEF\x80\x95"   // U+F015
struct Indicators { lv_obj_t *row, *wifi, *vpn, *ha; };
static Indicators ind_big, ind_graph;

static Indicators make_indicators(lv_obj_t *parent) {
  Indicators in;
  in.row = lv_obj_create(parent);
  lv_obj_remove_style_all(in.row);
  lv_obj_set_size(in.row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(in.row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(in.row, S(10), 0);
  lv_obj_align(in.row, LV_ALIGN_TOP_RIGHT, S(-16), S(10));
  lv_obj_remove_flag(in.row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(in.row, LV_OBJ_FLAG_HIDDEN);   // shown once there is state to report
  lv_obj_t **icons[3] = {&in.wifi, &in.vpn, &in.ha};
  const char *glyphs[3] = {ICON_WIFI, ICON_LOCK, ICON_HOME};
  for (int i = 0; i < 3; i++) {
    *icons[i] = lv_label_create(in.row);
    lv_label_set_text(*icons[i], glyphs[i]);
    lv_obj_set_style_text_font(*icons[i], &font_status_icons, 0);
    lv_obj_set_style_text_color(*icons[i], C_STALE, 0);
  }
  return in;
}

// ---------- state ----------
static lv_obj_t *scr_big = nullptr;
static lv_obj_t *scr_graph = nullptr;

// big screen
static lv_obj_t *big_value, *big_unit, *big_age, *big_hint, *big_arrow_box, *big_status;
static Arrow big_arrow;

// graph screen
static lv_obj_t *g_value, *g_age, *g_arrow_box, *g_chart, *g_band, *g_stale, *g_status;
static Arrow g_arrow;
static lv_chart_series_t *g_series;
static lv_obj_t *g_win_btns[5];
static lv_obj_t *g_xlabels[3];
static const int WINDOWS[5] = {1, 3, 6, 12, 24};
static int g_window_idx = 1;  // 3h default
static int32_t g_data[24 * 12 + 1];

#if UI_COMPACT
static lv_obj_t *g_win_label;
static const int CHART_X = 34, CHART_Y = 36, CHART_W = UI_W - CHART_X - 8, CHART_H = UI_H - CHART_Y - 20;
#else
static const int CHART_X = S(70), CHART_Y = S(92), CHART_W = UI_W - CHART_X - S(20), CHART_H = S(300);
#endif

// ---------- helpers ----------
static void fmt_age(char *out, size_t n, uint32_t age_s) {
  if (age_s < 60) snprintf(out, n, "just now");
  else if (age_s < 3600) snprintf(out, n, "%lu min ago", (unsigned long)(age_s / 60));
  else snprintf(out, n, "%luh %lum ago", (unsigned long)(age_s / 3600), (unsigned long)((age_s % 3600) / 60));
}

static int chart_y_for(int v) {
  return CHART_H * (CHART_Y_MAX - v) / (CHART_Y_MAX - CHART_Y_MIN);
}

// instant screen switch: the slide animation redraws the full screen ~30x, which hammers PSRAM
static void goto_graph(lv_event_t *) { lv_screen_load(scr_graph); }
static void goto_big(lv_event_t *)   { lv_screen_load(scr_big); }

static void refresh_chart();

#if !UI_COMPACT
static void on_window_btn(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  g_window_idx = idx;
  for (int i = 0; i < 5; i++) {
    if (i == idx) lv_obj_add_state(g_win_btns[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(g_win_btns[i], LV_STATE_CHECKED);
  }
  refresh_chart();
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud, int w) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, S(56));
  lv_obj_set_style_bg_color(b, C_PANEL, 0);
  lv_obj_set_style_bg_color(b, C_ACCENT, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3441), LV_STATE_PRESSED);
  lv_obj_set_style_radius(b, S(10), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, FONT_24, 0);
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  lv_obj_center(l);
  return b;
}
#endif

// ---------- build ----------
// chart with the target band and threshold lines; shared by both layouts
static void setup_chart(lv_obj_t *parent, int line_w) {
  g_chart = lv_chart_create(parent);
  lv_obj_set_pos(g_chart, CHART_X, CHART_Y);
  lv_obj_set_size(g_chart, CHART_W, CHART_H);
  lv_obj_set_style_pad_all(g_chart, 0, 0);
  lv_obj_set_style_radius(g_chart, 0, 0);
  lv_obj_set_style_bg_color(g_chart, C_BG, 0);
  lv_obj_set_style_border_width(g_chart, 0, 0);
  lv_obj_set_style_line_color(g_chart, lv_color_hex(0x1F2733), 0);   // div lines
  lv_obj_set_style_line_width(g_chart, 1, 0);
  lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, CHART_Y_MIN, CHART_Y_MAX);
  lv_chart_set_div_line_count(g_chart, 0, 0);
  lv_obj_set_style_size(g_chart, 0, 0, LV_PART_INDICATOR);           // no dots
  lv_obj_set_style_line_width(g_chart, line_w, LV_PART_ITEMS);
  lv_obj_set_style_line_rounded(g_chart, true, LV_PART_ITEMS);
  lv_obj_remove_flag(g_chart, LV_OBJ_FLAG_SCROLLABLE);

  // target band 70..200 (drawn underneath the series)
  g_band = lv_obj_create(g_chart);
  lv_obj_remove_style_all(g_band);
  lv_obj_set_style_bg_color(g_band, C_OK, 0);
  lv_obj_set_style_bg_opa(g_band, LV_OPA_10, 0);
  lv_obj_set_pos(g_band, 0, chart_y_for(GLUCOSE_HIGH));
  lv_obj_set_size(g_band, CHART_W, chart_y_for(GLUCOSE_LOW) - chart_y_for(GLUCOSE_HIGH));
  lv_obj_move_background(g_band);

  // threshold lines
  const int yl[] = {GLUCOSE_HIGH, GLUCOSE_LOW};
  for (int v : yl) {
    lv_obj_t *ln = lv_obj_create(g_chart);
    lv_obj_remove_style_all(ln);
    lv_obj_set_style_bg_color(ln, v == GLUCOSE_HIGH ? C_HIGH : C_LOW, 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
    lv_obj_set_pos(ln, 0, chart_y_for(v));
    lv_obj_set_size(ln, CHART_W, line_w > 2 ? line_w / 2 : 1);
    lv_obj_move_background(ln);
  }
}

#if UI_COMPACT
static void build_big() {
  scr_big = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_big, C_BG, 0);
  lv_obj_remove_flag(scr_big, LV_OBJ_FLAG_SCROLLABLE);

  // number + arrow, centred together as a row
  lv_obj_t *row = lv_obj_create(scr_big);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, -6);

  big_value = lv_label_create(row);
  lv_obj_set_style_text_font(big_value, FONT_HUGE, 0);
  lv_obj_set_style_text_color(big_value, C_OK, 0);
  lv_label_set_text(big_value, "---");

  big_arrow_box = lv_obj_create(row);
  lv_obj_remove_style_all(big_arrow_box);
  lv_obj_set_size(big_arrow_box, 64, 80);
  big_arrow = make_arrow(big_arrow_box, 60);

  big_unit = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_unit, FONT_20, 0);
  lv_obj_set_style_text_color(big_unit, C_MUTED, 0);
  lv_label_set_text(big_unit, "mg/dL");
  lv_obj_align(big_unit, LV_ALIGN_BOTTOM_LEFT, 10, -6);

  big_age = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_age, FONT_20, 0);
  lv_obj_set_style_text_color(big_age, C_MUTED, 0);
  lv_label_set_text(big_age, "");
  lv_obj_align(big_age, LV_ALIGN_BOTTOM_RIGHT, -10, -6);

  big_status = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_status, FONT_14, 0);
  lv_obj_set_style_text_color(big_status, C_MUTED, 0);
  lv_obj_set_width(big_status, 200);
  lv_label_set_long_mode(big_status, LV_LABEL_LONG_DOT);
  lv_label_set_text(big_status, "");
  lv_obj_align(big_status, LV_ALIGN_TOP_LEFT, 8, 6);

  ind_big = make_indicators(scr_big);
}

static void build_graph() {
  scr_graph = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_graph, C_BG, 0);
  lv_obj_remove_flag(scr_graph, LV_OBJ_FLAG_SCROLLABLE);

  // ----- header: value, arrow, age ............ [!] window -----
  g_value = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_value, FONT_VALUE, 0);
  lv_obj_set_style_text_color(g_value, C_OK, 0);
  lv_label_set_text(g_value, "---");
  lv_obj_align(g_value, LV_ALIGN_TOP_LEFT, 8, 2);

  g_arrow_box = lv_obj_create(scr_graph);
  lv_obj_remove_style_all(g_arrow_box);
  lv_obj_set_size(g_arrow_box, 28, 28);
  lv_obj_align(g_arrow_box, LV_ALIGN_TOP_LEFT, 66, 2);
  g_arrow = make_arrow(g_arrow_box, 26);

  g_age = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_age, FONT_14, 0);
  lv_obj_set_style_text_color(g_age, C_MUTED, 0);
  lv_label_set_text(g_age, "");
  lv_obj_align(g_age, LV_ALIGN_TOP_LEFT, 100, 10);

  g_win_label = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_win_label, FONT_20, 0);
  lv_obj_set_style_text_color(g_win_label, C_ACCENT, 0);
  lv_obj_align(g_win_label, LV_ALIGN_TOP_RIGHT, -8, 6);

  g_stale = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_stale, FONT_20, 0);
  lv_obj_set_style_text_color(g_stale, C_LOW, 0);
  lv_label_set_text(g_stale, LV_SYMBOL_WARNING);
  lv_obj_align(g_stale, LV_ALIGN_TOP_RIGHT, -56, 6);
  lv_obj_add_flag(g_stale, LV_OBJ_FLAG_HIDDEN);

  g_status = lv_label_create(scr_graph);   // no room for it here; status shows on the big screen
  lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);

  // ----- chart -----
  setup_chart(scr_graph, 2);

  const int ylabels[] = {250, 200, 150, 70};
  for (int v : ylabels) {
    lv_obj_t *l = lv_label_create(scr_graph);
    lv_obj_set_style_text_font(l, FONT_14, 0);
    lv_obj_set_style_text_color(l, (v == GLUCOSE_HIGH) ? C_HIGH : (v == GLUCOSE_LOW) ? C_LOW : C_MUTED, 0);
    lv_label_set_text_fmt(l, "%d", v);
    lv_obj_set_width(l, CHART_X - 4);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l, 0, CHART_Y + chart_y_for(v) - 8);
  }
  for (int i = 0; i < 3; i++) {
    g_xlabels[i] = lv_label_create(scr_graph);
    lv_obj_set_style_text_font(g_xlabels[i], FONT_14, 0);
    lv_obj_set_style_text_color(g_xlabels[i], C_MUTED, 0);
  }
  g_series = lv_chart_add_series(g_chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

  ind_graph = ind_big;   // no room for status icons in the graph header; set_indicators just updates them twice
}
#else
static void build_big() {
  scr_big = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_big, C_BG, 0);
  lv_obj_add_flag(scr_big, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr_big, goto_graph, LV_EVENT_CLICKED, nullptr);

  big_value = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_value, FONT_HUGE, 0);
  lv_obj_set_style_text_color(big_value, C_OK, 0);
  lv_label_set_text(big_value, "---");
  lv_obj_align(big_value, LV_ALIGN_CENTER, BIG_VALUE_X, BIG_VALUE_Y);

  big_arrow_box = lv_obj_create(scr_big);
  lv_obj_remove_style_all(big_arrow_box);
  lv_obj_set_size(big_arrow_box, S(120), S(160));
  lv_obj_align(big_arrow_box, LV_ALIGN_CENTER, BIG_ARROW_X, BIG_VALUE_Y);
  big_arrow = make_arrow(big_arrow_box, S(100));

  big_unit = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_unit, FONT_28, 0);
  lv_obj_set_style_text_color(big_unit, C_MUTED, 0);
  lv_label_set_text(big_unit, "mg/dL");
  lv_obj_align(big_unit, LV_ALIGN_CENTER, 0, S(105));

  big_age = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_age, FONT_28, 0);
  lv_obj_set_style_text_color(big_age, C_MUTED, 0);
  lv_label_set_text(big_age, "");
  lv_obj_align(big_age, LV_ALIGN_CENTER, 0, S(150));

  big_hint = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_hint, FONT_20, 0);
  lv_obj_set_style_text_color(big_hint, lv_color_hex(0x3D444D), 0);
  lv_label_set_text(big_hint, "tap for graph  " LV_SYMBOL_RIGHT);
  lv_obj_align(big_hint, LV_ALIGN_BOTTOM_RIGHT, S(-20), S(-14));

  big_status = lv_label_create(scr_big);
  lv_obj_set_style_text_font(big_status, FONT_20, 0);
  lv_obj_set_style_text_color(big_status, C_MUTED, 0);
  lv_label_set_text(big_status, "");
  lv_obj_align(big_status, LV_ALIGN_BOTTOM_LEFT, S(20), S(-14));

  ind_big = make_indicators(scr_big);
}

static void build_graph() {
  scr_graph = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_graph, C_BG, 0);

  // ----- header -----
  g_value = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_value, FONT_VALUE, 0);
  lv_obj_set_style_text_color(g_value, C_OK, 0);
  lv_label_set_text(g_value, "---");
  lv_obj_align(g_value, LV_ALIGN_TOP_LEFT, S(24), S(18));

  g_arrow_box = lv_obj_create(scr_graph);
  lv_obj_remove_style_all(g_arrow_box);
  lv_obj_set_size(g_arrow_box, S(60), S(60));
  lv_obj_align(g_arrow_box, LV_ALIGN_TOP_LEFT, S(130), S(14));
  g_arrow = make_arrow(g_arrow_box, S(44));

  lv_obj_t *unit = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(unit, FONT_20, 0);
  lv_obj_set_style_text_color(unit, C_MUTED, 0);
  lv_label_set_text(unit, "mg/dL");
  lv_obj_align(unit, LV_ALIGN_TOP_LEFT, S(200), S(20));

  g_age = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_age, FONT_20, 0);
  lv_obj_set_style_text_color(g_age, C_MUTED, 0);
  lv_label_set_text(g_age, "");
  lv_obj_align(g_age, LV_ALIGN_TOP_LEFT, S(200), S(48));

  g_stale = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_stale, FONT_24, 0);
  lv_obj_set_style_text_color(g_stale, C_LOW, 0);
  lv_label_set_text(g_stale, LV_SYMBOL_WARNING "  NO RECENT DATA");
  lv_obj_align(g_stale, LV_ALIGN_TOP_RIGHT, S(-24), S(38));
  lv_obj_add_flag(g_stale, LV_OBJ_FLAG_HIDDEN);

  g_status = lv_label_create(scr_graph);
  lv_obj_set_style_text_font(g_status, FONT_14, 0);
  lv_obj_set_style_text_color(g_status, C_MUTED, 0);
  lv_label_set_text(g_status, "");
  lv_obj_align(g_status, LV_ALIGN_TOP_RIGHT, S(-24), S(72));

  // ----- chart -----
  setup_chart(scr_graph, S(4));
  const int ylabels[] = {300, 250, 200, 150, 100, 70, 40};
  for (int v : ylabels) {
    lv_obj_t *l = lv_label_create(scr_graph);
    lv_obj_set_style_text_font(l, FONT_14, 0);
    lv_obj_set_style_text_color(l, (v == GLUCOSE_HIGH) ? C_HIGH : (v == GLUCOSE_LOW) ? C_LOW : C_MUTED, 0);
    lv_label_set_text_fmt(l, "%d", v);
    lv_obj_set_width(l, CHART_X - S(10));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l, 0, CHART_Y + chart_y_for(v) - S(8));
  }
  // x labels (start / mid / now)
  for (int i = 0; i < 3; i++) {
    g_xlabels[i] = lv_label_create(scr_graph);
    lv_obj_set_style_text_font(g_xlabels[i], FONT_14, 0);
    lv_obj_set_style_text_color(g_xlabels[i], C_MUTED, 0);
  }

  g_series = lv_chart_add_series(g_chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

  // ----- bottom bar -----
  make_btn(scr_graph, LV_SYMBOL_LEFT "  Back", goto_big, nullptr, S(140));
  lv_obj_align(lv_obj_get_child(scr_graph, -1), LV_ALIGN_BOTTOM_LEFT, S(20), S(-14));

  lv_obj_t *row = lv_obj_create(scr_graph);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 5 * S(88) + 4 * S(10), S(56));
  lv_obj_align(row, LV_ALIGN_BOTTOM_RIGHT, S(-20), S(-14));
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, S(10), 0);
  const char *names[5] = {"1h", "3h", "6h", "12h", "24h"};
  for (int i = 0; i < 5; i++) {
    g_win_btns[i] = make_btn(row, names[i], on_window_btn, (void *)(intptr_t)i, S(88));
    lv_obj_add_flag(g_win_btns[i], LV_OBJ_FLAG_CHECKABLE);
  }
  lv_obj_add_state(g_win_btns[g_window_idx], LV_STATE_CHECKED);

  ind_graph = make_indicators(scr_graph);
}
#endif

// ---------- refresh ----------
static void refresh_chart() {
  int hours = WINDOWS[g_window_idx];
  int n = hours * 12 + 1;
  glucose::window(hours, g_data, n, LV_CHART_POINT_NONE);
  // color the series by current state
  lv_chart_set_point_count(g_chart, n);
  lv_chart_set_ext_y_array(g_chart, g_series, g_data);
  lv_chart_refresh(g_chart);

  char b[16];
  snprintf(b, sizeof b, "-%dh", hours);
  lv_label_set_text(g_xlabels[0], b);
  lv_obj_set_pos(g_xlabels[0], CHART_X, CHART_Y + CHART_H + S(4));
  if (hours >= 2) snprintf(b, sizeof b, "-%dh", hours / 2); else snprintf(b, sizeof b, "-30m");
  lv_label_set_text(g_xlabels[1], b);
  lv_obj_align_to(g_xlabels[1], g_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, S(4));
  lv_label_set_text(g_xlabels[2], "now");
#if UI_COMPACT
  snprintf(b, sizeof b, "%dh", hours);
  lv_label_set_text(g_win_label, b);
#endif
  lv_obj_align_to(g_xlabels[2], g_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, S(4));
}

// only redraw when the text actually changes (every redraw costs a PSRAM framebuffer copy)
static void set_text_if_changed(lv_obj_t *l, const char *s) {
  if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}
static void set_color_if_changed(lv_obj_t *l, lv_color_t c) {
  lv_color_t cur = lv_obj_get_style_text_color(l, 0);
  if (!lv_color_eq(cur, c)) lv_obj_set_style_text_color(l, c, 0);
}

static void refresh_values() {
  const Reading *r = glucose::latest();
  if (!r) {
    set_text_if_changed(big_value, "---");
    set_color_if_changed(big_value, C_STALE);
    set_text_if_changed(big_age, "waiting for data");
    set_text_if_changed(g_value, "---");
    return;
  }
  uint32_t age = glucose::now() - r->ts;
  bool stale = age > STALE_AFTER_S;
  lv_color_t c = color_for(r->value, stale);
  char age_s[32], val_s[8];
  fmt_age(age_s, sizeof age_s, age);
  snprintf(val_s, sizeof val_s, "%d", r->value);

  set_text_if_changed(big_value, val_s);
  set_color_if_changed(big_value, c);
  set_text_if_changed(big_age, age_s);
  set_color_if_changed(big_age, stale ? C_LOW : C_MUTED);
  set_text_if_changed(g_value, val_s);
  set_color_if_changed(g_value, c);
  set_text_if_changed(g_age, age_s);
  if (stale) lv_obj_remove_flag(g_stale, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_stale, LV_OBJ_FLAG_HIDDEN);
}

// arrows + chart colour only change on new data, not every second
static void refresh_trend() {
  const Reading *r = glucose::latest();
  bool stale = r && (glucose::now() - r->ts > STALE_AFTER_S);
  lv_color_t c = r ? color_for(r->value, stale) : C_STALE;
  Trend t = r ? glucose::trend() : TREND_UNKNOWN;
  arrow_set(big_arrow, t, c);
  arrow_set(g_arrow, t, c);
  lv_chart_set_series_color(g_chart, g_series, c);
}

static void on_tick(lv_timer_t *) { refresh_values(); }

static void init() {
  build_big();
  build_graph();
  refresh_values();
  refresh_trend();
  refresh_chart();
  lv_timer_create(on_tick, 10000, nullptr);   // age text is minute-granular; no need to re-check every second
  lv_screen_load(scr_big);
}

// button navigation (boards without touch)
static void toggle_screen() {
  lv_screen_load(lv_screen_active() == scr_big ? scr_graph : scr_big);
}
static void next_window() {
  g_window_idx = (g_window_idx + 1) % 5;
  refresh_chart();
}

// call when a new reading arrives
static void on_new_reading() {
  refresh_values();
  refresh_trend();
  refresh_chart();
}

// status icons: green = ok, red = down; vpn < 0 hides the VPN icon (tunnel not used)
static void set_indicators(bool wifi, int vpn, bool ha) {
  Indicators *all[2] = {&ind_big, &ind_graph};
  for (Indicators *in : all) {
    lv_obj_remove_flag(in->row, LV_OBJ_FLAG_HIDDEN);
    set_color_if_changed(in->wifi, wifi ? C_OK : C_LOW);
    set_color_if_changed(in->ha, ha ? C_OK : C_LOW);
    if (vpn < 0) lv_obj_add_flag(in->vpn, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_remove_flag(in->vpn, LV_OBJ_FLAG_HIDDEN); set_color_if_changed(in->vpn, vpn ? C_OK : C_LOW); }
  }
}

// connection / source status shown in the corner ("" = hide)
static void set_status(const char *s) {
  set_text_if_changed(big_status, s);
  set_text_if_changed(g_status, s);
}

}  // namespace ui
