/* Main-page compact overlay underlay. Render-task owned; no worker or IO. */
#ifndef PET_P4_COMPACT_UNDERLAY_H
#define PET_P4_COMPACT_UNDERLAY_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
  PET_COMPACT_FRAME_WIDTH = 640,
  PET_COMPACT_ROI_X = 20,
  PET_COMPACT_ROI_Y = 404,
  PET_COMPACT_ROI_WIDTH = 422, /* max 420px panel plus 2px shadow */
  PET_COMPACT_ROI_HEIGHT = 60 /* 56px panel plus 4px shadow */
};
typedef struct {
  uint16_t pixels[PET_COMPACT_ROI_WIDTH * PET_COMPACT_ROI_HEIGHT];
  unsigned width;
  bool valid;
  bool painted;
} pet_compact_underlay_t;

static inline void pet_compact_underlay_forget(pet_compact_underlay_t *cache) {
  cache->valid = false;
  cache->painted = false;
  cache->width = 0;
}
static inline void pet_compact_underlay_restore(pet_compact_underlay_t *cache, uint16_t *frame) {
  if (!cache->valid || !cache->painted) return;
  for (unsigned y = 0; y < PET_COMPACT_ROI_HEIGHT; ++y) {
    memcpy(frame + (PET_COMPACT_ROI_Y + y) * PET_COMPACT_FRAME_WIDTH + PET_COMPACT_ROI_X,
      cache->pixels + y * PET_COMPACT_ROI_WIDTH, cache->width * sizeof(uint16_t));
  }
  cache->painted = false;
}
static inline bool pet_compact_underlay_begin(pet_compact_underlay_t *cache,
    const uint16_t *frame, unsigned width, bool fresh_asset) {
  if (!width || width > PET_COMPACT_ROI_WIDTH || (!fresh_asset && !cache->valid)) return false;
  /* The previous compact ROI was restored before this frame's overlays. Never
   * capture an unrelated card/page as the appearance underlay. */
  for (unsigned y = 0; y < PET_COMPACT_ROI_HEIGHT; ++y) {
    memcpy(cache->pixels + y * PET_COMPACT_ROI_WIDTH,
      frame + (PET_COMPACT_ROI_Y + y) * PET_COMPACT_FRAME_WIDTH + PET_COMPACT_ROI_X,
      width * sizeof(uint16_t));
  }
  cache->width = width;
  cache->valid = true;
  cache->painted = true;
  return true;
}
#endif
