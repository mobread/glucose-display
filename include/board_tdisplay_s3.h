// Board support for LilyGo T-Display-S3 (ESP32-S3R8, 1.9" ST7789 170x320 on an 8-bit parallel bus).
// Non-touch version: input is the two side buttons (GPIO0 = BOOT, GPIO14 = KEY).
// Pinout matches LilyGo's examples and esp-lcd/shared/tft_config.py.
#pragma once

#include <Arduino.h>
#include <lvgl.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define LCD_H_RES 320   // landscape
#define LCD_V_RES 170
#define UI_W LCD_H_RES
#define UI_H LCD_V_RES

#define PIN_LCD_POWER 15
#define PIN_BTN_BOOT  0
#define PIN_BTN_KEY   14

#ifndef BACKLIGHT_PCT
#define BACKLIGHT_PCT 50
#endif

namespace board {

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_Parallel8 bus_;
  lgfx::Light_PWM light_;

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.freq_write = 20000000;
      cfg.pin_wr = 8;
      cfg.pin_rd = 9;
      cfg.pin_rs = 7;   // D/C
      cfg.pin_d0 = 39; cfg.pin_d1 = 40; cfg.pin_d2 = 41; cfg.pin_d3 = 42;
      cfg.pin_d4 = 45; cfg.pin_d5 = 46; cfg.pin_d6 = 47; cfg.pin_d7 = 48;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 6;
      cfg.pin_rst = 5;
      cfg.pin_busy = -1;
      cfg.panel_width = 170;
      cfg.panel_height = 320;
      cfg.offset_x = 35;     // the 170-px-wide glass sits in the middle of the ST7789's 240 columns
      cfg.offset_y = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = 38;
      cfg.invert = false;
      cfg.freq = 22000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }
};

static LGFX lcd;

// percent: 0 = off, 100 = brightest
static void set_brightness(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lcd.setBrightness((uint8_t)(pct * 255 / 100));
}

// ---------- buttons ----------
enum : uint8_t { BTN_BOOT = 1, BTN_KEY = 2 };

// Returns a bitmask of buttons that were pressed since the last call (debounced, press edge).
static uint8_t poll_buttons() {
  static const uint8_t pins[2] = {PIN_BTN_BOOT, PIN_BTN_KEY};
  static bool stable[2] = {false, false};
  static uint32_t changed_at[2] = {0, 0};
  static bool last_raw[2] = {false, false};
  uint8_t events = 0;
  uint32_t now = millis();
  for (int i = 0; i < 2; i++) {
    bool down = digitalRead(pins[i]) == LOW;
    if (down != last_raw[i]) { last_raw[i] = down; changed_at[i] = now; }
    if (down != stable[i] && now - changed_at[i] > 30) {
      stable[i] = down;
      if (down) events |= (1 << i);
    }
  }
  return events;
}

// ---------- LVGL ----------
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  lcd.startWrite();
  lcd.pushImage(area->x1, area->y1, w, h, (lgfx::rgb565_t *)px_map);
  lcd.endWrite();
  lv_display_flush_ready(disp);
}

static void display_init() {
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);   // LCD + backlight power rail (also needed on battery)
  lcd.init();
  lcd.setRotation(1);                  // landscape, USB on the right
  lcd.fillScreen(TFT_BLACK);
  set_brightness(BACKLIGHT_PCT);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  constexpr size_t buf_px = LCD_H_RES * 40;   // 25 KB
  static uint16_t *buf1 = (uint16_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf1) { Serial.println("[board] LVGL buffer alloc failed"); abort(); }
  Serial.printf("[board] T-Display-S3 %dx%d, internal heap free: %u\n", lcd.width(), lcd.height(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf1, nullptr, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

static void init() {
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BTN_KEY, INPUT_PULLUP);
  display_init();
}

}  // namespace board
