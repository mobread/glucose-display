// Board support for Waveshare ESP32-P4-WIFI6-Touch-LCD-5
// (ESP32-P4 rev3.x, 720x1280 HX8394 MIPI-DSI 2-lane panel, GT911 touch, ESP32-C6 hosted WiFi).
// Pinout + panel init from Waveshare's official ESP32-P4-WIFI6-Touch-LCD-5 Arduino examples.
//
// The panel is natively portrait (720x1280). The UI runs in landscape (1280x720): LVGL renders
// into a small internal-SRAM buffer and flush_cb rotates each dirty rectangle into the single
// PSRAM framebuffer that the DPI controller scans out.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_ldo_regulator.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>

#define PANEL_W 720     // physical (portrait)
#define PANEL_H 1280
#define UI_W 1280       // logical (landscape)
#define UI_H 720

// 1 = rotate 90° (panel's top edge on the right), 0 = rotate 270° (top edge on the left).
// Flip this if the picture is upside down for how the panel is mounted.
#ifndef UI_ROTATE_90
#define UI_ROTATE_90 1
#endif

#define I2C_SDA 7
#define I2C_SCL 8
#define PIN_LCD_RST 27       // HX8394 reset, ACTIVE HIGH on this board
#define PIN_TP_RST 23         // GT911 reset (active low)
#define PIN_BACKLIGHT 26     // LEDC PWM (backlight path is AC-coupled: needs PWM, not a static level)
#define BL_FREQ 5000
#define BL_RES 10

