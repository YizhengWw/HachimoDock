/* Fixed factory defaults only; never accepts package/host supplied compressed data. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool pet_p4_cue_packed_init(void);
bool pet_p4_cue_packed_pcm(unsigned index, const uint8_t **pcm, size_t *size);
