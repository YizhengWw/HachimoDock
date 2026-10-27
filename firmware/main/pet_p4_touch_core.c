#include "pet_p4_touch_core.h"

#include <stdlib.h>
#include <string.h>

#define PET_P4_PANEL_WIDTH 480
#define PET_P4_PANEL_HEIGHT 640

bool pet_p4_ft6336_decode(const uint8_t *raw, size_t len, pet_p4_ft6336_frame_t *out) {
  if (!raw || !out || len < 13) return false;
  memset(out, 0, sizeof(*out));
  unsigned count = raw[0] & 15;
  if (count > 2) return false;
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *p = raw + 1 + i * 6;
    unsigned event = p[0] >> 6;
    if (event == 1) continue;
    if (event == 3) return false;
    int x = ((p[0] & 15) << 8) | p[1], y = ((p[2] & 15) << 8) | p[3];
    if (x >= PET_P4_PANEL_WIDTH || y >= PET_P4_PANEL_HEIGHT) return false;
    out->count++;
    out->x = x; out->y = y; out->track_id = p[2] >> 4;
  }
  return true;
}

static int clamp_int(int value, int min_value, int max_value) {
  if (value < min_value) return min_value;
  if (value > max_value) return max_value;
  return value;
}

void pet_p4_touch_decoder_init(
  pet_p4_touch_decoder_t *decoder,
  int swipe_threshold,
  uint64_t long_press_ms
) {
  if (!decoder) return;
  memset(decoder, 0, sizeof(*decoder));
  decoder->swipe_threshold = swipe_threshold > 0 ? swipe_threshold : 40;
  decoder->long_press_ms = long_press_ms > 0 ? long_press_ms : 700;
}

static void fill_event(
  const pet_p4_touch_decoder_t *decoder,
  pet_p4_touch_event_t *event,
  pet_p4_touch_gesture_t gesture,
  uint64_t now_ms
) {
  memset(event, 0, sizeof(*event));
  event->gesture = gesture;
  event->x = decoder->current_x;
  event->y = decoder->current_y;
  event->dx = decoder->current_x - decoder->start_x;
  event->dy = decoder->current_y - decoder->start_y;
  event->duration_ms = now_ms - decoder->start_ms;
  event->ts_ms = now_ms;
}

bool pet_p4_touch_decoder_update(
  pet_p4_touch_decoder_t *decoder,
  bool pressed,
  int x,
  int y,
  uint64_t now_ms,
  pet_p4_touch_event_t *event
) {
  if (!decoder || !event) return false;
  if (pressed) {
    decoder->current_x = x;
    decoder->current_y = y;
    if (!decoder->down) {
      decoder->down = true;
      decoder->long_press_emitted = false;
      decoder->moved = false;
      decoder->start_x = x;
      decoder->start_y = y;
      decoder->start_ms = now_ms;
      return false;
    }
    if (abs(x - decoder->start_x) > 12 || abs(y - decoder->start_y) > 12) decoder->moved = true;
    if (!decoder->moved && !decoder->long_press_emitted
        && now_ms >= decoder->start_ms && now_ms - decoder->start_ms >= decoder->long_press_ms) {
      decoder->long_press_emitted = true;
      fill_event(decoder, event, PET_P4_TOUCH_LONG_PRESS, now_ms);
      return true;
    }
    return false;
  }

  if (!decoder->down) return false;
  decoder->down = false;
  if (decoder->long_press_emitted) return false;

  int dx = decoder->current_x - decoder->start_x;
  int dy = decoder->current_y - decoder->start_y;
  int abs_x = abs(dx);
  int abs_y = abs(dy);
  pet_p4_touch_gesture_t gesture = PET_P4_TOUCH_TAP;
  if (abs_x >= decoder->swipe_threshold && abs_x >= abs_y) {
    gesture = dx > 0 ? PET_P4_TOUCH_SWIPE_RIGHT : PET_P4_TOUCH_SWIPE_LEFT;
  } else if (abs_y >= decoder->swipe_threshold && abs_y > abs_x) {
    gesture = dy > 0 ? PET_P4_TOUCH_SWIPE_DOWN : PET_P4_TOUCH_SWIPE_UP;
  }
  if (gesture == PET_P4_TOUCH_TAP && decoder->moved) return false;
  fill_event(decoder, event, gesture, now_ms);
  return true;
}

void pet_p4_touch_panel_to_ui(int panel_x, int panel_y, int *ui_x, int *ui_y) {
  panel_x = clamp_int(panel_x, 0, PET_P4_PANEL_WIDTH - 1);
  panel_y = clamp_int(panel_y, 0, PET_P4_PANEL_HEIGHT - 1);
  if (ui_x) *ui_x = panel_y;
  if (ui_y) *ui_y = PET_P4_PANEL_WIDTH - 1 - panel_x;
}

const char *pet_p4_touch_gesture_name(pet_p4_touch_gesture_t gesture) {
  switch (gesture) {
    case PET_P4_TOUCH_TAP: return "tap";
    case PET_P4_TOUCH_LONG_PRESS: return "long_press";
    case PET_P4_TOUCH_SWIPE_LEFT: return "swipe_left";
    case PET_P4_TOUCH_SWIPE_RIGHT: return "swipe_right";
    case PET_P4_TOUCH_SWIPE_UP: return "swipe_up";
    case PET_P4_TOUCH_SWIPE_DOWN: return "swipe_down";
    default: return "";
  }
}

void pet_p4_touch_cst826_to_ui(int panel_x, int panel_y, int *ui_x, int *ui_y) {
  int x, y;
  pet_p4_touch_panel_to_ui(panel_x, panel_y, &x, &y);
  if (ui_x) *ui_x = 639 - x;
  if (ui_y) *ui_y = 479 - y;
}

bool pet_p4_pointer_update(pet_p4_pointer_decoder_t *d, bool valid,
  bool pressed, int x, int y, uint64_t now_ms, pet_p4_pointer_event_t *e) {
  if (!d || !e) return false;
  if (!valid || now_ms < d->last_ms || (d->down && now_ms - d->last_ms > 150)) {
    bool down = d->down;
    *e = (pet_p4_pointer_event_t){PET_P4_POINTER_CANCEL, d->x, d->y, now_ms};
    *d = (pet_p4_pointer_decoder_t){.last_ms = now_ms};
    return down;
  }
  x = clamp_int(x, 0, 639); y = clamp_int(y, 0, 479);
  d->last_ms = now_ms;
  if (!pressed) {
    if (!d->down) return false;
    d->down = false;
    *e = (pet_p4_pointer_event_t){PET_P4_POINTER_UP, d->x, d->y, now_ms};
    return true;
  }
  *e = (pet_p4_pointer_event_t){d->down ? PET_P4_POINTER_MOVE : PET_P4_POINTER_DOWN, x, y, now_ms};
  d->down = true; d->x = x; d->y = y;
  /* Stationary samples are heartbeats too: distinguish a held finger from a
   * stalled controller without synthesizing motion or a cut. */
  return true;
}
