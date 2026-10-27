#include "pet_p4_slice_core.h"
#include <string.h>

static void launch(pet_p4_slice_t *s, uint64_t now) {
  s->x = 260 + (s->score + s->misses) % 3 * 60;
  s->y = 460;
  s->pointer_down = false; /* Never join a segment across target respawns. */
  s->vx = ((s->score + s->misses) % 2) ? -35 : 35;
  s->vy = -470; s->sliced = false; s->launch_ms = now;
}
void pet_p4_slice_start(pet_p4_slice_t *s, uint64_t now) {
  if (!s) return;
  memset(s, 0, sizeof(*s)); s->running = true; s->now_ms = now; launch(s, now);
}
void pet_p4_slice_tick(pet_p4_slice_t *s, uint64_t now) {
  if (!s || now < s->now_ms) return;
  uint64_t elapsed = now - s->now_ms;
  s->now_ms = now;
  if (s->pointer_down && now >= s->stroke_ms && now - s->stroke_ms > 150) s->pointer_down = false;
  unsigned keep = 0;
  for (unsigned i = 0; i < s->trail_count; i++)
    if (now >= s->trail[i].ts_ms && now - s->trail[i].ts_ms <= 180) s->trail[keep++] = s->trail[i];
  s->trail_count = keep;
  if (!s->running) return;
  /* Long stalls cancel the stroke and freeze simulation rather than teleporting
   * a target through a finger. Regular steps are bounded for stable physics. */
  if (elapsed > 150) { s->pointer_down = false; elapsed = 0; }
  while (elapsed) {
    unsigned step = elapsed > 10 ? 10 : (unsigned)elapsed;
    float dt = step / 1000.f;
    s->x += s->vx * dt; s->y += s->vy * dt + 260.f * dt * dt; s->vy += 520.f * dt;
    elapsed -= step;
  }
  if ((s->sliced && now >= s->cut_ms && now - s->cut_ms >= 500) || s->y > 530) {
    if (!s->sliced && ++s->misses >= 3) { s->finished = true; s->running = false; }
    else launch(s, now);
  }
}
static bool segment_hits(float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
  float t = length > 0 ? -(ax * dx + ay * dy) / length : 0;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  float x = ax + t * dx, y = ay + t * dy;
  return x * x + y * y <= 34.f * 34.f;
}
bool pet_p4_slice_pointer(pet_p4_slice_t *s, const pet_p4_pointer_event_t *e) {
  if (!s || !e) return false;
  if (e->x < 0 || e->x > 639 || e->y < 0 || e->y > 479) {
    s->pointer_down = false; return false;
  }
  if (e->kind == PET_P4_POINTER_UP || e->kind == PET_P4_POINTER_CANCEL) {
    s->pointer_down = false; return false;
  }
  if (e->ts_ms < s->stroke_ms) { s->pointer_down = false; return false; }
  bool continuing = s->pointer_down && e->kind == PET_P4_POINTER_MOVE && e->ts_ms - s->stroke_ms <= 150;
  int dx = e->x - s->pointer_x, dy = e->y - s->pointer_y;
  /* Test the stroke relative to the moving fruit, once per fruit. */
  bool cut = continuing && s->running && !s->sliced && dx * dx + dy * dy >= 9
    && segment_hits(s->pointer_x - s->pointer_fruit_x, s->pointer_y - s->pointer_fruit_y,
                    e->x - s->x, e->y - s->y);
  if (cut) { s->sliced = true; s->cut_ms = e->ts_ms; s->score++; }
  if (s->trail_count == PET_P4_SLICE_TRAIL_MAX) {
    memmove(s->trail, s->trail + 1, sizeof(s->trail[0]) * (PET_P4_SLICE_TRAIL_MAX - 1));
    s->trail_count--;
  }
  s->trail[s->trail_count++] = (pet_p4_slice_point_t){e->x, e->y, e->ts_ms, !continuing};
  s->pointer_x = e->x; s->pointer_y = e->y; s->stroke_ms = e->ts_ms; s->pointer_down = true;
  s->pointer_fruit_x = s->x; s->pointer_fruit_y = s->y;
  return cut;
}
