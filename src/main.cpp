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
#else
  glucose::dummy::init();
  ui::init();
  ui::set_status("dummy data");
#endif

  Serial.println("[glucose-display] ready");
}

void loop() {
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
  lv_timer_handler();
  delay(2);
}
