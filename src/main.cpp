#include <Arduino.h>
#include "board.h"
#include "glucose.h"
#include "ui.h"

#if USE_HA
#include "ha_source.h"
static volatile int pending_brightness = -1;
#if defined(BOARD_WAVESHARE_P4_5)
#define WIFI_SETUP_UI 1
#include "ui_wifi.h"
#endif
#if WIFI_PORTAL
#include "wifi_portal.h"
#endif
#endif

void setup() {
  Serial.begin(115200);
  Serial.println("[glucose-display] boot");

  board::init();
#if USE_HA
  ui::init();
#if WIFI_SETUP_UI
  ui_wifi::init();
#endif
  ha::on_brightness = [](int pct) { pending_brightness = pct; };  // applied on the main loop (Wire isn't cross-core safe)
  ha::begin();
#if WIFI_PORTAL
  portal::begin();
#endif
#else
  glucose::dummy::init();
  ui::init();
  ui::set_status("dummy data");
#endif

  Serial.println("[glucose-display] ready");
}

void loop() {
#if defined(BOARD_TDISPLAY_S3)
  // no touch: bottom button (BOOT) flips screens, top button (KEY) cycles the graph window
  uint8_t btn = board::poll_buttons();
  if (btn & board::BTN_BOOT) ui::toggle_screen();
  if (btn & board::BTN_KEY) {
    if (lv_screen_active() != ui::scr_graph) ui::toggle_screen();
    else ui::next_window();
  }
#endif
#if defined(BOARD_M5STICKS3) && USE_HA
  // hold the front button (A) 2 s: open / close the WiFi setup hotspot
  // hold the side button (B) 2 s: forget all saved WiFi and go back to setup (hotspot opens itself)
  board::BtnEvents btn = board::poll_buttons();
  if (btn.long_pressed & board::BTN_A) {
    if (portal::active()) portal::request_stop(); else portal::request_start();
  }
  if (btn.long_pressed & board::BTN_B) {
    net::request_forget_all();
    ui::show_overlay("WiFi setup", "Forgetting saved WiFi...", "");
  }
#endif
#if WIFI_PORTAL && USE_HA
  static uint32_t last_portal_ui = 0;
  if (millis() - last_portal_ui > 300) {
    last_portal_ui = millis();
    char ssid[33], reason[32], body[64];
    net::State st = net::get_state(ssid, sizeof ssid, reason, sizeof reason);
    switch (portal::phase) {
      case portal::OFF:     ui::show_overlay(nullptr); break;
      case portal::WAITING:
        snprintf(body, sizeof body, "Join  %s", portal::ap_name);
        ui::show_overlay("WiFi setup", body, "on your phone, then pick a network");
        break;
      case portal::CONNECTING: ui::show_overlay("Connecting to", ssid, "keep the setup page open"); break;
      case portal::FAILED:
        snprintf(body, sizeof body, "%s: %s", ssid, reason);
        ui::show_overlay("Couldn't connect", body, "try again on the setup page");
        break;
      case portal::DONE:
        ui::show_overlay("Connected to", st == net::CONNECTED ? ssid : "", "closing setup hotspot...");
        break;
    }
  }
#endif
#if USE_HA
  static uint32_t last_status = 0;
  if (pending_brightness >= 0) {
    board::set_brightness(pending_brightness);
    pending_brightness = -1;
  }
  if (ha::take()) {
    const Reading *r = glucose::latest();
    if (r) Serial.printf("[glucose] %d mg/dL trend=%d\n", r->value, (int)glucose::trend());
    ui::on_new_reading();
  }
#if WIFI_SETUP_UI
  ui_wifi::poll();
#endif
  if (millis() - last_status > 1000) {
    last_status = millis();
    char s[48];
    ha::get_status(s, sizeof s);
    ui::set_status(s);
    bool wifi = net::get_state() == net::CONNECTED;
    ui::set_indicators(wifi, wg::enabled ? (wifi && wg::last_up) : -1, wifi && ha::ha_ok);
  }
#else
  if (glucose::dummy::poll()) {
    const Reading *r = glucose::latest();
    Serial.printf("[glucose] new reading %d mg/dL trend=%d\n", r->value, (int)glucose::trend());
    ui::on_new_reading();
  }
#endif
#ifdef HAS_BATTERY
  // one I2C read every 30 s; next to the display and WiFi this costs nothing measurable
  static uint32_t last_batt = 0, last_batt_log = 0;
  if (!last_batt || millis() - last_batt > 30000) {
    last_batt = millis();
    int pct = -1, mv = 0;
    bool chg = false;
    if (board::battery(pct, chg, mv)) {
      if (!last_batt_log || millis() - last_batt_log > 300000) {
        last_batt_log = millis();
        Serial.printf("[battery] %d mV, %d%%%s\n", mv, pct, chg ? ", charging" : "");
      }
    } else {
      pct = -1;
    }
    ui::set_battery(pct, chg);
  }
#endif
  lv_timer_handler();
  delay(2);
}
