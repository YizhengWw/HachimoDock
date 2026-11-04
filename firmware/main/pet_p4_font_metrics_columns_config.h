/* Include before any packed-font conditionals. No default behavior change. */
#pragma once
#ifndef PET_FONT_NATIVE_32
#define PET_FONT_NATIVE_32 0
#endif
#if PET_FONT_NATIVE_32 && (!PET_FONT_BITMAP_PACKED || !PET_FONT_METRICS_PACKED)
#error "PET_FONT_NATIVE_32 requires PET_FONT_BITMAP_PACKED and PET_FONT_METRICS_PACKED"
#endif
