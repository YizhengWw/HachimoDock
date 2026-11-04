/* MiSans Medium: retain PR12 license/source header. Native32 is opt-in only. */
#pragma once
#include "pet_p4_font_bitmap.h"
#include "pet_p4_font_metrics_columns_config.h"
#if PET_FONT_NATIVE_32
#include "lvgl.h"
#include "miniz.h"
#include "pet_p4_font_native32_size.h"
extern const lv_font_t pet_p4_font_cn_32;
bool pet_p4_font_native32_prepare(tinfl_decompressor *shared);
void pet_p4_font_native32_clear(void);
bool pet_p4_font_native32_has_glyph(uint32_t codepoint);
#endif
