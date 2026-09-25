// Home Assistant data source: WiFi + NTP + REST polling in a background task.
// Readings are handed to the main loop via ha::take().
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "glucose.h"
#include "secrets.h"

namespace ha {

static const uint32_t POLL_MS = 60 * 1000;       // check for a new reading every minute
static const uint32_t HISTORY_HOURS = 24;
static const int MAX_BATCH = 320;

struct Batch {
  Reading r[MAX_BATCH];
  int n = 0;
  bool replace = false;   // true = history backfill (replace buffer), false = append
  Trend trend = TREND_UNKNOWN;
  bool ready = false;
};

static Batch batch;
static SemaphoreHandle_t mtx;
static char status[48] = "";
static uint32_t latest_ts_seen = 0;

static void set_status(const char *s) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  strncpy(status, s, sizeof status - 1);
  xSemaphoreGive(mtx);
}

static void get_status(char *out, size_t n) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  strncpy(out, status, n - 1); out[n - 1] = 0;
  xSemaphoreGive(mtx);
}

// ---------- time helpers ----------
static uint32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)(era * 146097 + (int)doe - 719468);
}

// Parses "2026-09-24T19:30:00.123456+00:00" / "...Z" -> epoch seconds (0 on failure)
static uint32_t parse_iso8601(const char *s) {
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int32_t off = 0;
  const char *p = strpbrk(s + 19, "+-Z");
  if (p && *p != 'Z') {
    int oh = 0, om = 0;
    sscanf(p + 1, "%d:%d", &oh, &om);
    off = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
  }
  return days_from_civil(Y, M, D) * 86400u + h * 3600 + m * 60 + sec - off;
}

static Trend parse_trend(const char *t) {
  if (!t) return TREND_UNKNOWN;
  // HA enum sensor states use underscores (e.g. "falling_slightly")
  if (!strcmp(t, "rising_quickly"))   return TREND_DOUBLE_UP;
  if (!strcmp(t, "rising"))           return TREND_SINGLE_UP;
  if (!strcmp(t, "rising_slightly"))  return TREND_45_UP;
  if (!strcmp(t, "steady"))           return TREND_FLAT;
  if (!strcmp(t, "falling_slightly")) return TREND_45_DOWN;
  if (!strcmp(t, "falling"))          return TREND_SINGLE_DOWN;
  if (!strcmp(t, "falling_quickly"))  return TREND_DOUBLE_DOWN;
  Serial.printf("[ha] unknown trend '%s'\n", t);
  return TREND_UNKNOWN;
}

static bool valid_state(const char *s) {
  return s && *s && strcmp(s, "unknown") && strcmp(s, "unavailable");
}

