// Small helpers for glucose-amoled.yaml (pulled in with esphome: includes:).
#pragma once
#include <cmath>
#include <string>

// Material Design Icons glyphs (UTF-8), see the icons_* fonts in the YAML
#define MDI_UP          "\U000F005D"
#define MDI_DOWN        "\U000F0045"
#define MDI_RIGHT       "\U000F0054"
#define MDI_UP_RIGHT    "\U000F005C"
#define MDI_DOWN_RIGHT  "\U000F0043"
#define MDI_HELP        "\U000F02D6"

// Dexcom ("rising_quickly" ...) and Medtronic CareLink ("UP_DOUBLE" ...) trend states -> arrow.
// CareLink's 1/2/3 arrows line up with Dexcom's 45-degree / single / double arrows.
static std::string trend_glyph(const std::string &t) {
  if (t == "rising_quickly" || t == "UP_TRIPLE") return MDI_UP MDI_UP;
  if (t == "rising" || t == "UP_DOUBLE") return MDI_UP;
  if (t == "rising_slightly" || t == "UP") return MDI_UP_RIGHT;
  if (t == "steady" || t == "NONE" || t == "FLAT") return MDI_RIGHT;
  if (t == "falling_slightly" || t == "DOWN") return MDI_DOWN_RIGHT;
  if (t == "falling" || t == "DOWN_DOUBLE") return MDI_DOWN;
  if (t == "falling_quickly" || t == "DOWN_TRIPLE") return MDI_DOWN MDI_DOWN;
  return MDI_HELP;
}

static std::string glucose_text(float v) {
  if (std::isnan(v)) return "--";
  return std::to_string((int) lroundf(v));
}

// red below the low limit, amber above the high limit, green in range, grey without a reading
static uint32_t glucose_rgb(float v, float lo, float hi) {
  if (std::isnan(v)) return 0x5A5A5A;
  if (std::isnan(lo)) lo = 70;
  if (std::isnan(hi)) hi = 200;
  if (v < lo) return 0xFF4D4D;
  if (v > hi) return 0xFFB020;
  return 0x3FD27A;
}