namespace board {

// ---------- backlight ----------
static void set_brightness(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const uint32_t max_duty = (1u << BL_RES) - 1;
  ledcWrite(PIN_BACKLIGHT, max_duty * pct / 100);
}

#ifndef BACKLIGHT_PCT
#define BACKLIGHT_PCT 10
#endif

// ---------- touch (GT911, polled over I2C) ----------
static uint8_t gt911_addr = 0;

static bool gt911_read(uint16_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(gt911_addr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(gt911_addr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static void gt911_write8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gt911_addr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(v);
  Wire.endTransmission();
}

static void touch_init() {
  // Hardware-reset the GT911 on every boot: a P4 reset (flash, watchdog, brownout) doesn't reset
  // the touch controller, which can otherwise come up wedged. TP_RST -> GPIO23 (R37, 0R).
  // TP_INT isn't reliably connected (R108 is NC/0R), so the address it latches can be either legal
  // GT911 address -- probe both afterwards.
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(20);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(100);   // GT911 needs >= 50 ms after reset before I2C is ready
  gt911_addr = 0;
  const uint8_t addrs[2] = {0x5D, 0x14};
  for (int tries = 0; tries < 5 && !gt911_addr; tries++) {
    for (uint8_t a : addrs) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { gt911_addr = a; break; }
    }
    if (!gt911_addr) delay(50);
  }
  if (gt911_addr) {
    gt911_write8(0x814E, 0);   // clear any stale "buffer ready" flag
    Serial.printf("[board] GT911 at 0x%02X\n", gt911_addr);
  } else {
    Serial.println("[board] GT911 not found; touch disabled");
  }
}

// physical (portrait) touch -> logical (landscape) UI coordinates
static void touch_to_ui(int tx, int ty, int32_t &x, int32_t &y) {
#if UI_ROTATE_90
  x = ty;               y = PANEL_W - 1 - tx;
#else
  x = PANEL_H - 1 - ty; y = tx;
#endif
}

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
  static int32_t last_x = 0, last_y = 0;
  static uint32_t last_poll = 0;
  static bool pressed = false;
  if (!gt911_addr) { data->state = LV_INDEV_STATE_RELEASED; return; }
  // GT911 updates at ~100 Hz; don't hammer the bus every LVGL tick
  if (millis() - last_poll >= 10) {
    last_poll = millis();
    uint8_t st = 0;
    bool ok = gt911_read(0x814E, &st, 1);
    static int fails = 0;
    if (!ok && ++fails >= 50) {   // ~0.5 s of bus errors: controller wedged, reset it
      Serial.println("[board] GT911 not responding; resetting");
      fails = 0;
      pressed = false;
      touch_init();
    } else if (ok) {
      fails = 0;
    }
#ifdef TOUCH_DEBUG
    static uint8_t last_st = 0xAA; static bool last_ok = true;
    if (st != last_st || ok != last_ok) { Serial.printf("[touch] ok=%d st=0x%02X\n", ok, st); last_st = st; last_ok = ok; }
#endif
    if (ok && (st & 0x80)) {
      uint8_t n = st & 0x0F;
      if (n > 0) {
        uint8_t p[4];
        if (gt911_read(0x8150, p, 4)) {
          touch_to_ui(p[0] | (p[1] << 8), p[2] | (p[3] << 8), last_x, last_y);
#ifdef TOUCH_DEBUG
          Serial.printf("[touch] raw=%d,%d ui=%ld,%ld\n", p[0] | (p[1] << 8), p[2] | (p[3] << 8), (long)last_x, (long)last_y);
#endif
          pressed = true;
        }
      } else {
        pressed = false;
      }
      gt911_write8(0x814E, 0);   // ack: let the controller report the next frame
    }
  }
  data->point.x = last_x;
  data->point.y = last_y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---------- MIPI-DSI panel ----------
struct InitCmd { uint8_t cmd; const uint8_t *data; uint8_t len; uint16_t delay_ms; };
#define D(...) (const uint8_t[]){__VA_ARGS__}
static const InitCmd hx8394_init[] = {
  {0x11, D(0x00), 1, 120},
  {0x36, D(0x00), 1, 0},
  {0x3A, D(0x55), 1, 0},
  {0xBA, D(0x61), 1, 0},
  {0xB9, D(0xFF, 0x83, 0x94), 3, 0},
  {0xB1, D(0x48, 0x0A, 0x6A, 0x09, 0x33, 0x54, 0x71, 0x71, 0x2E, 0x45), 10, 0},
  {0xBA, D(0x61, 0x03, 0x68, 0x6B, 0xB2, 0xC0), 6, 0},
  {0xB2, D(0x00, 0x80, 0x64, 0x0C, 0x06, 0x2F), 6, 0},
  {0xB4, D(0x1C, 0x78, 0x1C, 0x78, 0x1C, 0x78, 0x01, 0x0C, 0x86, 0x75, 0x00, 0x3F, 0x1C, 0x78, 0x1C, 0x78, 0x1C, 0x78, 0x01, 0x0C, 0x86), 21, 0},
  {0xD3, D(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x32, 0x10, 0x05, 0x00, 0x05, 0x32, 0x13, 0xC1, 0x00, 0x01, 0x32, 0x10, 0x08, 0x00, 0x00, 0x37, 0x03, 0x07, 0x07, 0x37, 0x05, 0x05, 0x37, 0x0C, 0x40), 33, 0},
  {0xD5, D(0x18, 0x18, 0x18, 0x18, 0x22, 0x23, 0x20, 0x21, 0x04, 0x05, 0x06, 0x07, 0x00, 0x01, 0x02, 0x03, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19), 44, 0},
  {0xD6, D(0x18, 0x18, 0x19, 0x19, 0x21, 0x20, 0x23, 0x22, 0x03, 0x02, 0x01, 0x00, 0x07, 0x06, 0x05, 0x04, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x18, 0x18), 44, 0},
  {0xE0, D(0x07, 0x08, 0x09, 0x0D, 0x10, 0x14, 0x16, 0x13, 0x24, 0x36, 0x48, 0x4A, 0x58, 0x6F, 0x76, 0x80, 0x97, 0xA5, 0xA8, 0xB5, 0xC6, 0x62, 0x63, 0x68, 0x6F, 0x72, 0x78, 0x7F, 0x7F, 0x00, 0x02, 0x08, 0x0D, 0x0C, 0x0E, 0x0F, 0x10, 0x24, 0x36, 0x48, 0x4A, 0x58, 0x6F, 0x78, 0x82, 0x99, 0xA4, 0xA0, 0xB1, 0xC0, 0x5E, 0x5E, 0x64, 0x6B, 0x6C, 0x73, 0x7F, 0x7F), 58, 0},
  {0xCC, D(0x0B), 1, 0},
  {0xC0, D(0x1F, 0x73), 2, 0},
  {0xB6, D(0x6B, 0x6B), 2, 0},
  {0xD4, D(0x02), 1, 0},
  {0xBD, D(0x01), 1, 0},
  {0xB1, D(0x00), 1, 0},
  {0xBD, D(0x00), 1, 0},
  {0xBF, D(0x40, 0x81, 0x50, 0x00, 0x1A, 0xFC, 0x01), 7, 0},
  {0x3A, D(0x50), 1, 0},
  {0x11, D(0x00), 1, 200},
  {0xB2, D(0x00, 0x80, 0x64, 0x0C, 0x06, 0x2F, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x18), 12, 0},
  {0x29, D(0x00), 1, 80},
};
#undef D

static esp_lcd_panel_handle_t dpi_panel = nullptr;
static uint16_t *fb = nullptr;   // PANEL_W x PANEL_H RGB565, scanned out continuously by the DPI DMA

#define CHECK(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { \
  Serial.printf("[board] %s failed: %s\n", #x, esp_err_to_name(e_)); abort(); } } while (0)

static void panel_init() {
  // HX8394 reset is active-high: release, assert 10 ms, release, settle
  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, LOW);  delay(10);
  digitalWrite(PIN_LCD_RST, HIGH); delay(10);
  digitalWrite(PIN_LCD_RST, LOW);  delay(120);

  // MIPI D-PHY is powered from on-chip LDO channel 3 @ 2.5 V
  static esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t ldo_cfg = {};
  ldo_cfg.chan_id = 3;
  ldo_cfg.voltage_mv = 2500;
  CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = 2;
  bus_cfg.phy_clk_src = (mipi_dsi_phy_pllref_clock_source_t)0;   // default for this chip revision
  bus_cfg.lane_bit_rate_mbps = 700;
  esp_lcd_dsi_bus_handle_t bus = nullptr;
  CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus));

