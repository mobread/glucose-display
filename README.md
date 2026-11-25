# Glucose Display (CrowPanel Advance 7" / Waveshare ESP32-P4 5" / LilyGo T-Display-S3 / M5StickC Plus / M5StickS3)

LVGL 9 firmware for a bedside/desk Dexcom glucose display fed by Home Assistant. Two boards:

| Board | PlatformIO env | Notes |
|---|---|---|
| Elecrow CrowPanel Advance 7.0" (ESP32-S3-WROOM-1-N16R8, 800x480 RGB, GT911) | `crowpanel-advance-7`, `dummy` | home LAN only |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (ESP32-P4 rev3, 720x1280 MIPI-DSI HX8394, GT911, ESP32-C6 WiFi) | `waveshare-p4-5`, `waveshare-p4-5-dummy` | runs landscape 1280x720; on-device WiFi setup; optional WireGuard tunnel home — [details](#waveshare-esp32-p4-wifi6-touch-lcd-5) |
| LilyGo T-Display-S3 (ESP32-S3R8, 1.9" 170x320 ST7789, 8-bit parallel, no touch) | `tdisplay-s3`, `tdisplay-s3-dummy` | runs landscape 320x170 with its own compact layout; side buttons instead of touch; home LAN only — [details](#lilygo-t-display-s3) |
| M5StickC Plus (ESP32-PICO-D4, 1.14" 135x240 ST7789, AXP192, no PSRAM) | `m5stickc-plus`, `m5stickc-plus-dummy` | landscape 240x135, big number + arrow only (no graph); optional WireGuard tunnel home — [details](#m5stickc-plus) |
| M5Stack StickS3 (ESP32-S3-PICO-1-N8R8, 1.14" 135x240 ST7789P3, M5PM1) | `m5sticks3`, `m5sticks3-dummy` | same big-number screen as the StickC Plus; WiFi set up from a phone through a captive portal; optional WireGuard — [details](#m5stack-sticks3) |

- **Big screen**: current mg/dL in 200px digits, trend arrow, age of reading. Tap anywhere -> graph.
- **Graph screen**: 1h / 3h / 6h / 12h / 24h history, target band 70-200 shaded, threshold lines, `Back` button.
- Colors: red < 70, green 70-200, orange > 200, grey + warning banner if no reading for 15 min.
- Currently fed by **Home Assistant** (`include/ha_source.h`): polls `sensor.dexcom_glucose_value` / `_trend` every 60 s, backfills 24 h from `/api/history/period` on boot. Copy `include/secrets.h.example` to `include/secrets.h` (gitignored) and fill in WiFi + HA URL + long-lived token. On the CrowPanel, HA must be reachable over **plain HTTP on the same subnet** — HTTPS fails (TLS needs ~40 KB contiguous internal RAM we don't have) and cross-VLAN was blocked. The Waveshare build can reach HA from anywhere through WireGuard (below).
- Brightness follows an HA helper `input_number.glucose_panel_brightness` (0–100), polled every 15 s — so night-dimming is just an HA automation.
- `pio run -e dummy` (or `-e waveshare-p4-5-dummy`) builds a no-WiFi variant with a simulated generator instead.

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

## Waveshare ESP32-P4-WIFI6-Touch-LCD-5

Same UI scaled 1.5x (`S()` in `ui.h`, 360px main digits), plus features for running away from home.

### Board specifics

- **Chip revision matters.** These boards ship with ESP32-P4 **rev3.x** silicon. The env uses pioarduino `55.03.313` (Arduino-ESP32 3.3.13 / ESP-IDF 5.5) and board `esp32-p4_r3` ("v3.00 or newer" variant). A pre-v3 build refuses to boot on it. Needs PlatformIO Core >= 6.2.
- **Display**: 720x1280 portrait HX8394 over 2-lane MIPI-DSI (700 Mbps, 58 MHz DPI), init sequence from Waveshare's official LCD-5 examples, driven with esp_lcd directly (no Arduino_GFX). LVGL renders 1280x720 landscape into a 40-line internal-SRAM buffer; `flush_cb` rotates each strip into the single PSRAM framebuffer and writes back the cache once per refresh. `UI_ROTATE_90` in `board_waveshare_p4_5.h` flips which edge is up.
- **Touch**: GT911 on I2C GPIO7/8, polled. Its reset (GPIO23) is pulsed on every boot — without it the controller can come up wedged after a soft reset — and it is re-reset automatically if it stops answering.
- **Backlight**: LEDC PWM on GPIO26 (must be PWM; the path is AC-coupled). Same 0–100 HA brightness helper.
- **WiFi** comes from the on-board ESP32-C6 via esp-hosted, through the normal Arduino `WiFi` API. **2.4 GHz only** (iPhone hotspots: turn on *Maximize Compatibility*).
- LVGL uses the system heap (spills to PSRAM) instead of a fixed pool. Don't use LVGL style transforms (rotation) on large objects: each one renders through an ARGB8888 layer, and in LVGL 9.1 a failed layer allocation hangs the render loop — the trend arrows rotate their points in software for this reason.

### WiFi setup on the device

`include/wifi_mgr.h` + `include/ui_wifi.h`.

- If no network is configured, or the configured one can't be joined and the panel hasn't been online since boot, the **WiFi setup screen opens by itself** (~20 s).
- Any time: tap the main screen → graph → **WiFi** button next to `Back`.
- Pick from the scanned list (strongest first) or type a hidden network's name; on-screen keyboard; eye button shows the password.
- **Saved networks**: up to 8 networks with passwords are kept in NVS, most recently used first. They're tagged `saved` in the list; tapping one pre-fills its password, so switching is just **Connect**. **Forget** removes the network in the name field.
- Credentials are saved **only after they actually connect**, so a typo can't replace a working network. At boot the most recent saved network is joined; if the current network can't be reached, the strongest visible saved network is tried instead (unless you just picked that network yourself). `WIFI_SSID`/`WIFI_PASS` from `secrets.h` are just the fallback (leave them `""` to always start in setup). Reflashing keeps saved networks; `esptool erase-flash` clears them.
- The built-in fonts are ASCII-only, so typographic quotes in SSIDs (iPhone hotspots use `’`) are shown as `'`; the exact name is still used to connect.

### WireGuard tunnel home

`include/wg_tunnel.h`, using [`esphome/wireguard`](https://github.com/esphome-libs/wireguard). Lets the panel reach HA's plain-HTTP LAN address from any network.

1. On the WireGuard server, create a peer for the panel and export its wg-quick `.conf`.
2. Copy the values into `secrets.h` (`WG_*` block; mapping is in `secrets.h.example`) and set `WG_ENABLE 1`.
   `AllowedIPs` isn't needed — the panel routes everything through the tunnel.
3. Keep `HA_BASE_URL` as the LAN address. A LAN-only hostname (e.g. `http://ha.home.lan:8123`) works when `WG_DNS` is set to the home DNS server (the `.conf`'s `DNS =`).
4. The server must allow the panel's tunnel IP to reach HA on port 8123 (e.g. add it to the firewall alias/rule your other VPN clients use).

How it runs: WiFi → NTP (handshakes fail with a wrong clock) → tunnel up → tunnel becomes the default route and `WG_DNS` the DNS server → HA polling as usual. WireGuard's own UDP stays bound to WiFi. Whenever WiFi comes back (reconnect or a different network) the tunnel is rebuilt with a fresh handshake, resolving the endpoint with the new network's DNS; the route and tunnel DNS are also re-checked every 10 s (DHCP resets both), and a peer that stays down 2 min is rebuilt.

### Status

Top-right on both screens: **WiFi**, **lock** (VPN, only when WireGuard is enabled) and **house** (Home Assistant) icons — green = OK, red = down. The house turns red when an HA request fails (timeout, bad token, HTTP error). The status text explains: `WiFi: …`, `VPN connecting` / `VPN down`, `HA unreachable`, or `Dexcom: no data` (HA answered but the Dexcom sensor is `unknown`/`unavailable` — Share or the phone isn't delivering readings). Icons are a 22 px Font Awesome subset (`src/font_status_icons.c`).

Debugging: `PLATFORMIO_BUILD_FLAGS="-DWG_DEBUG -DLOG_LOCAL_LEVEL=4" pio run -e waveshare-p4-5 -t upload` prints the library's handshake log. `sending initiation packet` every 5 s with no `tunnel up` means the server isn't answering: check that the peer's public key on the server matches the panel's private key, and the firewall/port forward. Limitation: if the remote WiFi uses the same subnet as the home LAN, HA's address is treated as local and bypasses the tunnel.

### Build / flash (macOS)

```sh
pio run -e waveshare-p4-5 -t upload --upload-port /dev/cu.usbmodem*   # use the board's UART USB-C port
pio device monitor -p /dev/cu.usbmodem* -b 115200
```

Long flash reads/writes through a USB hub can fail with "serial noise or corruption"; 460800 baud is reliable, or read in 1 MB chunks. Restore the factory image with `esptool --port <port> --baud 460800 write-flash 0 <backup>.bin` (factory partitions span the first 16 MB).

## LilyGo T-Display-S3

A pocket-size version: 1.9" ST7789 (170x320, run as 320x170 landscape) on the ESP32-S3's 8-bit parallel LCD bus, driven by LovyanGFX (`include/board_tdisplay_s3.h`). The 800x480 layout doesn't scale down to 170 px tall, so `ui.h` has a compact layout for panels under 240 px tall (`UI_COMPACT`):

- **Big screen**: 112 px digits (`src/font_glucose_112.c`) with the trend arrow beside them, `mg/dL` bottom-left, the reading's age bottom-right, the status text top-left and the WiFi/HA icons top-right.
- **Graph screen**: the value, arrow and age in a header row, the current window (`3h`) top-right, and a chart with the 70–200 band underneath.

**Buttons** (USB-C facing right):

| Button | Big screen | Graph screen |
|---|---|---|
| Bottom (BOOT, GPIO0) | open graph | back to big screen |
| Top (KEY, GPIO14) | open graph | next window: 1h → 3h → 6h → 12h → 24h |

**Network**: no WireGuard on this build, so it must join the same LAN as Home Assistant (plain HTTP, same rules as the CrowPanel). Because it shares `secrets.h` with the P4, it has its own WiFi override there under `#if defined(BOARD_TDISPLAY_S3)`; leave the main `WIFI_SSID` for the P4.

**Build / flash (macOS)**: plug in USB-C; it shows up as `/dev/cu.usbmodem*` and flashes without pressing any buttons.

```sh
pio run -e tdisplay-s3 -t upload           # or -e tdisplay-s3-dummy for simulated data
pio device monitor -e tdisplay-s3
```

To go back to the MicroPython apps in `../esp-lcd`, run its `flash.sh` and then `deploy.sh <app>`.

## M5StickC Plus

The smallest build: 1.14" ST7789V2 (135x240, run as 240x135 landscape) over SPI, driven by LovyanGFX (`include/board_m5stickc_plus.h`). It shows only the **big-number screen** — current value in 90 px digits (`src/font_glucose_90.c`, sized so `400` + arrow fits in 240 px) with the trend arrow beside it, `mg/dL` bottom-left, the reading's age bottom-right, status text top-left and WiFi / VPN / HA icons top-right. The board sets `UI_NO_GRAPH`, so `ui.h` never builds the graph screen, and it backfills only 2 h of history (`HISTORY_HOURS`) — enough for the trend fallback — to save RAM. Buttons are unused.

- **Power / backlight**: the LCD and backlight run off the AXP192 PMU (I2C 0x34 on GPIO21/22). Brightness drives LDO2 between 2.9 V (`BACKLIGHT_MIN_MV`; the LED is barely lit below that) and 3.3 V; 0 % switches LDO2 off. The AXP192's side button still powers the stick on (short press) / off (hold 6 s).
- **Memory**: no PSRAM, so LVGL gets a 40 KB pool and a 14 KB draw buffer; ~200 KB internal heap is free at boot before WiFi.
- **WireGuard**: same tunnel code as the P4 (`include/wg_tunnel.h`). Give it its **own peer** — two devices sharing one key fight over the session. `secrets.h` has a per-board override block under `#if defined(BOARD_M5STICKC_PLUS)` for the `WG_*` values; the server must let the new tunnel IP reach HA on port 8123.
- **Flash**: 4 MB, so it uses the `huge_app.csv` table (3 MB app, no OTA).

```sh
pio run -e m5stickc-plus -t upload --upload-port /dev/cu.usbserial-*   # or -e m5stickc-plus-dummy
pio device monitor -p /dev/cu.usbserial-* -b 115200
```

If the link fails with `ed25519_... file in wrong format` after building the P4: the WireGuard library writes objects to a shared `.pio/build/port` folder, so P4 (RISC-V) and M5 (Xtensa) builds clash. `rm -rf .pio/build/port` and build again.

## M5Stack StickS3

Same big-number-only screen as the M5StickC Plus, on an ESP32-S3 with 8 MB PSRAM (`include/board_m5sticks3.h`). Display and power values come from M5GFX's StickS3 board data, since the official docs leave gaps:

- **Power**: the LCD and backlight are on the M5PM1 PMIC's L3B rail (I2C 0x6E on GPIO47/48). It is switched on by making PMIC GPIO2 a push-pull output and driving it **high**; until then the panel is dead.
- **Display**: ST7789P3 over SPI (SCLK 40, MOSI 39, DC 45, CS 41, RST 21), offsets 52/40 like the StickC Plus. The backlight is PWM on GPIO38 (256 Hz). 1 % brightness starts at `BACKLIGHT_MIN_DUTY` so low HA settings stay readable.
- **Buttons**: A = GPIO11 (front), B = GPIO12 (side), active low. Hold **A** for 2 s to open/close the WiFi setup hotspot. Hold **B** for 2 s to forget every saved network (wiped from flash) and go back to setup.

### WiFi setup (captive portal)

`include/wifi_portal.h`, enabled per board with `WIFI_PORTAL 1`. There's no built-in network for this board in `secrets.h`, so a fresh stick goes straight to setup:

1. The stick opens an open access point **`Glucose-XXXX`** (the last 4 hex digits of its MAC) and the screen says to join it.
2. Join it from a phone. The stick answers every DNS lookup with its own address and redirects the OS connectivity checks, so the sign-in sheet pops up (otherwise open http://192.168.4.1).
3. Pick a network from the scan (or type a hidden one), enter the password, and tap **Connect**. The page polls and shows success or the reason it failed.
4. Credentials go through `wifi_mgr.h`, so they're saved only after they actually connect, alongside up to 8 saved networks. The hotspot closes 20 s after connecting.

To start over (new router, moving it to another network), hold **B** for 2 s: all saved networks are erased, WiFi disconnects and the hotspot opens. Holding **A** instead opens the hotspot but keeps the saved networks, so if you close it again (hold A) the stick goes back to what it had.

The hotspot also reopens by itself if WiFi has been down for 45 s (`PORTAL_AFTER_MS`). While it's up, the saved network is retried once a minute without scanning, so the phone isn't dropped. If that network comes back, the hotspot closes again. The portal runs in its own task, so the page stays responsive while the HA task blocks on HTTP.

**WireGuard**: its own peer in the `#if defined(BOARD_M5STICKS3)` block in `secrets.h`; the server must let that tunnel IP reach HA.

**Data source**: the same `secrets.h` block can point the stick at different HA entities. Ours shows a Medtronic pump through the [CareLink integration](https://github.com/yo-han/Home-Assistant-Carelink) instead of the Dexcom sensors:
- `HA_ENTITY_VALUE` = `..._last_glucose_level_mg_dl`
- `HA_ENTITY_TREND` = `..._last_glucose_trend`. CareLink's `UP` / `UP_DOUBLE` / `UP_TRIPLE` (1/2/3 arrows, ≥1/2/3 mg/dL/min) map to the diagonal / single / double arrows; `NONE` / `FLAT` = steady.
- `HA_ENTITY_TIMESTAMP` = `..._last_glucose_update`. Its **last_changed** is used as the reading time, because the value's own last_changed doesn't move when two readings in a row are equal. Its *state* is not used: CareLink converts the pump's local time with HA's timezone, so it's off by the difference between the two (an hour here).

With a timestamp entity the stick skips the history backfill and starts from the current reading.

**Flashing**: native USB-C (`/dev/cu.usbmodem*`). If the stick doesn't show up, hold the power button until the green LED blinks (download mode). Afterwards it stays in download mode, even after an esptool/RTS reset, until you **single-click** the power button.

```sh
pio run -e m5sticks3 -t upload --upload-port /dev/cu.usbmodem*   # or -e m5sticks3-dummy
```

Restore UiFlow2 from a full-flash backup with `esptool --port <port> write-flash 0 <backup>.bin`.

## Display stability (CrowPanel, hard-won)

The ESP32-S3 scans the RGB panel straight out of PSRAM; WiFi traffic competes for that bus and shows up as flicker/artifacts. What works:

- **Single framebuffer**, LVGL `PARTIAL` render mode into a 32 KB internal-SRAM buffer, flush waits for vsync. (Elecrow's double-buffer `DIRECT` mode flickers on every redraw when WiFi is up.)
- **`pclk_hz = 16 MHz`.** 18 (Elecrow's value) glitches under WiFi load; 14 makes the panel itself flicker badly.
- No idle redraws: labels only update when text actually changes; instant screen switch, no slide animation.
- `WiFi.persistent(false)` (no NVS flash writes), `WiFi.setSleep(false)`.
- If it ever regresses: Elecrow's `ESP32S3_120M` prebuilt libs (PSRAM 120 MHz + `SPIRAM_FETCH_INSTRUCTIONS`/`RODATA`) are the next lever.

## Build / flash (CrowPanel, Windows, from git-bash)

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

- `include/board.h`   — picks the board header (`BOARD_WAVESHARE_P4_5` or CrowPanel)
- `include/board_tdisplay_s3.h` — LilyGo T-Display-S3: ST7789 over `Bus_Parallel8` (LovyanGFX), LVGL partial render, PWM backlight, debounced side buttons
- `include/board_m5stickc_plus.h` — M5StickC Plus: ST7789 over SPI (LovyanGFX), AXP192 power + LDO2 backlight, LVGL partial render
- `include/board_m5sticks3.h` — M5StickS3: M5PM1 power-on (L3B), ST7789P3 over SPI (LovyanGFX), PWM backlight, buttons with long press
- `include/board_crowpanel.h` — display (esp_lcd RGB, single PSRAM framebuffer + LVGL partial render), GT911 touch, backlight (V1.0 TCA9534 or V1.2+ ctrl MCU @0x30)
- `include/board_waveshare_p4_5.h` — MIPI-DSI HX8394 panel, rotated flush, GT911 touch with reset, LEDC backlight
- `include/glucose.h` — ring buffer, thresholds, trend calc, windowing, dummy source
- `include/ha_source.h` — NTP, HA REST polling (readings, history backfill, brightness setting)
- `include/wifi_mgr.h` — WiFi connection + NVS-saved credentials, scan/connect requests from the UI
- `include/wg_tunnel.h` — optional WireGuard tunnel (Waveshare, M5StickC Plus and M5StickS3 builds)
- `include/wifi_portal.h` — captive-portal WiFi setup: SoftAP + catch-all DNS + setup page (M5StickS3 build)
- `include/ui.h`      — big-number and graph screens, scaled by `S()` for the panel size
- `include/ui_wifi.h` — WiFi setup screen (Waveshare build)
- `src/font_glucose_200.c` / `_360.c` / `_72.c` / `_112.c` / `_90.c` — Montserrat-Medium digits (`lv_font_conv`): CrowPanel main, Waveshare main, Waveshare graph header, T-Display-S3 main; `_90.c` M5StickC Plus / StickS3 main
- `src/font_status_icons.c` — wifi / lock / home glyphs from LVGL's FontAwesome5 file (`lv_font_conv --size 22 -r 0xF1EB,0xF023,0xF015`)
- `partitions_p4.csv` — Waveshare partition table (8 MB app)
- `boards/`, `partitions.csv`, `sdkconfig.defaults*` — from Elecrow's official V1.3-1.5 PlatformIO example

## Support

If this saved you some time, you can [buy me a coffee on Ko-fi](https://ko-fi.com/mobreadmeo).

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/mobreadmeo)
