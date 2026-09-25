# CrowPanel Advance 7.0" Glucose Display

LVGL 9 firmware for the Elecrow CrowPanel Advance 7.0" (ESP32-S3-WROOM-1-N16R8, 800x480, GT911 touch).

- **Big screen**: current mg/dL in 200px digits, trend arrow, age of reading. Tap anywhere -> graph.
- **Graph screen**: 1h / 3h / 6h / 12h / 24h history, target band 70-200 shaded, threshold lines, `Back` button.
- Colors: red < 70, green 70-200, orange > 200, grey + warning banner if no reading for 15 min.
- Currently fed by **Home Assistant** (`include/ha_source.h`): polls `sensor.dexcom_glucose_value` / `_trend` every 60 s, backfills 24 h from `/api/history/period` on boot. Copy `include/secrets.h.example` to `include/secrets.h` (gitignored) and fill in WiFi + HA URL + long-lived token. HA must be reachable over **plain HTTP on the same subnet** — HTTPS fails (TLS needs ~40 KB contiguous internal RAM we don't have) and cross-VLAN was blocked.
- Brightness follows an HA helper `input_number.glucose_panel_brightness` (0–100), polled every 15 s — so night-dimming is just an HA automation.
- `pio run -e dummy` builds a no-WiFi variant with a simulated generator instead.

## Home Assistant setup

The panel is a pure display — HA owns the data. You need three things in HA: the Dexcom integration, a brightness helper, and an access token.

### 1. Dexcom integration

**Settings → Devices & Services → Add Integration → Dexcom.** Sign in with the Dexcom account credentials (the same login used for the Dexcom / Follow app) and pick your server region (US or outside-US). This uses Dexcom Share, so **Share must be enabled** in the wearer's Dexcom app with at least one follower.

It creates two entities (the name part depends on what you called the integration; the firmware defaults assume plain "Dexcom"):

| Entity | Example state |
|---|---|
| `sensor.dexcom_glucose_value` | `142` (mg/dL) |
| `sensor.dexcom_glucose_trend` | `falling_slightly` |

Check them under **Developer Tools → States**. If they show `unknown`, HA hasn't received a reading yet — the wearer's phone needs an internet connection for Share to update. If your entity IDs differ, set them in `secrets.h`.

Readings flow at Dexcom's 5-minute cadence. HA's recorder stores every value, so the panel's 1h–24h chart is backfilled from HA history on boot.

### 2. Brightness helper

**Settings → Devices & Services → Helpers → Create Helper → Number:**

- Name: `Glucose Panel Brightness` → entity ID `input_number.glucose_panel_brightness`
- Min `0`, max `100`, step `1`, display mode *slider*, unit `%`

The panel polls this every 15 s and sets the backlight. `0` is off. Put it on a dashboard and/or drive it from an automation, e.g.:

```yaml
automation:
  - alias: Glucose panel night dim
    triggers:
      - trigger: sun
        event: sunset
    actions:
      - action: input_number.set_value
        target: { entity_id: input_number.glucose_panel_brightness }
        data: { value: 10 }
  - alias: Glucose panel day bright
    triggers:
      - trigger: sun
        event: sunrise
    actions:
      - action: input_number.set_value
        target: { entity_id: input_number.glucose_panel_brightness }
        data: { value: 60 }
```

### 3. Access token

Click your user (bottom-left) → **Security** → **Long-lived access tokens** → Create. Copy it into `HA_TOKEN` in `secrets.h`; it's shown only once.

### 4. Network

`HA_BASE_URL` must be **`http://`** (not https) and the panel must be on the **same subnet** as HA:

- TLS needs ~40 KB of contiguous internal RAM that the display and WiFi stack already consume, so HTTPS connections fail with `SSL - Memory allocation failed`.
- IoT/guest VLANs typically block traffic to the main LAN. If the panel logs `history HTTP -1` after connecting to WiFi, this is why — either join the panel to the main network or add a firewall rule allowing it to reach HA's port 8123.

Use an IP or a hostname your router's DNS resolves for LAN clients (e.g. `http://192.168.1.50:8123`). `homeassistant.local` (mDNS) is unreliable on ESP32.

### Verifying

Serial monitor at 115200 after flashing should show:

```
[ha] wifi ip=192.168.1.x gw=... dns=...
[ha] history: 288 readings
[glucose] 142 mg/dL trend=5
[ha] brightness -> 60%
```

`trend=` values: 1 ⇈, 2 ↑, 3 ↗, 4 →, 5 ↘, 6 ↓, 7 ⇊. Until the first reading arrives the panel shows `---` / "waiting for data". If HA can't be reached, the corner reads "HA unreachable" and it keeps retrying.

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
cd <project dir>
cmd.exe /c "set MSYSTEM=& pio run -t upload"
```

(`pio` must be on PATH — `uv tool install platformio` puts it in `~/.local/bin`.)

Serial monitor: `pio device monitor -p COM5 -b 115200` (same cmd.exe wrapper).

## Restore factory firmware

```sh
esptool --port COM5 --baud 460800 write-flash 0 <your-backup>.bin
```

## Layout

- `include/board.h`   — display (esp_lcd RGB, single PSRAM framebuffer + LVGL partial render), GT911 touch, backlight (V1.0 TCA9534 or V1.2+ ctrl MCU @0x30)
- `include/glucose.h` — ring buffer, thresholds, trend calc, windowing, dummy source
- `include/ha_source.h` — WiFi/NTP, HA REST polling (readings, history backfill, brightness setting)
- `include/ui.h`      — both screens
- `src/font_glucose_200.c` — Montserrat-Medium 200px digits (`lv_font_conv`)
- `boards/`, `partitions.csv`, `sdkconfig.defaults*` — from Elecrow's official V1.3-1.5 PlatformIO example

## Support

If this saved you some time, you can [buy me a coffee on Ko-fi](https://ko-fi.com/mobreadmeo).

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/mobreadmeo)
