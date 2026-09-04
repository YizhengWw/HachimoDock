#include "pet_p4_touch_core.h"

#include <stdlib.h>
#include <string.h>

#define PET_P4_PANEL_WIDTH 480
#define PET_P4_PANEL_HEIGHT 640

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
      decoder->start_x = x;
      decoder->start_y = y;
      decoder->start_ms = now_ms;
      return false;
    }
    if (!decoder->long_press_emitted && now_ms - decoder->start_ms >= decoder->long_press_ms) {
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
  if (abs_x >= decoder->swipe_threshold && abs_x > abs_y) {
    gesture = dx > 0 ? PET_P4_TOUCH_SWIPE_RIGHT : PET_P4_TOUCH_SWIPE_LEFT;
  } else if (abs_y >= decoder->swipe_threshold && abs_y > abs_x) {
    gesture = dy > 0 ? PET_P4_TOUCH_SWIPE_DOWN : PET_P4_TOUCH_SWIPE_UP;
  }
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
