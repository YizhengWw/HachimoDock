/* Bounded vector punctuation missing from the compact CJK font. Coordinates
 * and advances use its 16 px em; coverage is evaluated at output resolution. */
#pragma once
#include <math.h>
#include <stdint.h>

static inline int pet_app_symbol_advance(uint32_t cp) {
  switch (cp) {
    case 0x00b7: return 6;
    case 0x00b0: return 8;
    case 0x2103: return 18;
    case 0x2013: case 0x2212: return 10;
    case 0x2014: case 0x2026: return 16;
    default: return 0;
  }
}
static inline void pet_app_symbol_bounds(uint32_t cp, int *top, int *bottom) {
  *top = 7; *bottom = 9;
  if (cp == 0x00b0 || cp == 0x2103) { *top = 2; *bottom = cp == 0x2103 ? 14 : 6; }
  if (cp == 0x2026) { *top = 12; *bottom = 14; }
}
static inline float pet_app_symbol_coverage(uint32_t cp, float x, float y, float scale) {
  float distance;
  if (cp == 0x00b0 || cp == 0x2103) {
    distance = fabsf(hypotf(x - 4, y - 4) - 1.8f) - .65f;
  } else if (cp == 0x00b7 || cp == 0x2026) {
    float cx = cp == 0x00b7 ? 3 : x < 5.5f ? 3 : x < 10.5f ? 8 : 13;
    distance = hypotf(x - cx, y - (cp == 0x00b7 ? 8 : 13)) - 1;
  } else {
    float end = cp == 0x2014 ? 15 : 9;
    distance = hypotf(x - fminf(end, fmaxf(1, x)), y - 8) - .65f;
  }
  return fminf(1, fmaxf(0, .5f - distance * scale));
}
