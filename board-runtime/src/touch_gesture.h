#ifndef BOARD_RUNTIME_TOUCH_GESTURE_H
#define BOARD_RUNTIME_TOUCH_GESTURE_H

#include <stdbool.h>

typedef enum {
  BR_TOUCH_NONE = 0,
  BR_TOUCH_TAP,
  BR_TOUCH_LONG_PRESS,
  BR_TOUCH_SWIPE_LEFT,
  BR_TOUCH_SWIPE_RIGHT,
  BR_TOUCH_SWIPE_UP,
  BR_TOUCH_SWIPE_DOWN
} br_touch_action_type;

typedef struct {
  int swipe_threshold;
  int long_press_ms;
  int current_x;
  int current_y;
  bool touch_down;
  int start_x;
  int start_y;
  long long start_ms;
  bool long_press_emitted;
} br_touch_gesture_state;

typedef struct {
  br_touch_action_type type;
  int duration_ms;
  int x;
  int y;
  int dx;
  int dy;
} br_touch_action;

void br_touch_gesture_init(br_touch_gesture_state *state, int swipe_threshold, int long_press_ms);
void br_touch_gesture_set_position(br_touch_gesture_state *state, int x, int y);
void br_touch_gesture_start(br_touch_gesture_state *state, long long now_ms);
bool br_touch_gesture_sync(br_touch_gesture_state *state, long long now_ms, br_touch_action *action);
bool br_touch_gesture_finish(br_touch_gesture_state *state, long long now_ms, br_touch_action *action);
const char *br_touch_action_type_name(br_touch_action_type type);

#endif
