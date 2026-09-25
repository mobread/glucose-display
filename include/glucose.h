// Glucose data model + pluggable source (dummy generator for now, Home Assistant later)
#pragma once

#include <Arduino.h>
#include <stdint.h>

// ---- thresholds (mg/dL) ----
#define GLUCOSE_LOW    70
#define GLUCOSE_HIGH   200
#define GLUCOSE_URGENT_LOW 55
#define CHART_Y_MIN    40
#define CHART_Y_MAX    300
#define READING_INTERVAL_S 300     // Dexcom cadence
#define STALE_AFTER_S  (15 * 60)   // warn if no reading for 15 min

enum Trend {
  TREND_UNKNOWN = 0,
  TREND_DOUBLE_UP, TREND_SINGLE_UP, TREND_45_UP,
  TREND_FLAT,
  TREND_45_DOWN, TREND_SINGLE_DOWN, TREND_DOUBLE_DOWN,
};

struct Reading {
  uint32_t ts;   // seconds (epoch once we have NTP; simulated clock in dummy mode)
  int16_t value; // mg/dL
};

namespace glucose {

static const int MAX_READINGS = 320;  // > 24h at 5 min
static Reading buf[MAX_READINGS];
static int head = 0;   // next write index
static int count = 0;
static Trend current_trend = TREND_UNKNOWN;

static uint32_t (*now_fn)() = nullptr;
static uint32_t now() { return now_fn ? now_fn() : (uint32_t)(millis() / 1000); }

static void push(uint32_t ts, int16_t value) {
  buf[head] = {ts, value};
  head = (head + 1) % MAX_READINGS;
  if (count < MAX_READINGS) count++;
}

static void clear() { head = 0; count = 0; }

// i = 0 is newest
static const Reading *at(int i) {
  if (i < 0 || i >= count) return nullptr;
  int idx = (head - 1 - i + MAX_READINGS) % MAX_READINGS;
  return &buf[idx];
}

static const Reading *latest() { return at(0); }

static Trend trend() { return current_trend; }

// Derive a trend from the last three readings (used when the source doesn't supply one)
static Trend compute_trend() {
  const Reading *a = at(0), *b = at(2);
  if (!a || !b || a->ts == b->ts) return TREND_UNKNOWN;
  float per_min = (float)(a->value - b->value) / ((float)(a->ts - b->ts) / 60.0f);
  if (per_min >= 3)  return TREND_DOUBLE_UP;
  if (per_min >= 2)  return TREND_SINGLE_UP;
  if (per_min >= 1)  return TREND_45_UP;
  if (per_min <= -3) return TREND_DOUBLE_DOWN;
  if (per_min <= -2) return TREND_SINGLE_DOWN;
  if (per_min <= -1) return TREND_45_DOWN;
  return TREND_FLAT;
}

// Fill `out` (len n) with values for a window of `hours` ending now, one slot per 5 min.
// Missing slots are set to `none`.
static void window(int hours, int32_t *out, int n, int32_t none) {
  uint32_t t_now = now();
  uint32_t t0 = t_now - (uint32_t)hours * 3600;
  for (int i = 0; i < n; i++) out[i] = none;
  for (int i = 0; i < count; i++) {
    const Reading *r = at(i);
    if (r->ts < t0) break;  // buffer is newest-first
    int slot = (int)((r->ts - t0 + READING_INTERVAL_S / 2) / READING_INTERVAL_S);
    if (slot >= 0 && slot < n) out[slot] = r->value;
  }
}

// ---------------- Dummy source ----------------
// Simulated clock advances 5 min of "glucose time" every DUMMY_TICK_MS of real time,
// so you can watch the chart move without waiting all day.
namespace dummy {
  static const uint32_t DUMMY_TICK_MS = 10000;
  static uint32_t sim_now = 1700000000;  // arbitrary epoch base
  static uint32_t last_tick_ms = 0;
  static float level = 130, velocity = 0;
  static uint32_t rng = 0xC0FFEE;

  static float frand() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; }

  static int16_t next_value() {
    // mean-reverting random walk with occasional meal spikes / drops
    float r = frand();
    if (r < 0.03f) velocity += 6 + frand() * 8;        // meal
    else if (r < 0.05f) velocity -= 6 + frand() * 6;   // insulin / activity
    velocity += (130 - level) * 0.02f + (frand() - 0.5f) * 2.5f;
    velocity *= 0.85f;
    level += velocity;
    if (level < 45) level = 45;
    if (level > 320) level = 320;
    return (int16_t)(level + 0.5f);
  }

  static uint32_t clock() { return sim_now; }

  static void init() {
    now_fn = clock;
    // seed 24h of history
    for (int i = 288; i >= 0; i--) push(sim_now - (uint32_t)i * READING_INTERVAL_S, next_value());
    current_trend = compute_trend();
    last_tick_ms = millis();
  }

  // returns true if a new reading arrived
  static bool poll() {
    if (millis() - last_tick_ms < DUMMY_TICK_MS) return false;
    last_tick_ms = millis();
    sim_now += READING_INTERVAL_S;
    push(sim_now, next_value());
    current_trend = compute_trend();
    return true;
  }
}

}  // namespace glucose
