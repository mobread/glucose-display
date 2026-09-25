#include <Arduino.h>
#include "board.h"
#include "glucose.h"
#include "ui.h"

#if USE_HA
#include "ha_source.h"
#endif

void setup() {
  Serial.begin(115200);
  Serial.println("[glucose-display] boot");

  board::init();
#if USE_HA
  ui::init();
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
  if (ha::take()) {
    const Reading *r = glucose::latest();
    if (r) Serial.printf("[glucose] %d mg/dL trend=%d\n", r->value, (int)glucose::trend());
    ui::on_new_reading();
  }
  if (millis() - last_status > 1000) {
    last_status = millis();
    char s[48];
    ha::get_status(s, sizeof s);
    ui::set_status(s);
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
