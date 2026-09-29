#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PET_INSTRUMENT_DEFAULT_TONE 0u /* User-selected A: crisp, short wooden knock. */
/* Portable DSP, owned exclusively by the speaker worker. No allocation or I/O. */
typedef struct {
  uint32_t samples, noise;
  uint32_t age[4];
  unsigned next;
  float gain;
} pet_instrument_core_t;
typedef struct { float mallet_y, fish_scale, wave; } pet_instrument_motion_t;
void pet_instrument_reset(pet_instrument_core_t *s);
void pet_instrument_strike(pet_instrument_core_t *s);
void pet_instrument_render(pet_instrument_core_t *s, int16_t *pcm, size_t count,
                          bool enabled, unsigned effect, unsigned ambience);
/* Audition three bounded modal tunings; tone 0 is the current crisp default. */
void pet_instrument_render_tone(pet_instrument_core_t *s, int16_t *pcm, size_t count,
                               bool enabled, unsigned effect, unsigned ambience, unsigned tone);
pet_instrument_motion_t pet_instrument_motion(uint64_t elapsed_ms);
pet_instrument_motion_t pet_instrument_motion_from(uint64_t elapsed_ms, float start_y, float start_scale);