  esp_lcd_dbi_io_config_t dbi_cfg = {};
  dbi_cfg.virtual_channel = 0;
  dbi_cfg.lcd_cmd_bits = 8;
  dbi_cfg.lcd_param_bits = 8;
  esp_lcd_panel_io_handle_t io = nullptr;
  CHECK(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io));

  esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.virtual_channel = 0;
  dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = 58;
  dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dpi_cfg.num_fbs = 1;
  dpi_cfg.video_timing.h_size = PANEL_W;
  dpi_cfg.video_timing.v_size = PANEL_H;
  dpi_cfg.video_timing.hsync_pulse_width = 20;
  dpi_cfg.video_timing.hsync_back_porch = 20;
  dpi_cfg.video_timing.hsync_front_porch = 40;
  dpi_cfg.video_timing.vsync_pulse_width = 4;
  dpi_cfg.video_timing.vsync_back_porch = 10;
  dpi_cfg.video_timing.vsync_front_porch = 24;
  CHECK(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &dpi_panel));

  for (const InitCmd &c : hx8394_init) {
    CHECK(esp_lcd_panel_io_tx_param(io, c.cmd, c.data, c.len));
    if (c.delay_ms) delay(c.delay_ms);
  }
  CHECK(esp_lcd_panel_init(dpi_panel));

  void *p = nullptr;
  CHECK(esp_lcd_dpi_panel_get_frame_buffer(dpi_panel, 1, &p));
  fb = (uint16_t *)p;
  memset(fb, 0, PANEL_W * PANEL_H * 2);
  esp_cache_msync(fb, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// LVGL area is in logical landscape coordinates; rotate into the portrait framebuffer.
static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px_map) {
  const uint16_t *src = (const uint16_t *)px_map;
  const int w = a->x2 - a->x1 + 1;
  // Each logical column lx is one framebuffer row (py); walk it so writes are sequential.
  for (int lx = a->x1; lx <= a->x2; lx++) {
    const uint16_t *s = src + (lx - a->x1);
#if UI_ROTATE_90
    // (lx, ly) -> (px = PANEL_W-1-ly, py = lx)
    uint16_t *d = fb + lx * PANEL_W + (PANEL_W - 1 - a->y1);
    for (int ly = a->y1; ly <= a->y2; ly++, s += w) *d-- = *s;
#else
    // (lx, ly) -> (px = ly, py = PANEL_H-1-lx)
    uint16_t *d = fb + (PANEL_H - 1 - lx) * PANEL_W + a->y1;
    for (int ly = a->y1; ly <= a->y2; ly++, s += w) *d++ = *s;
#endif
  }
  // The DPI DMA reads the framebuffer from PSRAM directly, so the CPU cache must be written back.
  // Do it once per refresh (after the last dirty chunk) for the whole buffer: cheap (only dirty
  // lines are written) and it can't miss a region the way per-chunk ranges can.
  if (lv_display_flush_is_last(disp)) {
    esp_err_t e = esp_cache_msync(fb, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    static bool reported = false;
    if (e != ESP_OK && !reported) { reported = true; Serial.printf("[board] cache msync: %s\n", esp_err_to_name(e)); }
  }
  lv_display_flush_ready(disp);
}

static void display_init() {
  panel_init();

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  // 40 landscape lines = 100 KB in internal SRAM (fast to render into; P4 has plenty)
  constexpr size_t buf_px = UI_W * 40;
  static uint16_t *buf1 = (uint16_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf1) { Serial.println("[board] LVGL buffer alloc failed"); abort(); }
  Serial.printf("[board] internal heap free: %u, psram free: %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  lv_display_t *disp = lv_display_create(UI_W, UI_H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf1, nullptr, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
}

static void init() {
  ledcAttach(PIN_BACKLIGHT, BL_FREQ, BL_RES);
  set_brightness(0);                 // dark until the first frame is up
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  touch_init();
  display_init();
  lv_timer_handler();
  set_brightness(BACKLIGHT_PCT);
}

#undef CHECK
}  // namespace board
