/* Master-control scope only. Ordinary codec/capture transactions retain 100 ms. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
bool pet_audio_volume_deadline_begin(uint32_t budget_us);
void pet_audio_volume_deadline_end(void);
bool pet_audio_volume_deadline_active(void);
/* 0 means expired: the caller MUST NOT enter the I2C driver. */
int pet_audio_volume_i2c_timeout_ms(int ordinary_ms);
/* Link-time proof that the hash-pinned generated wrapper was integrated. */
extern const unsigned pet_audio_volume_codec_patch_v1;
