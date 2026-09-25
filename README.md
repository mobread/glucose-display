# CrowPanel Advance 7.0" Glucose Display

LVGL 9 firmware for the Elecrow CrowPanel Advance 7.0" (ESP32-S3-WROOM-1-N16R8, 800x480, GT911 touch).

- **Big screen**: current mg/dL in 200px digits, trend arrow, age of reading. Tap anywhere -> graph.
- **Graph screen**: 1h / 3h / 6h / 12h / 24h history, target band 70-200 shaded, threshold lines, `Back` button.
- Colors: red < 70, green 70-200, orange > 200, grey + warning banner if no reading for 15 min.
- Currently fed by a **dummy generator** (`include/glucose.h`, `glucose::dummy`) that emits a new 5-minute reading every 10 s of real time.

## Build / flash (Windows, from git-bash)

pioarduino's tool installer refuses to run when `MSYSTEM` is set, so run pio through cmd.exe with it cleared:

```sh
cd C:/Users/micha/claude-projects/crowpanel/glucose-display
cmd.exe /c "set MSYSTEM=& set PATH=C:\Users\micha\.local\bin;%PATH%& pio run -t upload"
```

Serial monitor: `pio device monitor -p COM5 -b 115200` (same cmd.exe wrapper).

## Restore factory firmware

```sh
esptool --port COM5 --baud 460800 write-flash 0 ../crowpanel_factory_backup.bin
```

## Layout

- `include/board.h`   — display (esp_lcd RGB, double-buffered in PSRAM), GT911 touch, backlight (V1.0 TCA9534 or V1.2+ ctrl MCU @0x30)
- `include/glucose.h` — ring buffer, thresholds, trend calc, windowing, dummy source
- `include/ui.h`      — both screens
- `src/font_glucose_200.c` — Montserrat-Medium 200px digits (`lv_font_conv`)
- `boards/`, `partitions.csv`, `sdkconfig.defaults*` — from Elecrow's official V1.3-1.5 PlatformIO example

## Next: real data from Home Assistant

Replace `glucose::dummy` with a source that:
1. Connects WiFi, syncs NTP.
2. Every 5 min GETs `http://<ha>/api/states/sensor.dexcom_<name>_glucose_value` (Bearer long-lived token) for value + trend attribute.
3. On boot, GETs `/api/history/period/<24h ago>?filter_entity_id=...` to backfill the chart.
