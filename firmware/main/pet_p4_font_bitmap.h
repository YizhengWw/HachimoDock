/* Startup-only native asset preparation, never component-supplied data.
 * A failed init latches failure and must prevent rendering/OTA validation.
 * No glyph-time work, filesystem access, task, allocation or audio dependency. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_FONT_BITMAP_PACKED
#define PET_FONT_BITMAP_PACKED 0
#endif
#ifndef PET_FONT_METRICS_PACKED
#define PET_FONT_METRICS_PACKED 0
#endif
#include "pet_p4_instrument_tone_config.h"
#if PET_FONT_METRICS_PACKED && !PET_FONT_BITMAP_PACKED
#error "Packed metrics require the reviewed packed-bitmap startup parent"
#endif
#if PET_FONT_BITMAP_PACKED
#include "pet_p4_font_bitmap_size.h"
extern uint8_t pet_font_bitmap[PET_FONT_BITMAP_BYTES];
#endif
#if PET_FONT_METRICS_PACKED
#include "lvgl.h"
#include "pet_p4_font_metrics_size.h"
#if LV_FONT_FMT_TXT_LARGE != 0
#error "Review large LVGL glyph descriptor representation before enabling metrics"
#endif
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t)==8,"review LVGL metric ABI");
extern lv_font_fmt_txt_glyph_dsc_t pet_font_metrics[PET_FONT_METRIC_RECORDS];
extern uint8_t pet_font_kern_left[PET_FONT_METRIC_RECORDS];
extern uint8_t pet_font_kern_right[PET_FONT_METRIC_RECORDS];
#endif
bool pet_p4_font_bitmap_init(void);
