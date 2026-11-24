#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#if defined(BOARD_WAVESHARE_P4_5)
// plenty of PSRAM: use the system heap (large allocations spill to PSRAM) instead of a fixed pool
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#elif defined(BOARD_M5STICKC_PLUS)
// no PSRAM and a single screen: a small pool leaves internal RAM for WiFi + WireGuard
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (40 * 1024U)
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (96 * 1024U)
#endif
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
#if defined(BOARD_WAVESHARE_P4_5)   // 1280x720 UI uses 1.5x font sizes
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_FMT_TXT_LARGE 1   // 360px digit font exceeds the default glyph-table limits
#endif
#define LV_FONT_DEFAULT &lv_font_montserrat_20

#define LV_USE_CHART 1
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_LINE 1
#define LV_USE_FLEX 1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif
