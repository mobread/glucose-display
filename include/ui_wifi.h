// On-device WiFi setup screen (Waveshare P4 build, 1280x720).
//
// Opens automatically when no network is configured or the configured one can't be joined
// (and this boot has never been online), and from the WiFi button on the graph screen.
// Left: scanned networks (tap to pick). Right: network name + password fields, Connect/Back.
// An on-screen keyboard slides up while a field is being edited.
#pragma once

#include <lvgl.h>
#include "ui.h"
#include "wifi_mgr.h"

namespace ui_wifi {

static lv_obj_t *scr = nullptr, *list, *ta_ssid, *ta_pass, *kb, *status, *btn_show, *btn_scan_lbl;
static net::Network nets[net::MAX_NETWORKS];
static uint32_t nets_gen = 0;
static bool auto_opened = false;
static bool waiting_for_connect = false;   // user pressed Connect; return to main screen on success
static uint32_t connected_at = 0;
static lv_obj_t *prev_screen = nullptr;
static char picked_ssid[33] = "";   // exact bytes of the network tapped in the list

// The built-in Montserrat fonts are ASCII-only: show typographic quotes (iPhone hotspot names
// use ’) as plain ASCII and anything else non-ASCII as '?'. Only for display -- connecting
// always uses the exact SSID bytes.
static void display_text(char *out, size_t n, const char *in) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n;) {
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) { out[o++] = '\''; p += 3; }
    else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x9C || p[2] == 0x9D)) { out[o++] = '"'; p += 3; }
    else if (*p < 0x80) out[o++] = *p++;
    else { out[o++] = '?'; p++; while ((*p & 0xC0) == 0x80) p++; }
  }
  out[o] = 0;
}

static const int TOP = 100;            // content starts below the title row
static const int LIST_W = 560;
static const int RIGHT_X = 24 + LIST_W + 36;
static const int RIGHT_W = UI_W - RIGHT_X - 24;
static const int KB_H = 300;

static void kb_hide() {
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(kb, nullptr);
  lv_obj_remove_state(ta_ssid, LV_STATE_FOCUSED);
  lv_obj_remove_state(ta_pass, LV_STATE_FOCUSED);
}

static void kb_show_for(lv_obj_t *ta) {
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_state(ta, LV_STATE_FOCUSED);
}

static void on_ta_event(lv_event_t *e) {
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_CLICKED || lv_event_get_code(e) == LV_EVENT_FOCUSED) kb_show_for(ta);
}

static void connect_now() {
  const char *ssid = lv_textarea_get_text(ta_ssid);
  // the field shows a display-safe copy of the picked network; connect with its exact name
  char shown[33];
  display_text(shown, sizeof shown, picked_ssid);
  if (picked_ssid[0] && !strcmp(ssid, shown)) ssid = picked_ssid;
  if (!ssid[0]) { lv_label_set_text(status, "Pick a network or type its name"); return; }
  kb_hide();
  net::request_connect(ssid, lv_textarea_get_text(ta_pass));
  waiting_for_connect = true;
  connected_at = 0;
  display_text(shown, sizeof shown, ssid);
  lv_label_set_text_fmt(status, "Connecting to %s ...", shown);
}

static void on_kb_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {           // the keyboard's OK key
    if (lv_keyboard_get_textarea(kb) == ta_ssid) kb_show_for(ta_pass);
    else connect_now();
  } else if (code == LV_EVENT_CANCEL) {
    kb_hide();
  }
}

static void on_network_click(lv_event_t *e) {
  const net::Network *n = (const net::Network *)lv_event_get_user_data(e);
  char shown[33];
  display_text(shown, sizeof shown, n->ssid);
  strlcpy(picked_ssid, n->ssid, sizeof picked_ssid);
  lv_textarea_set_text(ta_ssid, shown);
  lv_textarea_set_text(ta_pass, "");
  if (n->secure) kb_show_for(ta_pass);
  else kb_hide();
}

