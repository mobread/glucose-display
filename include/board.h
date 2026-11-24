// Board selection. Each board header provides namespace board { init(); set_brightness(pct); }
// and defines UI_W / UI_H (logical landscape resolution the UI is laid out for).
#pragma once

#if defined(BOARD_WAVESHARE_P4_5)
#include "board_waveshare_p4_5.h"
#elif defined(BOARD_TDISPLAY_S3)
#include "board_tdisplay_s3.h"
#elif defined(BOARD_M5STICKC_PLUS)
#include "board_m5stickc_plus.h"
#else
#include "board_crowpanel.h"
#endif