// ---------- HTTP ----------
static int http_get(const String &path, String &body) {
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(String(HA_BASE_URL) + path);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

static bool fetch_current(Reading &out, Trend &trend) {
  String body;
  int code = http_get(String("/api/states/") + HA_ENTITY_VALUE, body);
  if (code != 200) { Serial.printf("[ha] states value HTTP %d\n", code); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *state = doc["state"];
  if (!valid_state(state)) { Serial.println("[ha] glucose value unavailable"); return false; }
  out.value = atoi(state);
  out.ts = parse_iso8601(doc["last_changed"]);

  body = "";
  if (http_get(String("/api/states/") + HA_ENTITY_TREND, body) == 200 && !deserializeJson(doc, body)) {
    trend = parse_trend(doc["state"]);
  }
  return out.ts != 0;
}

static bool fetch_history(Batch &b) {
  time_t now = time(nullptr);
  time_t start = now - HISTORY_HOURS * 3600;
  struct tm tmv; gmtime_r(&start, &tmv);
  char ts[32];
  strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S+00:00", &tmv);
  String path = String("/api/history/period/") + ts +
                "?filter_entity_id=" + HA_ENTITY_VALUE + "&minimal_response&no_attributes";
  String body;
  int code = http_get(path, body);
  if (code != 200) { Serial.printf("[ha] history HTTP %d\n", code); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) { Serial.println("[ha] history parse failed"); return false; }
  JsonArray entity = doc[0].as<JsonArray>();
  b.n = 0;
  int16_t last_v = -1;
  for (JsonObject st : entity) {
    const char *s = st["state"];
    if (!valid_state(s)) continue;
    uint32_t t = parse_iso8601(st["last_changed"]);
    int16_t v = atoi(s);
    if (!t || v <= 0 || v == last_v) continue;   // HA only logs state changes; dedupe
    last_v = v;
    if (b.n < MAX_BATCH) b.r[b.n++] = {t, v};
  }
  b.replace = true;
  Serial.printf("[ha] history: %d readings\n", b.n);
  return true;   // HTTP + parse ok; n may be 0 if the sensor has no data yet
}

// ---------- background task ----------
static void task(void *) {
  WiFi.persistent(false);   // never write credentials/calibration to NVS: flash writes suspend the LCD feed ISR -> glitches
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem-sleep wakeups cause periodic RGB-panel glitches on the S3; keep the radio steady
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(250));
  Serial.printf("[ha] wifi ip=%s gw=%s dns=%s\n", WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  vTaskDelay(pdMS_TO_TICKS(1500));   // let DHCP/DNS settle before the first request

  set_status("");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  while (time(nullptr) < 1700000000) vTaskDelay(pdMS_TO_TICKS(250));

  bool have_history = false;
  uint32_t last_poll = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      set_status("wifi lost");
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    if (!have_history) {
      set_status("");
      Batch tmp;
      if (fetch_history(tmp)) {
        Reading cur; Trend tr = TREND_UNKNOWN;
        if (fetch_current(cur, tr)) tmp.trend = tr;
        xSemaphoreTake(mtx, portMAX_DELAY);
        batch = tmp; batch.ready = true;
        latest_ts_seen = tmp.n ? tmp.r[tmp.n - 1].ts : 0;
        xSemaphoreGive(mtx);
        have_history = true;
        set_status("");
        last_poll = millis();
      } else {
        set_status("HA unreachable");
        vTaskDelay(pdMS_TO_TICKS(10000));
      }
      continue;
    }
    if (millis() - last_poll >= POLL_MS) {
      last_poll = millis();
      Reading cur; Trend tr = TREND_UNKNOWN;
      if (fetch_current(cur, tr)) {
        set_status("");
        xSemaphoreTake(mtx, portMAX_DELAY);
        if (cur.ts > latest_ts_seen) {
          batch.n = 0; batch.replace = false;
          batch.r[batch.n++] = cur;
          batch.trend = tr; batch.ready = true;
          latest_ts_seen = cur.ts;
          Serial.printf("[ha] new reading %d @ %lu trend=%d\n", cur.value, (unsigned long)cur.ts, (int)tr);
        } else if (batch.trend != tr && !batch.ready) {
          batch.n = 0; batch.replace = false; batch.trend = tr; batch.ready = true;  // trend-only update
        }
        xSemaphoreGive(mtx);
      } else {
        set_status("HA unreachable");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static uint32_t clock_now() { return (uint32_t)time(nullptr); }

static void begin() {
  mtx = xSemaphoreCreateMutex();
  glucose::now_fn = clock_now;
  xTaskCreatePinnedToCore(task, "ha", 12288, nullptr, 1, nullptr, 0);
}

// Called from loop(). Applies any pending batch to the glucose buffer. Returns true if data changed.
static bool take() {
  if (!batch.ready) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (batch.replace) glucose::clear();
  for (int i = 0; i < batch.n; i++) glucose::push(batch.r[i].ts, batch.r[i].value);
  glucose::current_trend = batch.trend != TREND_UNKNOWN ? batch.trend : glucose::compute_trend();
  batch.n = 0; batch.ready = false;
  xSemaphoreGive(mtx);
  return true;
}

}  // namespace ha
