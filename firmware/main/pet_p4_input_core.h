/*
 * [Input] Raw physical samples and elapsed sampling time.
 * [Output] Public state and APIs for button, legacy rotary, and four-direction
 *          joystick decoding with center-aware directional thresholds and no
 *          ESP-IDF dependencies.
 * [Pos] Testable input-decoder contract shared by firmware and host tests.
 * [Sync] If this file changes, update `pet_p4_input_core.c`, host tests, and
 *        `firmware/.folder.md`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PET_P4_BUTTON_EVENT_NONE = 0,
  PET_P4_BUTTON_EVENT_DOWN = 1 << 0,
  PET_P4_BUTTON_EVENT_UP = 1 << 1,
  PET_P4_BUTTON_EVENT_SHORT_PRESS = 1 << 2,
  PET_P4_BUTTON_EVENT_LONG_PRESS = 1 << 3,
  PET_P4_BUTTON_EVENT_LONG_RELEASE = 1 << 4,
};

typedef struct {
  bool raw_pressed;
  bool stable_pressed;
  bool long_press_emitted;
  uint32_t raw_stable_ms;
  uint32_t held_ms;
  uint32_t debounce_ms;
  uint32_t long_press_ms;
} pet_p4_button_decoder_t;

typedef enum {
  PET_P4_ROTARY_COUNTER_CLOCKWISE = -1,
  PET_P4_ROTARY_NONE = 0,
  PET_P4_ROTARY_CLOCKWISE = 1,
} pet_p4_rotary_direction_t;

typedef struct {
  int previous_state;
  int accumulated_steps;
  int trigger_steps;
} pet_p4_rotary_decoder_t;

typedef enum {
  PET_P4_JOYSTICK_CENTER = 0,
  PET_P4_JOYSTICK_UP,
  PET_P4_JOYSTICK_DOWN,
  PET_P4_JOYSTICK_LEFT,
  PET_P4_JOYSTICK_RIGHT,
} pet_p4_joystick_direction_t;

typedef struct {
  int center_x;
  int center_y;
  int activation_left;
  int activation_right;
  int activation_up;
  int activation_down;
  int release_left;
  int release_right;
  int release_up;
  int release_down;
  uint32_t repeat_delay_ms;
  uint32_t repeat_interval_ms;
  uint32_t held_ms;
  uint32_t next_repeat_ms;
  pet_p4_joystick_direction_t direction;
} pet_p4_joystick_decoder_t;

void pet_p4_button_decoder_init(
  pet_p4_button_decoder_t *decoder,
  bool pressed,
  uint32_t debounce_ms,
  uint32_t long_press_ms
);
uint8_t pet_p4_button_decoder_update(
  pet_p4_button_decoder_t *decoder,
  bool pressed,
  uint32_t elapsed_ms
);
void pet_p4_rotary_decoder_init(
  pet_p4_rotary_decoder_t *decoder,
  int a_level,
  int b_level,
  int trigger_steps
);
pet_p4_rotary_direction_t pet_p4_rotary_decoder_update(
  pet_p4_rotary_decoder_t *decoder,
  int a_level,
  int b_level
);
void pet_p4_joystick_decoder_init(
  pet_p4_joystick_decoder_t *decoder,
  int center_x,
  int center_y,
  int activation_delta,
  int release_delta,
  uint32_t repeat_delay_ms,
  uint32_t repeat_interval_ms
);
pet_p4_joystick_direction_t pet_p4_joystick_decoder_update(
  pet_p4_joystick_decoder_t *decoder,
  int x,
  int y,
  uint32_t elapsed_ms
);

#ifdef __cplusplus
}
#endif
