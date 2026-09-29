#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Stateless per-block IMA decoder; output length is bytes. */
bool pet_p4_media_decode(const uint8_t *data, size_t length, uint8_t *pcm,
                         size_t capacity, size_t *written);
