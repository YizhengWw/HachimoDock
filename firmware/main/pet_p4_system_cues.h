#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot caller only, before audio workers; failed initialization is latched. */
bool pet_p4_system_cues_init(void);
bool pet_p4_system_cue_pcm(const char *family, const uint8_t **pcm, size_t *size);
bool pet_p4_system_cue_family(const char *family);
bool pet_p4_system_cue_legacy(const char *family, uint64_t checksum);
bool pet_p4_cue_parse_wav(const uint8_t *wav, size_t size, const uint8_t **pcm, size_t *pcm_size);
