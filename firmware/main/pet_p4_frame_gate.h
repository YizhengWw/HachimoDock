/* Exact frame deduplication: caller retains two distinct, complete buffers.
 * Only a successful LCD submission makes a reference valid. No hash collisions,
 * extra framebuffer, or changes to input/game/animation ticking are involved. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool pet_p4_frame_unchanged(const void *candidate,
    const void *previous, size_t bytes, bool valid) {
  return valid && candidate && previous && candidate != previous && bytes > 0
    && memcmp(candidate, previous, bytes) == 0;
}
