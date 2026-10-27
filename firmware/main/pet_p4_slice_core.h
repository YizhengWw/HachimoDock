#pragma once
/* Phase-one native diagnostic, not a published PetUI package engine. */
#include <stdbool.h>
#include <stdint.h>
#include "pet_p4_touch_core.h"
#define PET_P4_SLICE_TRAIL_MAX 24
typedef struct { int x, y; uint64_t ts_ms; bool start; } pet_p4_slice_point_t;
typedef struct {
  bool running, sliced, pointer_down, finished;
  unsigned score, misses;
  float x, y, pointer_fruit_x, pointer_fruit_y, vx, vy;
  int pointer_x, pointer_y;
  uint64_t now_ms, launch_ms, cut_ms, stroke_ms;
  unsigned trail_count;
  pet_p4_slice_point_t trail[PET_P4_SLICE_TRAIL_MAX];
} pet_p4_slice_t;
void pet_p4_slice_start(pet_p4_slice_t *s, uint64_t now_ms);
void pet_p4_slice_tick(pet_p4_slice_t *s, uint64_t now_ms);
bool pet_p4_slice_pointer(pet_p4_slice_t *s, const pet_p4_pointer_event_t *e);