static void rebuild_list(int count) {
  lv_obj_clean(list);
  if (count == 0) {
    lv_obj_t *l = lv_label_create(list);
    lv_label_set_text(l, "No 2.4 GHz networks found");
    lv_obj_set_style_text_color(l, ui::C_MUTED, 0);
    lv_obj_set_style_pad_all(l, 16, 0);
    return;
  }
  for (int i = 0; i < count; i++) {
    char shown[33];
    display_text(shown, sizeof shown, nets[i].ssid);
    lv_obj_t *b = lv_list_add_button(list, LV_SYMBOL_WIFI, shown);
    lv_obj_set_style_bg_color(b, ui::C_PANEL, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3441), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, ui::C_TEXT, 0);
    lv_obj_set_style_text_font(b, FONT_24, 0);
    lv_obj_set_style_pad_ver(b, 18, 0);
    lv_obj_set_style_border_color(b, ui::C_BG, 0);
    lv_obj_add_event_cb(b, on_network_click, LV_EVENT_CLICKED, &nets[i]);
    // signal quality on the right; "open" marks networks without a password
    int q = constrain(2 * (nets[i].rssi + 100), 0, 100);
    lv_obj_t *r = lv_label_create(b);
    lv_label_set_text_fmt(r, "%s%d%%", nets[i].secure ? "" : "open  ", q);
    lv_obj_set_style_text_font(r, FONT_20, 0);
    lv_obj_set_style_text_color(r, ui::C_MUTED, 0);
  }
}

static void rescan() {
  net::request_scan();
  lv_obj_clean(list);
  lv_obj_t *l = lv_label_create(list);
  lv_label_set_text(l, "Scanning ...");
  lv_obj_set_style_text_color(l, ui::C_MUTED, 0);
  lv_obj_set_style_pad_all(l, 16, 0);
}

static void close_screen() {
  kb_hide();
  waiting_for_connect = false;
  lv_screen_load(prev_screen ? prev_screen : ui::scr_big);
}

static void open_screen() {
  if (lv_screen_active() == scr) return;
  prev_screen = lv_screen_active();
  char ssid[33], shown[33];
  net::get_state(ssid, sizeof ssid);
  strlcpy(picked_ssid, ssid, sizeof picked_ssid);
  display_text(shown, sizeof shown, ssid);
  lv_textarea_set_text(ta_ssid, shown);
  lv_textarea_set_text(ta_pass, "");
  kb_hide();
  rescan();
  lv_screen_load(scr);
}

static lv_obj_t *make_ta(int y, bool password) {
  lv_obj_t *ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, password);
  lv_textarea_set_max_length(ta, password ? 64 : 32);
  lv_obj_set_style_text_font(ta, FONT_24, 0);
  lv_obj_set_style_bg_color(ta, ui::C_PANEL, 0);
  lv_obj_set_style_text_color(ta, ui::C_TEXT, 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(0x2A3441), 0);
  lv_obj_set_style_border_color(ta, ui::C_ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(ta, 10, 0);
  lv_obj_set_pos(ta, RIGHT_X, y);
  lv_obj_set_width(ta, password ? RIGHT_W - 140 : RIGHT_W);
  lv_obj_add_event_cb(ta, on_ta_event, LV_EVENT_ALL, nullptr);
  return ta;
}

