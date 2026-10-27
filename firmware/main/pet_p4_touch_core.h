#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PET_P4_TOUCH_NONE = 0,
  PET_P4_TOUCH_TAP,
  PET_P4_TOUCH_LONG_PRESS,
  PET_P4_TOUCH_SWIPE_LEFT,
  PET_P4_TOUCH_SWIPE_RIGHT,
  PET_P4_TOUCH_SWIPE_UP,
  PET_P4_TOUCH_SWIPE_DOWN,
} pet_p4_touch_gesture_t;

typedef struct {
  bool down;
  bool long_press_emitted;
  bool moved;
  int start_x;
  int start_y;
  int current_x;
  int current_y;
  uint64_t start_ms;
  int swipe_threshold;
  uint64_t long_press_ms;
} pet_p4_touch_decoder_t;

typedef struct {
  pet_p4_touch_gesture_t gesture;
  int x;
  int y;
  int dx;
  int dy;
  uint64_t duration_ms;
  uint64_t ts_ms;
} pet_p4_touch_event_t;

/* Continuous, single-contact input. DOWN never implies a cut; a lost sample
 * cancels the stroke instead of connecting positions across an unknown gap. */
typedef enum {
  PET_P4_POINTER_DOWN, PET_P4_POINTER_MOVE, PET_P4_POINTER_UP, PET_P4_POINTER_CANCEL
} pet_p4_pointer_kind_t;
typedef struct {
  pet_p4_pointer_kind_t kind;
  int x, y;
  uint64_t ts_ms;
} pet_p4_pointer_event_t;
typedef struct {
  bool down;
  int x, y;
  uint64_t last_ms;
} pet_p4_pointer_decoder_t;
typedef struct { unsigned count; int x, y, track_id; } pet_p4_ft6336_frame_t;
/* FT6336U / CST826 register 0x02 followed by two six-byte point records.
 * Controller identity must be verified separately before using this decoder. */
bool pet_p4_ft6336_decode(const uint8_t *raw, size_t len, pet_p4_ft6336_frame_t *out);
bool pet_p4_pointer_update(pet_p4_pointer_decoder_t *decoder, bool valid,
  bool pressed, int x, int y, uint64_t now_ms, pet_p4_pointer_event_t *event);

void pet_p4_touch_decoder_init(
  pet_p4_touch_decoder_t *decoder,
  int swipe_threshold,
  uint64_t long_press_ms
);
bool pet_p4_touch_decoder_update(
  pet_p4_touch_decoder_t *decoder,
  bool pressed,
  int x,
  int y,
  uint64_t now_ms,
  pet_p4_touch_event_t *event
);
void pet_p4_touch_panel_to_ui(int panel_x, int panel_y, int *ui_x, int *ui_y);
/* Installed CST826 panel's sensor axes are 180 degrees from LCD native axes. */
void pet_p4_touch_cst826_to_ui(int panel_x, int panel_y, int *ui_x, int *ui_y);
const char *pet_p4_touch_gesture_name(pet_p4_touch_gesture_t gesture);

#ifdef __cplusplus
}
#endif
