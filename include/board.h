// Board support for Elecrow CrowPanel Advance 7.0" (ESP32-S3-WROOM-1-N16R8, 800x480 RGB, GT911 touch)
// Pinout from Elecrow's official V1.3-1.5 PlatformIO example.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>

#define LCD_H_RES 800
#define LCD_V_RES 480

// I2C peripherals on SDA=15 SCL=16
#define I2C_SDA 15
#define I2C_SCL 16
#define I2C_ADDR_CTRL_MCU 0x30   // V1.2+ : backlight/reset helper MCU
#define I2C_ADDR_TCA9534  0x18   // V1.0  : IO expander (pin1 = backlight, pin2 = LCD reset)
#define I2C_ADDR_GT911    0x5D

namespace board {

// ---------- touch (GT911 via LovyanGFX) ----------
static lgfx::Touch_GT911 touch;

static void touch_init() {
  auto cfg = touch.config();
  cfg.x_min = 0; cfg.x_max = LCD_H_RES;
  cfg.y_min = 0; cfg.y_max = LCD_V_RES;
  cfg.pin_int = -1;
  cfg.pin_rst = -1;
  cfg.bus_shared = false;
  cfg.offset_rotation = 0;
  cfg.i2c_port = I2C_NUM_0;
  cfg.pin_sda = I2C_SDA;
  cfg.pin_scl = I2C_SCL;
  cfg.freq = 400000;
  cfg.i2c_addr = I2C_ADDR_GT911;
  touch.config(cfg);
  if (!touch.init()) Serial.println("[board] GT911 init failed");
}

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
  lgfx::touch_point_t p;
  if (touch.getTouchRaw(&p, 1)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = p.x;
    data->point.y = p.y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------- backlight / power ----------
static bool i2c_probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static void ctrl_mcu_cmd(uint8_t cmd) {
  Wire.beginTransmission(I2C_ADDR_CTRL_MCU);
  Wire.write(cmd);
  Wire.endTransmission();
}

// brightness: 0 = brightest ... 245 = off (per Elecrow's V1.2+ helper MCU protocol)
static void set_backlight_raw(uint8_t v) { ctrl_mcu_cmd(v); }

static void power_init() {
  pinMode(19, OUTPUT);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);

  if (i2c_probe(I2C_ADDR_TCA9534)) {
    // V1.0 hardware: TCA9534 expander. Reg 3 = config (0=out), reg 1 = output.
    Serial.println("[board] V1.0 (TCA9534) detected");
    Wire.beginTransmission(I2C_ADDR_TCA9534); Wire.write(0x03); Wire.write(0x00); Wire.endTransmission();
    // reset LCD (pin2 low->high), backlight on (pin1 high)
    Wire.beginTransmission(I2C_ADDR_TCA9534); Wire.write(0x01); Wire.write(0x02); Wire.endTransmission();
    delay(20);
    Wire.beginTransmission(I2C_ADDR_TCA9534); Wire.write(0x01); Wire.write(0x06); Wire.endTransmission();
    delay(50);
    return;
  }

  // V1.2+ hardware: helper MCU at 0x30. Wait for it and the touch controller.
  for (int tries = 0; tries < 50; ++tries) {
    if (i2c_probe(I2C_ADDR_CTRL_MCU) && i2c_probe(I2C_ADDR_GT911)) {
      Serial.println("[board] V1.2+ (ctrl MCU 0x30) detected");
      break;
    }
    ctrl_mcu_cmd(250);  // activate touch
    pinMode(1, OUTPUT); digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);
    delay(100);
  }
  set_backlight_raw(0);  // full brightness
}

// ---------- RGB panel via esp_lcd ----------
// One framebuffer in PSRAM that the LCD DMA scans continuously. LVGL renders into a
// small internal-SRAM buffer and only the dirty rectangle is copied to PSRAM. This keeps
// PSRAM traffic minimal so WiFi + LCD scan-out don't starve each other (DIRECT mode with
// two PSRAM buffers renders *and* syncs in PSRAM and flickers whenever WiFi is busy).
static esp_lcd_panel_handle_t rgb_panel = nullptr;
static SemaphoreHandle_t vsync_sem = nullptr;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsync_sem, &woken);
  return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  // Copy into the framebuffer right after vsync, while the DMA is scanning the top of the
  // frame -- minimizes the chance of writing a line the LCD is reading at the same moment.
  xSemaphoreTake(vsync_sem, 0);
  xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(40));
  esp_lcd_panel_draw_bitmap(rgb_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
  lv_display_flush_ready(disp);
}

static void display_init() {
  vsync_sem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 16000000;   // Elecrow uses 18; 14 flickered badly. 16 = less PSRAM DMA bandwidth for the scan-out
  cfg.timings.h_res = LCD_H_RES;
  cfg.timings.v_res = LCD_V_RES;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.hsync_idle_low = 0;
  cfg.timings.flags.vsync_idle_low = 0;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.timings.flags.pclk_idle_high = 1;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = LCD_H_RES * 10;   // 16 KB internal SRAM staging for the scan-out DMA
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = GPIO_NUM_40;
  cfg.vsync_gpio_num = GPIO_NUM_41;
  cfg.de_gpio_num = GPIO_NUM_42;
  cfg.pclk_gpio_num = GPIO_NUM_39;
  cfg.disp_gpio_num = -1;
  const int pins[16] = {21, 47, 48, 45, 38,      // B0-B4
                        9, 10, 11, 12, 13, 14,   // G0-G5
                        7, 17, 18, 3, 46};       // R0-R4
  for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = pins[i];
  cfg.flags.fb_in_psram = 1;
  cfg.flags.bb_invalidate_cache = 1;

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &rgb_panel));
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = on_vsync;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(rgb_panel, &cbs, nullptr));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(rgb_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(rgb_panel));

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  // clear the scan-out framebuffer so we don't show PSRAM garbage before LVGL's first frame
  void *fb = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(rgb_panel, 1, &fb));
  memset(fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));

  // LVGL render buffer in internal SRAM (DMA-capable); 20 lines = 32 KB. WiFi + LCD bounce
  // buffers also need internal RAM, so keep this modest.
  constexpr size_t buf_px = LCD_H_RES * 20;
  static uint16_t *buf1 = (uint16_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf1) { Serial.println("[board] LVGL buffer alloc failed"); abort(); }
  Serial.printf("[board] internal heap free: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf1, nullptr, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
}

static void init() {
  power_init();
  touch_init();
  display_init();
}

}  // namespace board