static lv_obj_t *make_label(const char *txt, int x, int y, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(scr);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

static void build() {
  scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, ui::C_BG, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, [](lv_event_t *) { kb_hide(); }, LV_EVENT_CLICKED, nullptr);   // tap outside -> hide keyboard

  make_label(LV_SYMBOL_WIFI "  WiFi setup", 24, 22, FONT_28, ui::C_TEXT);

  lv_obj_t *scan = ui::make_btn(scr, LV_SYMBOL_REFRESH "  Scan", [](lv_event_t *) { rescan(); }, nullptr, 200);
  lv_obj_set_pos(scan, 24 + LIST_W - 200, 16);
  (void)btn_scan_lbl;

  list = lv_list_create(scr);
  lv_obj_set_pos(list, 24, TOP);
  lv_obj_set_size(list, LIST_W, UI_H - TOP - 24);
  lv_obj_set_style_bg_color(list, ui::C_PANEL, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_radius(list, 12, 0);
  lv_obj_set_style_pad_all(list, 6, 0);

  make_label("Network name", RIGHT_X, TOP, FONT_20, ui::C_MUTED);
  ta_ssid = make_ta(TOP + 34, false);
  make_label("Password", RIGHT_X, TOP + 120, FONT_20, ui::C_MUTED);
  ta_pass = make_ta(TOP + 154, true);

  btn_show = ui::make_btn(scr, LV_SYMBOL_EYE_OPEN, [](lv_event_t *) {
    bool pw = lv_textarea_get_password_mode(ta_pass);
    lv_textarea_set_password_mode(ta_pass, !pw);
  }, nullptr, 124);
  lv_obj_set_height(btn_show, lv_obj_get_height(ta_pass));
  lv_obj_set_pos(btn_show, RIGHT_X + RIGHT_W - 124, TOP + 154);

  lv_obj_t *bc = ui::make_btn(scr, "Connect", [](lv_event_t *) { connect_now(); }, nullptr, (RIGHT_W - 16) / 2);
  lv_obj_set_style_bg_color(bc, ui::C_ACCENT, 0);
  lv_obj_set_pos(bc, RIGHT_X, TOP + 240);
  lv_obj_t *bb = ui::make_btn(scr, LV_SYMBOL_LEFT "  Back", [](lv_event_t *) { close_screen(); }, nullptr, (RIGHT_W - 16) / 2);
  lv_obj_set_pos(bb, RIGHT_X + (RIGHT_W - 16) / 2 + 16, TOP + 240);

  status = make_label("", RIGHT_X, TOP + 340, FONT_24, ui::C_MUTED);
  lv_obj_set_width(status, RIGHT_W);
  lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);

  kb = lv_keyboard_create(scr);
  lv_obj_set_size(kb, UI_W, KB_H);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(kb, FONT_24, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x0F141A), 0);
  lv_obj_set_style_bg_color(kb, ui::C_PANEL, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, ui::C_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_ALL, nullptr);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

// WiFi button on the graph screen, next to Back
static void add_graph_button() {
  lv_obj_t *b = ui::make_btn(ui::scr_graph, LV_SYMBOL_WIFI, [](lv_event_t *) { open_screen(); }, nullptr, S(70));
  lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, S(20) + S(140) + S(12), S(-14));
}

static void init() {
  net::init();
  build();
  add_graph_button();
}

// call from loop(); cheap
static void poll() {
  static uint32_t last = 0;
  if (millis() - last < 250) return;
  last = millis();

  char raw[33], ssid[33], reason[32];
  net::State st = net::get_state(raw, sizeof raw, reason, sizeof reason);
  display_text(ssid, sizeof ssid, raw);

  // first-run / new-location: no usable network and never online this boot -> open setup once
  if (!auto_opened && (st == net::NO_CONFIG || (st == net::FAILED && !net::has_ever_connected()))) {
    auto_opened = true;
    open_screen();
    lv_label_set_text(status, st == net::NO_CONFIG ? "Choose a WiFi network (2.4 GHz)"
                                                   : "Can't join the saved network. Choose another or check the password.");
  }

  if (lv_screen_active() != scr) return;

  int n = net::get_networks(nets, nets_gen);
  if (n >= 0) rebuild_list(n);

  if (waiting_for_connect) {
    if (st == net::CONNECTED) {
      if (!connected_at) {
        connected_at = millis();
        lv_label_set_text_fmt(status, LV_SYMBOL_OK "  Connected to %s", ssid);
        lv_obj_set_style_text_color(status, ui::C_OK, 0);
      } else if (millis() - connected_at > 2000) {
        lv_obj_set_style_text_color(status, ui::C_MUTED, 0);
        waiting_for_connect = false;
        lv_screen_load(ui::scr_big);
      }
    } else if (st == net::FAILED) {
      lv_label_set_text_fmt(status, "Couldn't join %s: %s", ssid, reason);
      lv_obj_set_style_text_color(status, ui::C_LOW, 0);
    }
  }
}

}  // namespace ui_wifi
