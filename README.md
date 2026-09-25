# CrowPanel Advance 7.0" Glucose Display

LVGL 9 firmware for the Elecrow CrowPanel Advance 7.0" (ESP32-S3-WROOM-1-N16R8, 800x480, GT911 touch).

- **Big screen**: current mg/dL in 200px digits, trend arrow, age of reading. Tap anywhere -> graph.
- **Graph screen**: 1h / 3h / 6h / 12h / 24h history, target band 70-200 shaded, threshold lines, `Back` button.
- Colors: red < 70, green 70-200, orange > 200, grey + warning banner if no reading for 15 min.
- Currently fed by **Home Assistant** (`include/ha_source.h`): polls `sensor.dexcom_glucose_value` / `_trend` every 60 s, backfills 24 h from `/api/history/period` on boot. Needs `include/secrets.h` (gitignored) with WiFi + HA URL + long-lived token. HA must be reachable over **plain HTTP on the same subnet** — HTTPS fails (TLS needs ~40 KB contiguous internal RAM we don't have) and cross-VLAN was blocked.
- `pio run -e dummy` builds a no-WiFi variant with a simulated generator instead.

## Display stability (hard-won)

The ESP32-S3 scans the RGB panel straight out of PSRAM; WiFi traffic competes for that bus and shows up as flicker/artifacts. What works:

- **Single framebuffer**, LVGL `PARTIAL` render mode into a 32 KB internal-SRAM buffer, flush waits for vsync. (Elecrow's double-buffer `DIRECT` mode flickers on every redraw when WiFi is up.)
- **`pclk_hz = 16 MHz`.** 18 (Elecrow's value) glitches under WiFi load; 14 makes the panel itself flicker badly.
- No idle redraws: labels only update when text actually changes; instant screen switch, no slide animation.
- `WiFi.persistent(false)` (no NVS flash writes), `WiFi.setSleep(false)`.
- If it ever regresses: Elecrow's `ESP32S3_120M` prebuilt libs (PSRAM 120 MHz + `SPIRAM_FETCH_INSTRUCTIONS`/`RODATA`) are the next lever.

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
