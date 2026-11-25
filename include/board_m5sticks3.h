// Board support for M5Stack StickS3 (ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM,
// 1.14" ST7789P3 135x240 on SPI, M5PM1 PMIC). Big-number screen only, like the M5StickC Plus.
//
// Values come from M5GFX's StickS3 board data (board_detect/m5/generated/esp32s3_board_data.hpp
// and pmic_ops.hpp sticks3_power_on) and M5Unified's button handling:
//   - the LCD + backlight sit on the PMIC's L3B rail, enabled by driving M5PM1 GPIO2 high;
//     nothing on the panel works until that is done
//   - backlight is plain PWM on GPIO38
//   - buttons: A = GPIO11 (front), B = GPIO12 (side), active low
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define LCD_H_RES 240   // landscape
#define LCD_V_RES 135
#define UI_W LCD_H_RES
#define UI_H LCD_V_RES
#define UI_NO_GRAPH 1
#define WIFI_PORTAL 1   // captive-portal WiFi setup (wifi_portal.h)

#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define PM1_ADDR    0x6E
#define PIN_BTN_A   11
#define PIN_BTN_B   12

#ifndef BACKLIGHT_PCT
#define BACKLIGHT_PCT 50
#endif
// PWM duty at 1 % brightness (of 255). Keeps a low HA brightness setting readable.
#ifndef BACKLIGHT_MIN_DUTY
#define BACKLIGHT_MIN_DUTY 48
#endif

namespace board {

// ---------- M5PM1 PMIC ----------
static uint8_t pm1_read(uint8_t reg) {
  Wire.beginTransmission(PM1_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)PM1_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

static void pm1_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(PM1_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void pm1_bits(uint8_t reg, uint8_t mask, bool on) {
  uint8_t v = pm1_read(reg);
  pm1_write(reg, on ? (v | mask) : (v & ~mask));
}

// Same sequence as M5GFX's sticks3_power_on.
static bool pm1_power_on() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  Wire.beginTransmission(PM1_ADDR);
  if (Wire.endTransmission() != 0) { Serial.println("[board] M5PM1 not responding"); return false; }
  pm1_write(0x09, 0x00);            // keep the PMIC's I2C awake (no idle sleep)
  pm1_bits(0x16, 0x04, false);      // GPIO2: plain GPIO function
  pm1_bits(0x10, 0x04, true);       // GPIO2: output
  pm1_bits(0x13, 0x04, false);      // GPIO2: push-pull
  pm1_bits(0x11, 0x04, true);       // GPIO2 high -> L3B rail on (LCD, backlight, mic, speaker)
  return true;
}

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = 40;
      cfg.pin_mosi = 39;
      cfg.pin_miso = -1;
      cfg.pin_dc = 45;
      cfg.spi_3wire = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 41;
      cfg.pin_rst = 21;
      cfg.pin_busy = -1;
      cfg.panel_width = 135;
      cfg.panel_height = 240;
      cfg.offset_x = 52;
      cfg.offset_y = 40;
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
      cfg.freq = 256;        // what M5GFX uses for this board
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }
};

static LGFX lcd;

// percent: 0 = off, 100 = brightest; 1 % starts at BACKLIGHT_MIN_DUTY so dim settings stay visible
static void set_brightness(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lcd.setBrightness(pct == 0 ? 0 : (uint8_t)(BACKLIGHT_MIN_DUTY + (255 - BACKLIGHT_MIN_DUTY) * pct / 100));
}

// ---------- buttons ----------
enum : uint8_t { BTN_A = 1, BTN_B = 2 };

struct BtnEvents { uint8_t pressed = 0, long_pressed = 0; };

// Debounced. `pressed` fires on release after a short press, `long_pressed` once after held 2 s.
static BtnEvents poll_buttons() {
  static const uint8_t pins[2] = {PIN_BTN_A, PIN_BTN_B};
  static bool stable[2] = {false, false}, last_raw[2] = {false, false}, long_fired[2] = {false, false};
  static uint32_t changed_at[2] = {0, 0}, down_at[2] = {0, 0};
  BtnEvents ev;
  uint32_t now = millis();
  for (int i = 0; i < 2; i++) {
    bool down = digitalRead(pins[i]) == LOW;
    if (down != last_raw[i]) { last_raw[i] = down; changed_at[i] = now; }
    if (down != stable[i] && now - changed_at[i] > 30) {
      stable[i] = down;
      if (down) { down_at[i] = now; long_fired[i] = false; }
      else if (!long_fired[i]) ev.pressed |= (1 << i);
    }
    if (stable[i] && !long_fired[i] && now - down_at[i] > 2000) {
      long_fired[i] = true;
      ev.long_pressed |= (1 << i);
    }
  }
  return ev;
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
  pm1_power_on();
  delay(10);
  lcd.init();
  lcd.setRotation(1);                  // landscape; 3 flips it 180 degrees
  lcd.fillScreen(TFT_BLACK);
  set_brightness(BACKLIGHT_PCT);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  constexpr size_t buf_px = LCD_H_RES * 40;
  static uint16_t *buf1 = (uint16_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf1) { Serial.println("[board] LVGL buffer alloc failed"); abort(); }
  Serial.printf("[board] M5StickS3 %dx%d, internal heap free: %u, psram: %u\n", lcd.width(), lcd.height(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf1, nullptr, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

static void init() {
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  display_init();
}

}  // namespace board
