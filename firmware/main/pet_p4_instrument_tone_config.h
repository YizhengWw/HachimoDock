/* Native candidate data representation only; never an audio/runtime feature. */
#pragma once
#ifndef PET_INSTRUMENT_TONE_PACKED
#define PET_INSTRUMENT_TONE_PACKED 0
#endif
#if PET_INSTRUMENT_TONE_PACKED && !(defined(PET_FONT_BITMAP_PACKED) && PET_FONT_BITMAP_PACKED)
#error "Packed instrument tone requires the packed-font startup initialization parent"
#endif
