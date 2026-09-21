#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* P4 codec adapter for ESP-SR MR (microphone + actual speaker reference).
 * Architecture follows DeskBot V2's audio frontend; no PDM/Arduino dependency.
 * All public PCM frames are 320 samples / 20 ms / mono / 16 kHz. */
bool pet_p4_afe_init(void);
bool pet_p4_afe_ready(void);
void pet_p4_afe_reset_stream(void);
bool pet_p4_afe_feed(const int16_t *mic);
bool pet_p4_afe_take(int16_t *pcm, bool *speech);
void pet_p4_afe_reference(const int16_t *pcm, size_t samples);
void pet_p4_afe_reference_abort(void);
