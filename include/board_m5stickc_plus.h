// Board support for M5StickC Plus (ESP32-PICO-D4, no PSRAM, 1.14" ST7789V2 135x240 on SPI).
// Backlight and LCD power come from the AXP192 PMU (I2C 0x34): LDO2 = backlight, LDO3 = LCD logic.
// Big-number screen only (no graph). Buttons: A = GPIO37 (front), B = GPIO39 (side), active low,
// external pull-ups (input-only pins). Hold A: WiFi setup hotspot on/off; hold B: forget WiFi.
// Pinout matches M5's M5StickCPlus library / LovyanGFX's autodetect for this board.
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

#define PIN_BTN_A 37
#define PIN_BTN_B 39

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define AXP192_ADDR 0x34

#ifndef BACKLIGHT_PCT
#define BACKLIGHT_PCT 50
#endif

namespace board {

// ---------- AXP192 ----------
static uint8_t axp_read(uint8_t reg) {
  Wire.beginTransmission(AXP192_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)AXP192_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

static void axp_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AXP192_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Same register setup as M5's AXP192::begin() for the StickC Plus.
static void axp_init() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  axp_write(0x28, 0xCC);                     // LDO2 (backlight) / LDO3 (LCD) = 3.0 V
  axp_write(0x82, 0xFF);                     // all ADCs on (battery / VBUS monitoring)
  axp_write(0x33, 0xC0);                     // charge to 4.2 V, 100 mA
  axp_write(0x12, axp_read(0x12) | 0x4D);    // enable EXTEN, LDO3, LDO2, DCDC1
  axp_write(0x36, 0x0C);                     // power key: 128 ms on, 4 s off
  axp_write(0x91, 0xF0);                     // RTC / GPIO0 LDO = 3.3 V
  axp_write(0x90, 0x02);                     // GPIO0 as LDO (mic power)
  axp_write(0x30, 0x80);                     // no VBUS hold voltage limit
  axp_write(0x39, 0xFC);                     // temperature protection
  axp_write(0x35, 0xA2);                     // RTC backup battery charging
  axp_write(0x32, 0x46);                     // battery detection on
}

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_SPI bus_;

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = 13;
      cfg.pin_mosi = 15;
      cfg.pin_miso = -1;
      cfg.pin_dc = 23;
      cfg.spi_3wire = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 5;
      cfg.pin_rst = 18;
      cfg.pin_busy = -1;
      cfg.panel_width = 135;
      cfg.panel_height = 240;
      cfg.offset_x = 52;     // the 135x240 glass sits inside the ST7789's 240x320 RAM
      cfg.offset_y = 40;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    setPanel(&panel_);
  }
};

static LGFX lcd;

// percent: 0 = off, 100 = brightest. LDO2 is set in 100 mV steps (max 3.3 V); 1..100 % maps onto
// BACKLIGHT_MIN_MV..3.3 V. The LED only glows faintly at 2.5 V, so the floor is raised to keep a low
// HA setting readable. Call from the main loop only (Wire isn't thread safe).
#ifndef BACKLIGHT_MIN_MV
#define BACKLIGHT_MIN_MV 2900
#endif
static void set_brightness(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  uint8_t pwr = axp_read(0x12);
  if (pct == 0) { axp_write(0x12, pwr & ~0x04); return; }   // LDO2 off
  int mv = BACKLIGHT_MIN_MV + (3300 - BACKLIGHT_MIN_MV) * pct / 100;
  uint8_t step = (mv - 1800) / 100;                          // 0..15 -> 1.8..3.3 V
  axp_write(0x28, (axp_read(0x28) & 0x0F) | (step << 4));
  if (!(pwr & 0x04)) axp_write(0x12, pwr | 0x04);
}

// ---------- battery ----------
#define HAS_BATTERY 1
// pct: 3.30 V = 0 %, 4.15 V = 100 % (M5Unified's curve); false if there's no battery to report.
// Main loop only (shares Wire with set_brightness).
static bool battery(int &pct, bool &charging, int &mv) {
  uint8_t hi = axp_read(0x78), lo = axp_read(0x79);
  mv = ((hi << 4) | (lo & 0x0F)) * 11 / 10;          // 12-bit, 1.1 mV per step
  if (mv < 2500) return false;                       // no battery connected
  pct = constrain((mv - 3300) * 100 / (4150 - 3300), 0, 100);
  charging = axp_read(0x00) & 0x04;                  // battery current flowing in
  return true;
}

// ---------- buttons ----------
enum : uint8_t { BTN_A = 1, BTN_B = 2 };

struct BtnEvents { uint8_t pressed = 0, long_pressed = 0; };

// Debounced. `pressed` fires on release after a short press, `long_pressed` once after held 2 s.
// The debounce also hides the ESP32's GPIO36/39 glitches when WiFi powers the ADC (errata 3.11).
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
  axp_init();
  lcd.init();
  lcd.setRotation(1);                  // landscape; 3 flips it 180 degrees
  lcd.fillScreen(TFT_BLACK);
  set_brightness(BACKLIGHT_PCT);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  constexpr size_t buf_px = LCD_H_RES * 30;   // 14 KB
  static uint16_t *buf1 = (uint16_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf1) { Serial.println("[board] LVGL buffer alloc failed"); abort(); }
  Serial.printf("[board] M5StickC Plus %dx%d, heap free: %u\n", lcd.width(), lcd.height(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf1, nullptr, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

static void init() {
  pinMode(PIN_BTN_A, INPUT);   // input-only pins, pulled up on the board
  pinMode(PIN_BTN_B, INPUT);
  display_init();
}

}  // namespace board
