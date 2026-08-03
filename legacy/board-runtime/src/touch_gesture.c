#include "touch_gesture.h"

#include <string.h>

void br_touch_gesture_init(br_touch_gesture_state *state, int swipe_threshold, int long_press_ms) {
  if (!state) {
    return;
  }
  memset(state, 0, sizeof(*state));
  state->swipe_threshold = swipe_threshold > 0 ? swipe_threshold : 40;
  state->long_press_ms = long_press_ms > 0 ? long_press_ms : 5000;
}

void br_touch_gesture_set_position(br_touch_gesture_state *state, int x, int y) {
  if (!state) {
    return;
  }
  state->current_x = x;
  state->current_y = y;
}

void br_touch_gesture_start(br_touch_gesture_state *state, long long now_ms) {
  if (!state) {
    return;
  }
  state->touch_down = true;
  state->start_x = state->current_x;
  state->start_y = state->current_y;
  state->start_ms = now_ms;
  state->long_press_emitted = false;
}

bool br_touch_gesture_sync(br_touch_gesture_state *state, long long now_ms, br_touch_action *action) {
  int duration_ms;
  if (!state || !action || !state->touch_down || state->long_press_emitted) {
    return false;
  }
  duration_ms = (int) (now_ms - state->start_ms);
  if (duration_ms < state->long_press_ms) {
    return false;
  }
  memset(action, 0, sizeof(*action));
  action->type = BR_TOUCH_LONG_PRESS;
  action->duration_ms = duration_ms;
  action->x = state->current_x;
  action->y = state->current_y;
  state->long_press_emitted = true;
  return true;
}

bool br_touch_gesture_finish(br_touch_gesture_state *state, long long now_ms, br_touch_action *action) {
  int duration_ms;
  int dx;
  int dy;

  if (!state || !action || !state->touch_down) {
    return false;
  }
  state->touch_down = false;
  duration_ms = (int) (now_ms - state->start_ms);
  dx = state->current_x - state->start_x;
  dy = state->current_y - state->start_y;

  memset(action, 0, sizeof(*action));
  action->duration_ms = duration_ms;
  action->x = state->current_x;
  action->y = state->current_y;
  action->dx = dx;
  action->dy = dy;

  if (state->long_press_emitted || duration_ms >= state->long_press_ms) {
    action->type = BR_TOUCH_LONG_PRESS;
    return true;
  }
  if (dx >= state->swipe_threshold && dx > (dy >= 0 ? dy : -dy)) {
    action->type = BR_TOUCH_SWIPE_RIGHT;
    return true;
  }
  if (-dx >= state->swipe_threshold && -dx > (dy >= 0 ? dy : -dy)) {
    action->type = BR_TOUCH_SWIPE_LEFT;
    return true;
  }
  if (dy >= state->swipe_threshold && dy > (dx >= 0 ? dx : -dx)) {
    action->type = BR_TOUCH_SWIPE_DOWN;
    return true;
  }
  if (-dy >= state->swipe_threshold && -dy > (dx >= 0 ? dx : -dx)) {
    action->type = BR_TOUCH_SWIPE_UP;
    return true;
  }
  action->type = BR_TOUCH_TAP;
  return true;
}

const char *br_touch_action_type_name(br_touch_action_type type) {
  switch (type) {
    case BR_TOUCH_TAP: return "tap";
    case BR_TOUCH_LONG_PRESS: return "long_press";
    case BR_TOUCH_SWIPE_LEFT: return "swipe_left";
    case BR_TOUCH_SWIPE_RIGHT: return "swipe_right";
    case BR_TOUCH_SWIPE_UP: return "swipe_up";
    case BR_TOUCH_SWIPE_DOWN: return "swipe_down";
    default: return "";
  }
}
